// Host test for output/SdrToHdr_Common.h: when the HDR output proxy is on, and the numbers around it.
//
//   cl /std:c++20 /EHsc /W4 tests\sdr_to_hdr_rules_smoke.cpp
#include "../OptiScaler/output/SdrToHdr_Common.h"
#include "../OptiScaler/output/SdrToHdr_Status.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace SdrToHdr;

namespace
{
int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

// A swap chain that would be turned into an HDR one: everything the rules look at is in its favour.
Inputs Good()
{
    Inputs in;
    in.enabled = true;
    in.gameFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    in.flipModel = true;
    in.displayHdr = true;
    return in;
}

bool Is(const Inputs& in, Reason expected)
{
    const Decision d = Decide(in);
    return d.reason == expected && d.use == (expected == Reason::Active);
}
} // namespace

int main()
{
    Check(Is(Good(), Reason::Active), "everything in favour: Active, proxy on");

    // Off wins over every other reason
    Inputs off = Good();
    off.enabled = false;
    off.gameFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    off.flipModel = false;
    off.displayHdr = false;
    off.frameGen = off.dxvk = off.streamlineDlssg = off.multiNode = off.composition = off.multisampled = true;
    off.displayKnown = false;
    Check(Is(off, Reason::Off), "Enabled=false: Off, although every other reason would also hold");

    // The supported formats
    for (DXGI_FORMAT format : { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM,
                                DXGI_FORMAT_B8G8R8A8_UNORM_SRGB })
    {
        Inputs in = Good();
        in.gameFormat = format;
        Check(Is(in, Reason::Active), "supported 8-bit format is Active");
    }

    // HDR formats: the game outputs HDR itself
    for (DXGI_FORMAT format : { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_TYPELESS,
                                DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_TYPELESS,
                                DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM })
    {
        Inputs in = Good();
        in.gameFormat = format;
        Check(Is(in, Reason::GameHdrFormat), "FP16 / 10-bit format is GameHdrFormat");
    }

    // Anything else is unsupported
    for (DXGI_FORMAT format : { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R8G8B8A8_TYPELESS,
                                DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_R32G32B32A32_FLOAT })
    {
        Inputs in = Good();
        in.gameFormat = format;
        Check(Is(in, Reason::UnsupportedFormat), "other format is UnsupportedFormat");
    }

    Inputs in = Good();
    in.flipModel = false;
    Check(Is(in, Reason::BitBlt), "bitblt model is BitBlt");

    in = Good();
    in.frameGen = true;
    Check(Is(in, Reason::FrameGen), "frame generation is FrameGen");

    in = Good();
    in.dxvk = true;
    Check(Is(in, Reason::Dxvk), "dxvk is Dxvk");

    in = Good();
    in.streamlineDlssg = true;
    Check(Is(in, Reason::StreamlineDlssg), "Streamline DLSS-G proxy is StreamlineDlssg");

    in = Good();
    in.multiNode = true;
    Check(Is(in, Reason::MultiNode), "multi-GPU chain is MultiNode");

    in = Good();
    in.composition = true;
    Check(Is(in, Reason::Composition), "composition swap chain is Composition");

    in = Good();
    in.coreWindow = true;
    Check(Is(in, Reason::CoreWindow), "CoreWindow (UWP) swap chain is CoreWindow");

    in = Good();
    in.presentQueueDiffers = true;
    Check(Is(in, Reason::OtherPresentQueue), "a ResizeBuffers1 present queue other than the creation one is OtherPresentQueue");

    in = Good();
    in.multisampled = true;
    Check(Is(in, Reason::Multisampled), "a multisampled swap chain is Multisampled");
    in.flipModel = false;
    Check(Is(in, Reason::Multisampled), "Multisampled says so before BitBlt (a multisampled chain is always bitblt)");

    // A plain Direct3D 11 flip chain is Active: nothing in Inputs says Direct3D 11 any more
    in = Good();
    in.gameFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    Check(Is(in, Reason::Active), "a Direct3D 11 flip chain, 8-bit, on an HDR display is Active");

    in = Good();
    in.displayKnown = false;
    in.displayHdr = false;
    Check(Is(in, Reason::DisplayUnknown), "display not read yet is DisplayUnknown");

    in = Good();
    in.displayHdr = false;
    Check(Is(in, Reason::DisplayNotHdr), "display not in HDR mode is DisplayNotHdr");

    // Order: the game's own format outranks the chain type, which outranks who else owns it, then the display
    in = Good();
    in.gameFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    in.flipModel = false;
    Check(Is(in, Reason::GameHdrFormat), "HDR format outranks bitblt");
    in = Good();
    in.flipModel = false;
    in.frameGen = true;
    Check(Is(in, Reason::BitBlt), "bitblt outranks frame generation");
    in = Good();
    in.dxvk = true;
    in.displayHdr = false;
    Check(Is(in, Reason::Dxvk), "dxvk outranks the display");
    in = Good();
    in.multisampled = true;
    in.displayKnown = false;
    Check(Is(in, Reason::Multisampled), "Multisampled outranks an unknown display");

    // Every reason has its own one-line text, starting the same way
    const Reason all[] = { Reason::Off,         Reason::GameHdrFormat,   Reason::DisplayNotHdr,
                           Reason::BitBlt,      Reason::FrameGen,        Reason::Dxvk,
                           Reason::StreamlineDlssg, Reason::UnsupportedFormat, Reason::MultiNode, Reason::Composition,
                           Reason::Multisampled, Reason::DisplayUnknown, Reason::CoreWindow,
                           Reason::OtherPresentQueue, Reason::StartFailed, Reason::StuckHdr, Reason::DlssgStartedLater,
                           Reason::ExclusiveBitBlt, Reason::Active };
    bool texts = true;

    for (size_t i = 0; i < std::size(all); ++i)
    {
        const char* text = ReasonText(all[i]);
        texts = texts && text != nullptr && std::strncmp(text, "HDR output: ", 12) == 0 && std::strchr(text, '\n') == nullptr;

        for (size_t j = 0; j < i; ++j)
            texts = texts && std::strcmp(text, ReasonText(all[j])) != 0;
    }

    Check(texts, "each reason has its own one-line text");

    // The numbers
    Check(std::fabs(SdrWhiteLevelToNits(1000) - 80.0f) < 1e-4f, "SDR white level 1000 = 80 nits");
    Check(std::fabs(SdrWhiteLevelToNits(3000) - 240.0f) < 1e-3f, "SDR white level 3000 = 240 nits");
    Check(ResolvePeakNits(0.0f, 1500.0f) == 1500.0f, "PeakNits 0 = the display's maximum");
    Check(ResolvePeakNits(0.0f, 0.0f) == 1000.0f, "PeakNits 0 with no display maximum = 1000");
    Check(ResolvePeakNits(600.0f, 1500.0f) == 600.0f, "PeakNits set wins over the display");
    Check(ClampGameNits(10.0f) == 80.0f && ClampGameNits(5000.0f) == 1000.0f && ClampGameNits(203.0f) == 203.0f,
          "GameNits is clamped to 80..1000");
    Check(RealFormat(false) == DXGI_FORMAT_R16G16B16A16_FLOAT && RealFormat(true) == DXGI_FORMAT_R10G10B10A2_UNORM,
          "scRGB is FP16, HDR10 is 10-bit");
    Check(PassMode(false) == 0u && PassMode(true) == 1u, "pass mode 0 = scRGB, 1 = PQ");

    // ---- the menu's status line ----
    {
        StatusSettings on;
        on.enabled = true;
        on.gameNits = 203.0f;
        StatusSnapshot snap;

        Check(StatusLine(snap, StatusSettings {}).colour == StatusColour::None && StatusLine(snap, StatusSettings {}).text.empty(),
              "status: off and never decided: no line");

        auto line = StatusLine(snap, on);
        Check(line.colour == StatusColour::Amber && line.text == AppliesLaterText(),
              "status: turned on, no chain decided yet: applies later (amber)");

        snap.decided = true;
        snap.enabledAtDecision = true;
        snap.active = true;
        snap.reason = Reason::Active;
        snap.displayMaxLuminance = 800.0f;
        line = StatusLine(snap, on);
        Check(line.colour == StatusColour::Green && line.text.find("active") != std::string::npos &&
                  line.text.find("scRGB") != std::string::npos && line.text.find("203 nits") != std::string::npos &&
                  line.text.find("800 nits (display)") != std::string::npos && line.signalHint.empty(),
              "status: active scRGB, 203 nits, peak 800 from the display (green)");

        StatusSettings ini = on;
        ini.configuredPeak = 600.0f;
        Check(StatusLine(snap, ini).text.find("600 nits (ini)") != std::string::npos, "status: peak from the ini says so");
        snap.displayMaxLuminance = 0.0f;
        Check(StatusLine(snap, on).text.find("1000 nits (default)") != std::string::npos, "status: no display peak: 1000 (default)");
        snap.displayMaxLuminance = 800.0f;

        StatusSettings hdr10 = on;
        hdr10.hdr10 = true;
        line = StatusLine(snap, hdr10);
        Check(line.colour == StatusColour::Green && line.signalHint.find("HDR10") != std::string::npos,
              "status: signal setting differs from the active chain: a hint");
        snap.hdr10 = true;
        Check(StatusLine(snap, hdr10).signalHint.empty() && StatusLine(snap, hdr10).text.find("HDR10") != std::string::npos,
              "status: HDR10 chain, HDR10 setting: no hint");
        snap.hdr10 = false;

        StatusSettings offSettings = on;
        offSettings.enabled = false;
        line = StatusLine(snap, offSettings);
        Check(line.colour == StatusColour::Amber && line.text == AppliesLaterText(),
              "status: turned off while the chain converts: applies later");

        snap.enabledAtDecision = false;
        snap.active = false;
        snap.reason = Reason::Off;
        Check(StatusLine(snap, offSettings).colour == StatusColour::None, "status: off and the chain was off: no line");
        Check(StatusLine(snap, on).text == AppliesLaterText(), "status: turned on, the chain was made with it off: applies later");

        // A Vulkan game never decides a chain: its own line, not "applies later" forever (Review Pass B2)
        {
            StatusSettings vulkan = on;
            vulkan.nonDxgiApi = true;
            const StatusSnapshot never; // nothing decided: the game made no DXGI chain
            const auto vk = StatusLine(never, vulkan);
            Check(vk.colour == StatusColour::Amber && vk.text.find("Vulkan") != std::string::npos,
                  "status: turned on in a Vulkan game: not available for Vulkan (amber)");
            vulkan.enabled = false;
            Check(StatusLine(never, vulkan).colour == StatusColour::None, "status: off in a Vulkan game: no line");
        }

        snap.enabledAtDecision = true;

        for (Reason reason : { Reason::GameHdrFormat, Reason::DisplayNotHdr, Reason::BitBlt, Reason::FrameGen, Reason::Dxvk,
                               Reason::StreamlineDlssg, Reason::UnsupportedFormat, Reason::MultiNode, Reason::Composition,
                               Reason::Multisampled, Reason::DisplayUnknown, Reason::CoreWindow,
                               Reason::OtherPresentQueue, Reason::StartFailed, Reason::StuckHdr, Reason::ExclusiveBitBlt })
        {
            snap.reason = reason;
            line = StatusLine(snap, on);
            Check(line.colour == StatusColour::Amber && line.text == ReasonText(reason) && line.signalHint.empty(),
                  ReasonText(reason));
        }

        Check(std::strstr(ReasonText(Reason::StuckHdr), "restart the game") != nullptr &&
                  std::strstr(ReasonText(Reason::StuckHdr), "shown as it is") == nullptr,
              "stuck HDR says to restart, not that the picture is shown as it is");

        // Still converting, but the game turned on its own DLSS-G: the line says when it ends, the controls stay
        snap.active = true;
        snap.reason = Reason::Active;
        snap.endsAtResize = true;
        snap.endReason = Reason::DlssgStartedLater;
        line = StatusLine(snap, on);
        Check(line.colour == StatusColour::Amber && line.text == ReasonText(Reason::DlssgStartedLater) &&
                  line.text.find("next resolution / window change") != std::string::npos,
              "status: active, then the game turned on DLSS-G: amber, stops at the next resolution / window change");
        snap.endsAtResize = false;
        Check(StatusLine(snap, on).colour == StatusColour::Green, "status: the next decision clears that note");
    }

    // ---- older (bitblt) swap chains made flip model (S2b) ----
    {
        Inputs bitblt = Good();
        bitblt.flipModel = false;

        // an existing chain (a resize): cannot change its model
        Check(Is(bitblt, Reason::BitBlt), "bitblt, windowed, not at creation: BitBlt (it cannot be upgraded any more)");

        // at creation: windowed or borderless, one sample, everything else passing -> Active, upgraded
        bitblt.canUpgradeBitBlt = true;
        Decision d = Decide(bitblt);
        Check(d.use && d.upgrade && d.reason == Reason::Active, "bitblt, windowed, single sample, at creation: Active, upgraded");

        Inputs flip = Good();
        flip.canUpgradeBitBlt = true;
        d = Decide(flip);
        Check(d.use && !d.upgrade, "a flip chain at creation is Active and not upgraded");

        Inputs exclusive = bitblt;
        exclusive.windowed = false;
        Check(Is(exclusive, Reason::ExclusiveBitBlt), "bitblt in exclusive full screen: ExclusiveBitBlt");
        exclusive.canUpgradeBitBlt = false;
        Check(Is(exclusive, Reason::ExclusiveBitBlt), "... also for a chain that exists");
        exclusive.flipModel = true;
        Check(Is(exclusive, Reason::Active), "a flip chain in exclusive full screen is Active (flip allows it)");

        Inputs msaa = bitblt;
        msaa.multisampled = true;
        Check(Is(msaa, Reason::Multisampled), "bitblt with MSAA: Multisampled, not upgraded");

        Inputs others = bitblt;
        others.displayHdr = false;
        d = Decide(others);
        Check(!d.use && !d.upgrade && d.reason == Reason::DisplayNotHdr, "bitblt on a display without HDR: DisplayNotHdr, nothing is upgraded");
        others = bitblt;
        others.frameGen = true;
        Check(Is(others, Reason::FrameGen), "bitblt with frame generation: FrameGen, nothing is upgraded");

        Inputs off = bitblt;
        off.enabled = false;
        d = Decide(off);
        Check(!d.use && !d.upgrade && d.reason == Reason::Off, "off: nothing is upgraded");

        Check(std::strstr(ReasonText(Reason::ExclusiveBitBlt), "borderless or windowed") != nullptr,
              "the exclusive full screen text says what to do");
        Check(ReasonText(Reason::BitBlt) != ReasonText(Reason::ExclusiveBitBlt) &&
                  std::strstr(ReasonText(Reason::BitBlt), "restart") != nullptr,
              "BitBlt now means 'made before HDR output was on': restart");

        // the desc edit
        auto edit = UpgradeSwapDesc(DXGI_SWAP_EFFECT_DISCARD, 1, 0);
        Check(edit.swapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD && edit.bufferCount == 2 && edit.flags == 0,
              "edit: DISCARD, 1 buffer -> FLIP_DISCARD, 2");
        edit = UpgradeSwapDesc(DXGI_SWAP_EFFECT_SEQUENTIAL, 1, 0);
        Check(edit.swapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL && edit.bufferCount == 2, "edit: SEQUENTIAL, 1 buffer -> FLIP_SEQUENTIAL, 2");
        edit = UpgradeSwapDesc(DXGI_SWAP_EFFECT_DISCARD, 3, 0);
        Check(edit.bufferCount == 3, "edit: three buffers stay three");
        edit = UpgradeSwapDesc(DXGI_SWAP_EFFECT_DISCARD, 1,
                               DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE | DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH |
                                   DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
        Check((edit.flags & DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE) == 0 && (edit.flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH) != 0 &&
                  (edit.flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0,
              "edit: the GDI-compatible flag (flip cannot have it) is dropped, mode switch and tearing flags are untouched");
        edit = UpgradeSwapDesc(DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, 1, 0);
        Check(edit.swapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, "edit: a flip effect stays as it is");
        Check(UpgradedResizeCount(0) == 0 && UpgradedResizeCount(1) == 2 && UpgradedResizeCount(2) == 2 && UpgradedResizeCount(4) == 4,
              "resize count: 0 keeps, 1 becomes 2, others stay");
    }

    std::printf(g_failures == 0 ? "sdr_to_hdr_rules_smoke: PASS\n" : "sdr_to_hdr_rules_smoke: %d FAIL\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
