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
    off.frameGen = off.dxvk = off.streamlineDlssg = off.multiNode = off.composition = off.d3d11 = true;
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
    in.d3d11 = true;
    Check(Is(in, Reason::D3D11NotYet), "Direct3D 11 is D3D11NotYet");

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
    in.d3d11 = true;
    in.displayKnown = false;
    Check(Is(in, Reason::D3D11NotYet), "D3D11 outranks an unknown display");

    // Every reason has its own one-line text, starting the same way
    const Reason all[] = { Reason::Off,         Reason::GameHdrFormat,   Reason::DisplayNotHdr,
                           Reason::BitBlt,      Reason::FrameGen,        Reason::Dxvk,
                           Reason::StreamlineDlssg, Reason::UnsupportedFormat, Reason::MultiNode, Reason::Composition,
                           Reason::D3D11NotYet, Reason::DisplayUnknown, Reason::Active };
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

        snap.enabledAtDecision = true;

        for (Reason reason : { Reason::GameHdrFormat, Reason::DisplayNotHdr, Reason::BitBlt, Reason::FrameGen, Reason::Dxvk,
                               Reason::StreamlineDlssg, Reason::UnsupportedFormat, Reason::MultiNode, Reason::Composition,
                               Reason::D3D11NotYet, Reason::DisplayUnknown })
        {
            snap.reason = reason;
            line = StatusLine(snap, on);
            Check(line.colour == StatusColour::Amber && line.text == ReasonText(reason) && line.signalHint.empty(),
                  ReasonText(reason));
        }
    }

    std::printf(g_failures == 0 ? "sdr_to_hdr_rules_smoke: PASS\n" : "sdr_to_hdr_rules_smoke: %d FAIL\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
