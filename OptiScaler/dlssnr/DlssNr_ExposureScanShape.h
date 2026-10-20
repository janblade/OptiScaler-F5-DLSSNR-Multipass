#pragma once

// What the exposure scan takes as a candidate, the same on every API (DlssNr_ExposureScan.cpp for D3D12,
// DlssNr_ExposureScan_Dx11.cpp, DlssNr_ExposureScan_Vk.cpp), and how its list reuses slots.
//
// The shape: something a shader writes (a UAV on D3D, a storage buffer or image on Vulkan) that holds a number rather
// than a picture -- a texture of at most 256 texels in a float format of one or two channels, or a buffer of 4 to 128
// bytes (the value read is its first 4 bytes, a float). The reasons for each bound are with the D3D12 filter
// (LooksLikeANumber).
//
// The slots: a candidate that goes away (the game destroyed it, or the scan let go of it) frees its slot rather than
// shifting the list, because each API's readback ring holds values by slot index for a few frames. A slot's generation
// counts its owners, so a value copied for the old owner is never credited to the new one. A full list makes room for a
// newcomer whose shape is likelier to be the exposure, or for any newcomer once a candidate has shown it holds no
// decimal number (Rank, EvictFor, HoldsNoDecimal); on D3D12 one turned away waits for a slot (NextWaiting, Rotate).
//
// Header-only, no D3D or Vulkan types: tests/nr_exposure_scan_shape_smoke.cpp.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace DlssNrExposureScanShape
{
inline constexpr uint64_t kMinBufferBytes = 4; // the float read; a smaller buffer cannot hold one
inline constexpr uint64_t kMaxBufferBytes = 128;
inline constexpr uint64_t kMaxTexels = 256;

inline bool SmallBuffer(uint64_t bytes) { return bytes >= kMinBufferBytes && bytes <= kMaxBufferBytes; }

inline bool SmallTexture(uint64_t width, uint64_t height)
{
    return width > 0 && height > 0 && width * height <= kMaxTexels;
}

// Formats an exposure could be in: floating point (or typeless, read as float), one or two channels -- eye adaptation
// commonly keeps the value and something beside it (the previous value, a target). Wider is a picture; an integer
// format cannot hold a scale. `bytes` is the size of the FIRST channel, the one texel read; a name for the readout.
// Not a candidate when `bytes` is 0.
struct Format
{
    unsigned int bytes = 0;
    const char* name = "";
};

// DXGI_FORMAT values (asserted against dxgiformat.h where used). Typeless: the view decides the type, so the texture
// says nothing more than its channel sizes -- NBA 2K27 hands DLSS a 1x1 R32_TYPELESS exposure (2026-10-14), which a
// float-only list missed. An integer in there reads as a tiny or huge float and stays outside the plausible range.
inline constexpr uint32_t kDxgiR32G32Typeless = 15;
inline constexpr uint32_t kDxgiR32G32Float = 16;
inline constexpr uint32_t kDxgiR16G16Typeless = 33;
inline constexpr uint32_t kDxgiR16G16Float = 34;
inline constexpr uint32_t kDxgiR32Typeless = 39;
inline constexpr uint32_t kDxgiR32Float = 41;
inline constexpr uint32_t kDxgiR16Typeless = 53;
inline constexpr uint32_t kDxgiR16Float = 54;

inline Format DxgiFloatFormat(uint32_t dxgiFormat)
{
    switch (dxgiFormat)
    {
    case kDxgiR32Float:
        return { 4, "R32_FLOAT" };
    case kDxgiR16Float:
        return { 2, "R16_FLOAT" };
    case kDxgiR32G32Float:
        return { 4, "R32G32_FLOAT" };
    case kDxgiR16G16Float:
        return { 2, "R16G16_FLOAT" };
    case kDxgiR32Typeless:
        return { 4, "R32_TYPELESS" };
    case kDxgiR16Typeless:
        return { 2, "R16_TYPELESS" };
    case kDxgiR32G32Typeless:
        return { 4, "R32G32_TYPELESS" };
    case kDxgiR16G16Typeless:
        return { 2, "R16G16_TYPELESS" };
    default:
        return {};
    }
}

// VkFormat values (asserted against vulkan_core.h where used)
inline constexpr uint32_t kVkR16Sfloat = 76;
inline constexpr uint32_t kVkR16G16Sfloat = 83;
inline constexpr uint32_t kVkR32Sfloat = 100;
inline constexpr uint32_t kVkR32G32Sfloat = 103;

inline Format VkFloatFormat(uint32_t vkFormat)
{
    switch (vkFormat)
    {
    case kVkR16Sfloat:
        return { 2, "R16_SFLOAT" };
    case kVkR16G16Sfloat:
        return { 2, "R16G16_SFLOAT" };
    case kVkR32Sfloat:
        return { 4, "R32_SFLOAT" };
    case kVkR32G32Sfloat:
        return { 4, "R32G32_SFLOAT" };
    default:
        return {};
    }
}

// The slot a new candidate takes: the first free one, else a new one at the end while under `cap`; -1 when full.
// `Slot` has `bool alive`.
template <class Slot> int FreeSlot(const std::vector<Slot>& slots, size_t cap)
{
    for (size_t i = 0; i < slots.size(); ++i)
    {
        if (!slots[i].alive)
            return (int) i;
    }

    return slots.size() < cap ? (int) slots.size() : -1;
}

// What an exposure could plausibly be. Outside these it is a flag, a counter, a sentinel or a zeroed buffer nobody has
// written yet -- Nioh 3 offers all four, including one holding 1000000.
inline constexpr float kPlausibleFloor = 1e-6f;
inline constexpr float kPlausibleCeiling = 1e4f;

// A value read as a float that is not a decimal number at all: not zero (a buffer not yet written), and beyond what the
// matcher still reads as a float (DlssNrExposureMatch's NotFloat, 1e-30 to 1e30) -- wider than the plausible range, so
// a value in physical units (a luminance past 1e4 cd/m2 in daylight) is not taken for one. An integer read as a float
// is a denormal or close: Witcher 3's UAV buffers held only values like 65536, 9.18e-41 as a float (2026-10-20). A NaN
// or an infinity is not counted (the reader skips those).
inline constexpr float kNotADecimalBelow = 1e-30f;
inline constexpr float kNotADecimalAbove = 1e30f;

inline bool NotADecimal(float v)
{
    const float a = v < 0.0f ? -v : v;
    return v != 0.0f && a == a && (a < kNotADecimalBelow || a > kNotADecimalAbove);
}

// NR frames of reads before a candidate that never held a plausible value is given up for anything (EvictFor) and let
// go of for one that waits (Rotate): kNotADecimalReads that all say it holds something else (such a buffer says so from
// its first read), or kNothingReads of anything (a buffer that stays zero: Witcher 3 had 12 among 256).
inline constexpr unsigned int kNotADecimalReads = 60;
inline constexpr unsigned int kNothingReads = 600;

// `Slot` has `unsigned int inRange` (plausible reads), `notDecimal` (NotADecimal reads) and `reads`
template <class Slot> bool HoldsNoDecimal(const Slot& s)
{
    return s.inRange == 0 && (s.notDecimal >= kNotADecimalReads || s.reads >= kNothingReads);
}

// How likely a candidate's shape is to hold the exposure, for a full list (EvictFor). `size`: bytes for a buffer, texels
// for a texture (every layer and slice counted). A texture of up to 4 texels ranks 4 (the exposures found so far:
// Cyberpunk 2077's and Arkham Knight's 1x1, RDR2's 1x1, Doom Eternal's 2x2); another small texture, or a buffer of up to
// 16 bytes (Cyberpunk's copy of its exposure), ranks 3; a buffer of up to 64 bytes (an exposure kept in a small struct,
// beside its target and history) ranks 2; a bigger one ranks 1 (Cyberpunk's 28 never-useful 128-byte buffers).
inline int Rank(bool isBuffer, uint64_t size)
{
    if (isBuffer)
        return size <= 16 ? 3 : size <= 64 ? 2 : 1;

    return size <= 4 ? 4 : 3;
}

// The slot a newcomer of `rank` takes when the list is full, or -1. Games make most candidates at load, before NR has
// read any of them, so the full list is mostly judged by shape: the exposures of RDR2 and Doom Eternal came after 64
// buffers and bigger textures. The candidate given up is one of a lower rank that has not been seen moving and is not
// the handed exposure, that `mayGo` allows (its backend's own): the lowest rank, then the most reads without moving,
// then the lowest slot (the earlier made, while slots are not yet reused: an exposure tends to come late, with the
// post-processing). Once NR runs, a candidate that moves is kept whatever comes after it, and one that holds no decimal
// (HoldsNoDecimal) gives way first, to a newcomer of any rank.
template <class Slot, class MayGo> int EvictFor(const std::vector<Slot>& slots, int rank, MayGo mayGo)
{
    int best = -1;

    for (size_t i = 0; i < slots.size(); ++i)
    {
        const Slot& s = slots[i];
        const bool noDecimal = HoldsNoDecimal(s);

        if (!s.alive || s.moves || s.handed || (s.rank >= rank && !noDecimal) || !mayGo(s))
            continue;

        if (best < 0)
        {
            best = (int) i;
            continue;
        }

        const Slot& b = slots[best];
        const bool bNoDecimal = HoldsNoDecimal(b);

        const bool better =
            noDecimal != bNoDecimal ? noDecimal : (s.rank < b.rank || (s.rank == b.rank && s.reads > b.reads));

        if (better)
            best = (int) i;
    }

    return best;
}

// The waiting list's next one to take a free slot (Core): the likeliest shape, then the first to arrive. `Waiting` has
// `int rank`. -1 when empty.
template <class Waiting> int NextWaiting(const std::vector<Waiting>& waiting)
{
    int best = -1;

    for (size_t i = 0; i < waiting.size(); ++i)
    {
        if (best < 0 || waiting[i].rank > waiting[best].rank)
            best = (int) i;
    }

    return best;
}

// One rotation of the waiting list (Core's RotateLocked, once per NR frame while every candidate is copied): the
// candidates `ours` (the waiting list's API) that hold no decimal number and are not the handed exposure are let go of
// (`letGo(slot)`), and those waiting take the free slots, the likeliest first (`take(waiting)`: true when a slot was
// taken; the entry leaves the list either way). Nothing is let go of while nothing waits.
struct Rotation
{
    unsigned int letGo = 0;
    unsigned int taken = 0;
};

template <class Slot, class Waiting, class Ours, class LetGo, class Take>
Rotation Rotate(const std::vector<Slot>& slots, std::vector<Waiting>& waiting, size_t cap, Ours ours, LetGo letGo,
                Take take)
{
    Rotation r;

    if (waiting.empty())
        return r;

    for (size_t i = 0; i < slots.size(); ++i)
    {
        if (slots[i].alive && !slots[i].handed && ours(slots[i]) && HoldsNoDecimal(slots[i]))
        {
            letGo(i);
            r.letGo++;
        }
    }

    while (!waiting.empty() && FreeSlot(slots, cap) >= 0)
    {
        const int next = NextWaiting(waiting);
        Waiting w = std::move(waiting[(size_t) next]);
        waiting.erase(waiting.begin() + next);

        if (take(w))
            r.taken++;
    }

    return r;
}

// Whether a value copied from slot `index` when its generation was `copied` still belongs to who holds it now.
template <class Slot> bool SameOwner(const std::vector<Slot>& slots, size_t index, uint32_t copied)
{
    return index < slots.size() && slots[index].alive && slots[index].generation == copied;
}
} // namespace DlssNrExposureScanShape
