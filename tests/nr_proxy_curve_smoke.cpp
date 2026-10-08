// Host test for shaders/dlssnr/DlssNr_ProxyCurve.h: the proxy curve numbers ([DlssNr] ReversibleMode), which the
// menu offers, when Linear bands, the signal curves' white placement, and Tune's damage thresholds per curve.
//
// Build: cl /nologo /std:c++20 /EHsc /W4 tests\nr_proxy_curve_smoke.cpp
#include <cmath>
#include <cstdio>
#include <string>
#include "../OptiScaler/shaders/dlssnr/DlssNr_ProxyCurve.h"

using namespace DlssNrProxyCurve;

static int fails = 0;
static void Check(bool ok, const std::string& label)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", label.c_str());
        ++fails;
    }
}

static float SrgbEncode(float v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f; }

// dlssnr.hlsl's soft knee on a grey: identity to 0.75, then an exponential roll to 1.
static float SoftKneeGrey(float x) { return x <= 0.75f ? x : 0.75f + 0.25f * (1.0f - std::exp(-(x - 0.75f) / 0.25f)); }

int main()
{
    // The mode numbers and what the menu offers.
    for (uint32_t mode = 0; mode < kCount; ++mode)
        Check(Valid(mode), "mode " + std::to_string(mode) + " should be valid");
    Check(!Valid(kCount) && !Valid(99), "modes past the last curve should be invalid");
    Check(kPickable == kCount && kLinear == kCount - 1, "the menu should offer every curve, Linear last");
    for (uint32_t mode = 0; mode < kCount; ++mode)
    {
        Check(IsReplace(mode) == (mode == 2 || mode == 4), "Replace is 2 and 4 only (mode " + std::to_string(mode) + ")");
        Check(IsSignalCurve(mode) == (mode >= 5), "the signal curves are 5-7 (mode " + std::to_string(mode) + ")");
    }

    // Linear bands in 8- and 10-bit buffers, not in float ones.
    Check(LinearBandsIn(DXGI_FORMAT_R8G8B8A8_UNORM) && LinearBandsIn(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) &&
              LinearBandsIn(DXGI_FORMAT_R10G10B10A2_UNORM),
          "Linear should warn on 8- and 10-bit buffers");
    Check(!LinearBandsIn(DXGI_FORMAT_R16G16B16A16_FLOAT) && !LinearBandsIn(DXGI_FORMAT_R11G11B10_FLOAT) &&
              !LinearBandsIn(DXGI_FORMAT_R32G32B32A32_FLOAT),
          "Linear should not warn on float buffers");

    // White where ITU-R BT.2408 puts it, the ceilings, and rising.
    Check(std::abs(SignalOf(kHlg, 1.0f) - 0.75f) < 1e-4f, "HLG white should be 0.75");
    Check(std::abs(SignalOf(kPq, 1.0f) - 0.5807f) < 1e-4f, "PQ white should be 0.58");
    Check(SignalOf(kLinear, 1.0f) == 1.0f && SignalOf(kLinear, 2.0f) == 1.0f, "Linear should clip at white");
    Check(SignalOf(kHlg, 3.8f) == 1.0f && SignalOf(kHlg, 3.7f) < 1.0f, "HLG should reach 1 at 3.77x white");
    Check(std::abs(SignalOf(kPq, 49.26f) - 1.0f) < 1e-3f, "PQ should reach 1 at 49x white");
    for (uint32_t mode : {kHlg, kPq, kLinear})
    {
        float last = -1.0f;
        bool rises = true;
        for (float n = 1e-4f; n < 1.0f; n *= 1.3f)
        {
            rises = rises && SignalOf(mode, n) > last;
            last = SignalOf(mode, n);
        }
        Check(rises, "curve " + std::to_string(mode) + " should rise with the light");
    }

    // Tune's thresholds: the knee's 0.95 / 0.02 sit at kShoulderScene / kFloorScene, and the signal curves take their
    // thresholds at those same levels of light.
    Check(std::abs(SrgbEncode(SoftKneeGrey(kShoulderScene)) - kKneeShoulder) < 1e-4f,
          "the soft knee should store the shoulder at kShoulderScene");
    Check(std::abs(SrgbEncode(SoftKneeGrey(kFloorScene)) - kKneeFloor) < 1e-6f, "the soft knee should store the floor at kFloorScene");
    for (uint32_t mode = 0; mode <= kBalancedReplace; ++mode)
    {
        const TuneThresholds t = Thresholds(mode);
        Check(t.shoulder == 0.95f && t.floor == 0.02f, "curves 0-4 keep 0.95 / 0.02 (mode " + std::to_string(mode) + ")");
    }
    for (uint32_t mode : {kHlg, kPq, kLinear})
    {
        const TuneThresholds t = Thresholds(mode);
        std::printf("curve %u: Tune shoulder %.4f, floor %.5f\n", mode, t.shoulder, t.floor);
        Check(t.shoulder == SignalOf(mode, kShoulderScene) && t.floor == SignalOf(mode, kFloorScene),
              "curve " + std::to_string(mode) + " should take its thresholds at the knee's levels of light");
        Check(t.floor > 0.0f && t.floor < t.shoulder && t.shoulder < 1.0f, "curve " + std::to_string(mode) + " thresholds out of order");
    }
    Check(Thresholds(kPq).shoulder < 0.6f, "PQ's shoulder should sit near its white (0.58), far below 0.95");

    if (fails)
    {
        std::printf("%d check(s) failed\n", fails);
        return 1;
    }
    std::puts("PASS: proxy curve numbers, menu range, Linear banding, white placement and Tune thresholds");
    return 0;
}
