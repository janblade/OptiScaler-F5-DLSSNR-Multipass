#pragma once

// What the exposure scan takes as a candidate, the same on every API (DlssNr_ExposureScan.cpp for D3D12,
// DlssNr_ExposureScan_Dx11.cpp, DlssNr_ExposureScan_Vk.cpp), and how its list reuses slots.
//
// The shape: something a shader writes (a UAV on D3D, a storage buffer or image on Vulkan) that holds a number rather
// than a picture -- a texture of at most 256 texels in a float format of one or two channels, or a buffer of at most
// 128 bytes. The reasons for each bound are with the D3D12 filter (LooksLikeANumber).
//
// The slots: a candidate that goes away (the game destroyed it, or the scan let go of it) frees its slot rather than
// shifting the list, because each API's readback ring holds values by slot index for a few frames. A slot's generation
// counts its owners, so a value copied for the old owner is never credited to the new one.
//
// Header-only, no D3D or Vulkan types: tests/nr_exposure_scan_shape_smoke.cpp.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace DlssNrExposureScanShape
{
inline constexpr uint64_t kMaxBufferBytes = 128;
inline constexpr uint64_t kMaxTexels = 256;

inline bool SmallBuffer(uint64_t bytes) { return bytes > 0 && bytes <= kMaxBufferBytes; }

inline bool SmallTexture(uint64_t width, uint64_t height)
{
    return width > 0 && height > 0 && width * height <= kMaxTexels;
}

// Vulkan formats an exposure could be in, by their VkFormat values (asserted against vulkan_core.h where used):
// the size of the first channel, the one texel read, and a name for the readout. Not a candidate when `bytes` is 0.
struct Format
{
    unsigned int bytes = 0;
    const char* name = "";
};

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

// Whether a value copied from slot `index` when its generation was `copied` still belongs to who holds it now.
template <class Slot> bool SameOwner(const std::vector<Slot>& slots, size_t index, uint32_t copied)
{
    return index < slots.size() && slots[index].alive && slots[index].generation == copied;
}
} // namespace DlssNrExposureScanShape
