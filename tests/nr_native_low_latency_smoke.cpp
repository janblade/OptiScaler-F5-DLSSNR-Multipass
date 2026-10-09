// Host check of native/NativeLowLatencyRule.h: when Optical F5Low's own low latency runs and stands aside. No GPU, no
// game. cl /std:c++20 /EHsc /W4 tests/nr_native_low_latency_smoke.cpp
#include "../OptiScaler/native/NativeLowLatencyRule.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace native::lowlatency;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

int main()
{
    Inputs in;
    in.f5lowRunning = true;

    // The default: on with an Optical F5Low mode
    CHECK(Decide(in) == Decision::Run);

    // Auto needs an Optical F5Low mode; On does not
    in.f5lowRunning = false;
    CHECK(Decide(in) == Decision::NoF5Low);
    in.setting = Setting::On;
    CHECK(Decide(in) == Decision::Run);

    // Off beats everything
    in.setting = Setting::Off;
    in.f5lowRunning = true;
    CHECK(Decide(in) == Decision::SettingOff);

    // The game's own Reflex: ours never runs, in either setting
    for (auto setting : { Setting::Auto, Setting::On })
    {
        Inputs g;
        g.setting = setting;
        g.f5lowRunning = true;
        g.gameCallsReflex = true;
        CHECK(Decide(g) == Decision::GameRunsReflex);
    }

    // fakenvapi's settings are respected, never overridden
    {
        Inputs f;
        f.f5lowRunning = true;
        f.forceReflexDisabled = true;
        CHECK(Decide(f) == Decision::ForceReflexDisabled);
        f.forceReflexDisabled = false;
        f.forceXell = true;
        CHECK(Decide(f) == Decision::ForceXell);
        f.forceXell = false;
        f.otherFrameGenerationOwner = true;
        CHECK(Decide(f) == Decision::FrameGenerationOwnsReflex);
        f.otherFrameGenerationOwner = false;
        f.apiAvailable = false;
        CHECK(Decide(f) == Decision::NoApi);
    }

    // The game beats the fakenvapi reasons (the more useful line)
    {
        Inputs f;
        f.f5lowRunning = true;
        f.gameCallsReflex = true;
        f.forceReflexDisabled = true;
        CHECK(Decide(f) == Decision::GameRunsReflex);
    }

    // Every reason has its plain text; Run has none
    for (auto d : { Decision::SettingOff, Decision::NoF5Low, Decision::GameRunsReflex, Decision::ForceReflexDisabled,
                    Decision::ForceXell, Decision::FrameGenerationOwnsReflex, Decision::NoApi })
        CHECK(DecisionText(d)[0] != '\0');

    CHECK(DecisionText(Decision::Run)[0] == '\0');
    CHECK(strcmp(DecisionText(Decision::GameRunsReflex), "Please use the Game's Own Reflex Settings") == 0);

    // The status label: On when it runs, Standing Aside when something else owns latency, Off otherwise
    CHECK(strcmp(StatusLabel(Decision::Run), "On") == 0);

    for (auto d : { Decision::GameRunsReflex, Decision::ForceReflexDisabled, Decision::ForceXell,
                    Decision::FrameGenerationOwnsReflex })
        CHECK(strcmp(StatusLabel(d), "Standing Aside") == 0);

    for (auto d : { Decision::SettingOff, Decision::NoF5Low, Decision::NoApi })
        CHECK(strcmp(StatusLabel(d), "Off") == 0);

    // With frame generation's swap chain, only the game's present to it counts; the wrapped swap chain's presents
    // behind it (real and generated frames) are skipped, and count again once frame generation's presents stop
    CHECK(!SwapChainPresentIgnored(5000.0, 0.0));
    CHECK(SwapChainPresentIgnored(5000.0, 4990.0));
    CHECK(SwapChainPresentIgnored(5000.0, 5000.0));
    CHECK(!SwapChainPresentIgnored(5000.0, 5000.0 - kFrameGenerationPresentHoldMs));
    CHECK(!SwapChainPresentIgnored(9000.0, 4990.0));

    // Which Reflex surface a game's present goes through: decided per call from what the caller says, never remembered
    // from an earlier call (a D3D call after a Vulkan one is D3D; no D3D12 device pointer reaches the Vulkan calls).
    {
        int vkDevice = 0, bridgeDevice12 = 0, d3dDevice = 0;

        // A Vulkan game, not bridged: its VkDevice through the Vulkan surface
        auto t = PresentTarget(true, false, &vkDevice, &bridgeDevice12);
        CHECK(t.api == Api::Vulkan && t.device == &vkDevice);

        // Then a D3D game's present (the Vulkan one before it leaves nothing behind)
        t = PresentTarget(false, false, &d3dDevice, nullptr);
        CHECK(t.api == Api::D3D && t.device == &d3dDevice);

        // A bridged Vulkan game: the bridge's private D3D12 device through the D3D surface; no Vulkan call at all
        t = PresentTarget(true, true, &vkDevice, &bridgeDevice12);
        CHECK(t.api == Api::D3D && t.device == &bridgeDevice12);

        // The bridge's device is gone mid-present: nothing to call (and still not the VkDevice through the D3D calls)
        t = PresentTarget(true, true, &vkDevice, nullptr);
        CHECK(t.device == nullptr);

        // And back to the unbridged Vulkan game
        t = PresentTarget(true, false, &vkDevice, &bridgeDevice12);
        CHECK(t.api == Api::Vulkan && t.device == &vkDevice);
    }

    printf(fails == 0 ? "nr_native_low_latency_smoke: PASS\n" : "nr_native_low_latency_smoke: %d FAIL\n", fails);
    return fails == 0 ? 0 : 1;
}
