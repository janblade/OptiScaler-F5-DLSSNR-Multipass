#pragma once

// HDR output for SDR games: the rules that need no Direct3D object, so tests/sdr_to_hdr_rules_smoke.cpp builds them alone.
//
// The real swap chain is made HDR (scRGB or HDR10) and the game gets proxy back buffers in the format it asked for; one
// pass at present converts the proxy into the real buffer (output/SdrToHdr_Dx12.h). This file decides whether that
// happens for a swap chain, and holds the few numbers both sides agree on. The log text of each reason is also the menu's
// status line, so it is written for the player.
#include <dxgi.h>
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
    BitBlt,           // not flip model, and not upgradeable (made before HDR output was on, or not at creation): needs a restart
    ExclusiveBitBlt,  // an older (bitblt) swap chain in exclusive full screen: cannot be made flip model
    FrameGen,         // OptiScaler frame generation is on and this swap chain is not the frame generation library's own
    FgExclusive,      // the frame generation library's chain is really in exclusive full screen: it is not made HDR
    DlssgOutput,      // OptiScaler's DLSS-G (Streamline) output owns the swap chain
    Dxvk,             // a dxvk / Vulkan present path
    StreamlineDlssg,  // the chain is Streamline's DLSS-G proxy
    UnsupportedFormat, // not one of the four 8-bit formats
    MultiNode,        // a multi-GPU swap chain
    Composition,      // a composition swap chain (UWP / XAML), presented through the compositor
    Multisampled,     // a multisampled swap chain (Direct3D 11 bitblt with MSAA): HDR output needs one sample
    DisplayUnknown,   // the display could not be read yet: decided again at the next resize
    CoreWindow,       // a CoreWindow (UWP) swap chain
    OtherPresentQueue, // ResizeBuffers1 presents on a queue other than the one the chain was made with
    StartFailed,      // the proxy or the pass could not be made: the chain is shown as the game made it
    StuckHdr,         // the proxy could not be made and the chain could not be put back: it stays HDR (known limit: for good)
    DlssgStartedLater, // (a note, never a Decide result) the game turned on its own DLSS-G after the chain was proxied
    DisplayLeftHdrKept, // (a note) Windows HDR went off for the display, on a chain that was made flip model for HDR output
    DlssgKeptUpgraded, // (a note) the same, on a chain that was made flip model for HDR output: it stays on until the next start
    Active,
};

// Which OptiScaler frame generation output is selected (State::activeFgOutput, reduced to what HDR output cares about).
enum class FgBackend
{
    None,
    XeFG,
    FsrFG,
    Dlssg, // OptiScaler's own DLSS-G output (Streamline)
};

struct Inputs
{
    bool enabled = false;                          // [HDR] Enabled
    DXGI_FORMAT gameFormat = DXGI_FORMAT_UNKNOWN;  // what the game asked for (create, or resize with UNKNOWN resolved)
    bool flipModel = false;                        // the swap effect, after OptiScaler's own changes to it
    bool windowed = true;                          // not exclusive full screen
    bool canUpgradeBitBlt = false;                 // at creation only: a bitblt chain can still be made flip model
    bool displayHdr = false;                       // Windows HDR is on for the output the window is on
    bool frameGen = false;                         // OptiScaler frame generation is on (any backend)
    // ... and which one. FgBackend::Dlssg (OptiScaler's own DLSS-G output) and streamlineDlssg (the game's own) are separate on
    // purpose: both stand aside, with their own text
    FgBackend fgBackend = FgBackend::None;
    // This chain is the frame generation library's own: XeFG and FSR FG make their real swap chain through our factory hooks
    // (inside DxgiFactoryHooks::ScopedSkipFGSCCreation) and hand the game their own chain on top of it. The library draws and
    // presents through ours, so HDR output can convert there: frame generation only ever sees the game's SDR format. Only
    // meaningful with frameGen.
    bool fgLibraryChain = false;
    // At creation only: the library's exclusive full screen request is made a windowed chain (and kept borderless, see
    // FGHooks::KeepBorderless). False for a chain that exists: one that is really exclusive stands aside.
    bool fgWindowable = false;
    bool dxvk = false;
    bool streamlineDlssg = false;
    bool multiNode = false;
    bool composition = false;
    bool coreWindow = false;
    bool presentQueueDiffers = false;
    bool multisampled = false;                     // SampleDesc.Count > 1
    bool displayKnown = true;                      // false: the output the window is on could not be found
};

