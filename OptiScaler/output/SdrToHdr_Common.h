#pragma once

// HDR output for SDR games: the rules that need no Direct3D object, so tests/sdr_to_hdr_rules_smoke.cpp builds them alone.
//
// The real swap chain is made HDR (scRGB or HDR10) and the game gets proxy back buffers in the format it asked for; one
// pass at present converts the proxy into the real buffer (output/SdrToHdr_Dx12.h). This file decides whether that
// happens for a swap chain, and holds the few numbers both sides agree on. The log text of each reason is also the menu's
// status line, so it is written for the player.
#include <dxgiformat.h>

#include <algorithm>

namespace SdrToHdr
{
// Why the proxy and the pass are on or off for one swap chain. Active is the only one that turns them on.
enum class Reason
{
    Off,              // [HDR] Enabled is false: nothing else is looked at
    GameHdrFormat,    // the game made an FP16 or 10-bit chain: it outputs HDR itself
    DisplayNotHdr,    // Windows HDR is off for the display the window is on
    BitBlt,           // not flip model: HDR needs flip
    FrameGen,         // OptiScaler frame generation owns the swap chain
    Dxvk,             // a dxvk / Vulkan present path
    StreamlineDlssg,  // the chain is Streamline's DLSS-G proxy
    UnsupportedFormat, // not one of the four 8-bit formats
    MultiNode,        // a multi-GPU swap chain
    Active,
};

struct Inputs
{
    bool enabled = false;                          // [HDR] Enabled
    DXGI_FORMAT gameFormat = DXGI_FORMAT_UNKNOWN;  // what the game asked for (create, or resize with UNKNOWN resolved)
    bool flipModel = false;                        // the swap effect, after OptiScaler's own changes to it
    bool displayHdr = false;                       // Windows HDR is on for the output the window is on
    bool frameGen = false;
    bool dxvk = false;
    bool streamlineDlssg = false;
    bool multiNode = false;
};

struct Decision
{
    bool use = false; // proxy and pass on
    Reason reason = Reason::Off;
};

// The four formats the proxy can stand in for. The _SRGB ones are views of the same bits: the pass reads the encoded
// values through the UNORM view.
inline bool IsSupportedGameFormat(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

// A chain the game made FP16 or 10-bit is one that can carry HDR: HDR10 games create 10-bit and set the colour space later.
inline bool IsGameHdrFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
        return true;
    default:
        return false;
    }
}

// Order matters: Off first (nothing else counts while the feature is off), then what the game made, then the chain, then
// who else owns it, then the display.
inline Decision Decide(const Inputs& in)
{
    if (!in.enabled)
        return { false, Reason::Off };

    if (IsGameHdrFormat(in.gameFormat))
        return { false, Reason::GameHdrFormat };

    if (!IsSupportedGameFormat(in.gameFormat))
        return { false, Reason::UnsupportedFormat };

    if (!in.flipModel)
        return { false, Reason::BitBlt };

    if (in.frameGen)
        return { false, Reason::FrameGen };

    if (in.dxvk)
        return { false, Reason::Dxvk };

    if (in.streamlineDlssg)
        return { false, Reason::StreamlineDlssg };

    if (in.multiNode)
        return { false, Reason::MultiNode };

    if (!in.displayHdr)
        return { false, Reason::DisplayNotHdr };

    return { true, Reason::Active };
}

// One player-readable line for the log, and for the menu's status line.
inline const char* ReasonText(Reason reason)
{
    switch (reason)
    {
    case Reason::Off:
        return "HDR output: off";
    case Reason::GameHdrFormat:
        return "HDR output: standing aside, the game already outputs HDR";
    case Reason::DisplayNotHdr:
        return "HDR output: standing aside, Windows HDR is off for this display";
    case Reason::BitBlt:
        return "HDR output: standing aside, this swap chain type cannot show HDR";
    case Reason::FrameGen:
        return "HDR output: standing aside, not available with frame generation yet";
    case Reason::Dxvk:
        return "HDR output: standing aside, not available with dxvk (Vulkan) games yet";
    case Reason::StreamlineDlssg:
        return "HDR output: standing aside, the game's DLSS frame generation owns the swap chain";
    case Reason::UnsupportedFormat:
        return "HDR output: standing aside, this swap chain format is not supported";
    case Reason::MultiNode:
        return "HDR output: standing aside, multi-GPU swap chains are not supported";
    case Reason::Active:
        return "HDR output: active";
    }

    return "HDR output: off";
}

// What the display reports. sdrWhiteNits is what Windows shows an SDR app's white at (the "SDR content brightness"
// slider); 80 when it cannot be read.
struct DisplayInfo
{
    bool hdrOn = false;
    float maxLuminance = 0.0f; // nits, 0 when unknown
    float sdrWhiteNits = 80.0f;
    bool sdrWhiteFromWindows = false; // false: the 80 nit fallback
};

// Windows reports the SDR white level in thousandths of the 80 nit reference (1000 = 80 nits).
inline float SdrWhiteLevelToNits(unsigned long level) { return (float) level / 1000.0f * 80.0f; }

// The peak the pass clamps to: [HDR] PeakNits, or with 0 the display's MaxLuminance, or 1000 when it reports none.
inline float ResolvePeakNits(float configured, float displayMaxLuminance)
{
    if (configured > 0.0f)
        return configured;

    return displayMaxLuminance > 0.0f ? displayMaxLuminance : 1000.0f;
}

inline float ClampGameNits(float nits) { return std::clamp(nits, 80.0f, 1000.0f); }

// The real chain's format for scRGB or HDR10 (its colour space is set where the chain is made, from dxgitype.h).
inline DXGI_FORMAT RealFormat(bool hdr10)
{
    return hdr10 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
}

// The pass's mode constant: 0 = scRGB, 1 = PQ.
inline unsigned PassMode(bool hdr10) { return hdr10 ? 1u : 0u; }
} // namespace SdrToHdr