struct Decision
{
    bool use = false; // proxy and pass on
    Reason reason = Reason::Off;
    bool upgrade = false; // ... on a bitblt chain that is made flip model first (Inputs::canUpgradeBitBlt)
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
// who else owns it, then the API, then the display.
inline Decision Decide(const Inputs& in)
{
    if (!in.enabled)
        return { false, Reason::Off };

    if (IsGameHdrFormat(in.gameFormat))
        return { false, Reason::GameHdrFormat };

    if (!IsSupportedGameFormat(in.gameFormat))
        return { false, Reason::UnsupportedFormat };

    // (a multisampled chain is always bitblt: it says the more useful thing)
    if (in.multisampled)
        return { false, Reason::Multisampled };

    // An older (bitblt) chain: made flip model at creation when it is windowed (or borderless) and single-sample. In exclusive
    // full screen it cannot be, and a chain that already exists cannot change its model.
    bool upgrade = false;
    bool exclusiveBitBlt = false;
    const bool libraryChain = in.frameGen && in.fgLibraryChain;

    if (!in.flipModel)
    {
        // (the library's chains are flip model; one that is not does not take the bitblt upgrade, which does not mix with
        // keeping the window borderless for frame generation)
        if (libraryChain)
            return { false, Reason::BitBlt };

        if (!in.windowed)
            exclusiveBitBlt = true; // named last, below: the display, frame generation and the rest are the real blockers first
        else if (!in.canUpgradeBitBlt)
            return { false, Reason::BitBlt };
        else
            upgrade = true;
    }

    // Frame generation: only the library's own chain of XeFG and FSR FG is converted. Anything else it is on for (DLSS-G
    // output, another swap chain of the game, a library chain we cannot name) is left alone.
    if (in.frameGen)
    {
        if (in.fgBackend == FgBackend::Dlssg)
            return { false, Reason::DlssgOutput };

        if (!in.fgLibraryChain || (in.fgBackend != FgBackend::XeFG && in.fgBackend != FgBackend::FsrFG))
            return { false, Reason::FrameGen };
    }

    // The library's chain in exclusive full screen: made windowed at creation, but one that already is exclusive is not made
    // FP16 / 10-bit (it would have to be resized while exclusive, which its library does not expect)
    if (libraryChain && !in.windowed && !in.fgWindowable)
        return { false, Reason::FgExclusive };

    if (in.dxvk)
        return { false, Reason::Dxvk };

    if (in.streamlineDlssg)
        return { false, Reason::StreamlineDlssg };

    if (in.composition)
        return { false, Reason::Composition };

    if (in.coreWindow)
        return { false, Reason::CoreWindow };

    if (in.multiNode)
        return { false, Reason::MultiNode };

    if (in.presentQueueDiffers)
        return { false, Reason::OtherPresentQueue };

    if (!in.displayKnown)
        return { false, Reason::DisplayUnknown };

    if (!in.displayHdr)
        return { false, Reason::DisplayNotHdr };

    if (exclusiveBitBlt)
        return { false, Reason::ExclusiveBitBlt };

    return { true, Reason::Active, upgrade };
}

// A chain that was made flip model for HDR output (an older, bitblt one) never goes back to the game's own buffer at a resize:
// the real flip buffer is unbound from the pipeline at every Present, which a bitblt-era game that binds its render target once
// does not expect, so it would draw into nothing. So a resize that would end HDR output on such a chain (HDR output turned
// off, Windows HDR turned off, frame generation or the game's DLSS-G started, dxvk) keeps the proxy and the pass until the next
// start. Four stand-downs stay what they are, because keeping the proxy there is impossible or the game takes over: the game
// resizes to an FP16 / 10-bit format (it draws HDR itself, and binds per frame like any HDR game), an 8-bit format the proxy
// cannot stand in for, a multi-GPU chain, and a present queue the pass cannot run on.
inline Decision KeepUpgradedProxy(const Decision& decision, bool upgradedAndProxied)
{
    if (!upgradedAndProxied || decision.use)
        return decision;

    switch (decision.reason)
    {
    case Reason::GameHdrFormat:
    case Reason::UnsupportedFormat:
    case Reason::MultiNode:
    case Reason::OtherPresentQueue:
        return decision;
    default:
        return { true, Reason::Active, false };
    }
}

// The note over the active line for a chain that is kept converting through a resize that would have ended HDR output
// (KeepUpgradedProxy); Reason::Off when there is nothing to say
inline Reason KeptNote(Reason decided)
{
    switch (decided)
    {
    case Reason::DisplayNotHdr:
    case Reason::DisplayUnknown:
        return Reason::DisplayLeftHdrKept;
    case Reason::StreamlineDlssg:
    case Reason::DlssgOutput:
        return Reason::DlssgKeptUpgraded;
    default:
        return Reason::Off;
    }
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
        return "HDR output: standing aside, this game's swap chain is an older type that can only be changed when the game "
               "starts; restart the game with HDR output and Windows HDR on";
    case Reason::ExclusiveBitBlt:
        return "HDR output: standing aside, the game uses exclusive full screen with an older swap chain type; switch the "
               "game to borderless or windowed";
    case Reason::FrameGen:
        return "HDR output: standing aside, not available for this swap chain while frame generation is on (works with XeFG "
               "and FSR FG)";
    case Reason::FgExclusive:
        return "HDR output: standing aside, the frame generation swap chain is in exclusive full screen; switch the game to "
               "borderless or windowed";
    case Reason::DlssgOutput:
        return "HDR output: standing aside, not available with DLSS-G (OptiScaler's DLSS-G output, or a game that uses DLSS-G "
               "itself); use XeFG or FSR FG";
    case Reason::Dxvk:
        return "HDR output: standing aside, not available with dxvk (Vulkan) games yet";
    case Reason::StreamlineDlssg:
        return "HDR output: standing aside, the game's DLSS frame generation owns the swap chain";
    case Reason::UnsupportedFormat:
        return "HDR output: standing aside, this swap chain format is not supported";
    case Reason::MultiNode:
        return "HDR output: standing aside, multi-GPU swap chains are not supported";
    case Reason::Composition:
        return "HDR output: standing aside, composition swap chains are not supported";
    case Reason::CoreWindow:
        return "HDR output: standing aside, CoreWindow (UWP) swap chains are not supported";
    case Reason::OtherPresentQueue:
        return "HDR output: standing aside, the swap chain presents on a different queue than it was made with";
    case Reason::StartFailed:
        return "HDR output: could not start, the game's picture is shown as it is";
    case Reason::Multisampled:
        return "HDR output: standing aside, multisampled swap chains are not supported";
    case Reason::DisplayUnknown:
        return "HDR output: not decided yet, the display is checked again at the next resize";
    case Reason::StuckHdr:
        return "HDR output: could not undo the HDR swap chain; restart the game";
    case Reason::DlssgStartedLater:
        return "HDR output: the game turned on DLSS frame generation; HDR output stops at the next resolution / window change";
    case Reason::DisplayLeftHdrKept:
        return "HDR output: Windows HDR is off for this display; restart the game to go back to SDR";
    case Reason::DlssgKeptUpgraded:
        return "HDR output: the game turned on DLSS frame generation; this game's swap chain was changed for HDR, so HDR "
               "output stays on until the next start";
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

// The edit that makes an older (bitblt) swap chain flip model: DISCARD -> FLIP_DISCARD, SEQUENTIAL -> FLIP_SEQUENTIAL, at least
// two buffers (flip needs them), and without the GDI-compatible flag (flip cannot have it). V-Sync, tearing flags and the sync
// interval are not touched.
struct SwapDescEdit
{
    DXGI_SWAP_EFFECT swapEffect = DXGI_SWAP_EFFECT_DISCARD;
    UINT bufferCount = 2;
    UINT flags = 0;
};

inline SwapDescEdit UpgradeSwapDesc(DXGI_SWAP_EFFECT swapEffect, UINT bufferCount, UINT flags)
{
    SwapDescEdit edit;
    edit.swapEffect = swapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL ? DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
                      : swapEffect == DXGI_SWAP_EFFECT_DISCARD  ? DXGI_SWAP_EFFECT_FLIP_DISCARD
                                                                : swapEffect;
    edit.bufferCount = bufferCount < 2 ? 2 : bufferCount;
    edit.flags = flags & ~(UINT) DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
    return edit;
}

// A flip model chain refuses the GDI-compatible flag (read from DXGI, tests/sdr_to_hdr_dx11_gpu.cpp): a resize of a chain made
// flip model for HDR output goes without it. (GetDesc therefore does not show the flag the game asked for: known limit.)
inline UINT UpgradedResizeFlags(UINT flags) { return flags & ~(UINT) DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE; }

// A game's ResizeBuffers count on a chain that was made flip model for it: 0 keeps the count, anything else is at least 2
inline UINT UpgradedResizeCount(UINT requested) { return requested == 0 ? 0 : (requested < 2 ? 2 : requested); }

// What DXGI answers for CheckColorSpaceSupport on an 8-bit flip swap chain (a DXGI_COLOR_SPACE_TYPE, as an int to keep
// this header free of the Windows types): the G22 / G24 RGB spaces and PQ, but not scRGB (SetColorSpace1(scRGB) on an
// 8-bit chain is E_INVALIDARG). Read from real DXGI on WARP and on a hardware adapter with Windows HDR on alike, and
// tests/sdr_to_hdr_swapchain_dx12_gpu.cpp compares it with the real answer. The wrapper answers the game with this while
// it holds a proxy, whose real chain is FP16 and would answer scRGB only.
inline bool EightBitChainTakesColourSpace(int space)
{
    switch (space)
    {
    case 0:  // RGB_FULL_G22_NONE_P709
    case 2:  // RGB_STUDIO_G22_NONE_P709
    case 3:  // RGB_STUDIO_G22_NONE_P2020
    case 12: // RGB_FULL_G2084_NONE_P2020
    case 14: // RGB_STUDIO_G2084_NONE_P2020
    case 17: // RGB_FULL_G22_NONE_P2020
    case 20: // RGB_STUDIO_G24_NONE_P709
    case 21: // RGB_STUDIO_G24_NONE_P2020
        return true;
    default:
        return false;
    }
}

// The real chain's format for scRGB or HDR10 (its colour space is set where the chain is made, from dxgitype.h).
inline DXGI_FORMAT RealFormat(bool hdr10)
{
    return hdr10 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
}

// The pass's mode constant: 0 = scRGB, 1 = PQ.
inline unsigned PassMode(bool hdr10) { return hdr10 ? 1u : 0u; }
} // namespace SdrToHdr
