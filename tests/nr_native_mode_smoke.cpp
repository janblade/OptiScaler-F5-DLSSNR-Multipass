// Host check of DlssNr_NativeMode.h: the mode selector of "NR without a game upscaler". No GPU and no game needed.
// cl /std:c++20 /EHsc /W4 tests/nr_native_mode_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_NativeMode.h"

#include <cstdio>

using namespace DlssNrNativeMode;

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

static bool Same(const std::optional<Keys>& a, const Keys& b)
{
    return a.has_value() && a->depthFinder == b.depthFinder && a->motion == b.motion && a->input == b.input &&
           a->upscaler == b.upscaler;
}

static Keys K(bool depthFinder, bool motion, bool input, bool upscaler)
{
    return { depthFinder, motion, input, upscaler };
}

int main()
{
    // Each mode's keys, whatever NR Pass at: was.
    for (bool fp : { false, true })
    {
        CHECK(Same(ForMode(Mode::Off, fp).keys, K(false, false, false, false)));
        CHECK(Same(ForMode(Mode::NrOnly, fp).keys, K(true, true, true, false)));
        CHECK(Same(ForMode(Mode::NrAndFrameGeneration, fp).keys, K(true, true, false, true)));
    }

    // Finished Picture: Off leaves it alone; NR only turns it on; NR + frame generation turns it off. A change clears a
    // session failure, as the NR Pass at: combo does; no change writes nothing.
    {
        CHECK(!ForMode(Mode::Off, false).finishedPicture.has_value() && !ForMode(Mode::Off, false).retryAfterFailure);
        CHECK(!ForMode(Mode::Off, true).finishedPicture.has_value() && !ForMode(Mode::Off, true).retryAfterFailure);

        const auto nrOnlyFromOff = ForMode(Mode::NrOnly, false);
        CHECK(nrOnlyFromOff.finishedPicture == std::optional<bool>(true) && nrOnlyFromOff.retryAfterFailure);
        const auto nrOnlyAlreadyOn = ForMode(Mode::NrOnly, true);
        CHECK(!nrOnlyAlreadyOn.finishedPicture.has_value() && !nrOnlyAlreadyOn.retryAfterFailure);

        const auto fgFromOn = ForMode(Mode::NrAndFrameGeneration, true);
        CHECK(fgFromOn.finishedPicture == std::optional<bool>(false) && fgFromOn.retryAfterFailure);
        const auto fgAlreadyOff = ForMode(Mode::NrAndFrameGeneration, false);
        CHECK(!fgAlreadyOff.finishedPicture.has_value() && !fgAlreadyOff.retryAfterFailure);
    }

    // Read-back: every combination of the four keys. The depth finder never decides the mode.
    for (int bits = 0; bits < 16; ++bits)
    {
        const Keys k = K(bits & 1, bits & 2, bits & 4, bits & 8);
        const Shown shown = FromKeys(k);

        if (!k.motion)
            CHECK(shown == Shown::Off);
        else if (k.upscaler)
            CHECK(shown == Shown::NrAndFrameGeneration); // also with input on: the upscaler is what runs
        else if (k.input)
            CHECK(shown == Shown::NrOnly);
        else
            CHECK(shown == Shown::MotionOnly);
    }

    // Inconsistent key sets, spelled out.
    CHECK(FromKeys(K(true, true, true, true)) == Shown::NrAndFrameGeneration);
    CHECK(FromKeys(K(false, true, false, true)) ==
          Shown::NrAndFrameGeneration); // depth unticked afterwards: mode stays
    CHECK(FromKeys(K(false, true, true, false)) == Shown::NrOnly);
    CHECK(FromKeys(K(true, false, true, false)) == Shown::Off); // no motion, nothing runs
    CHECK(FromKeys(K(true, false, false, true)) == Shown::Off);
    CHECK(FromKeys(K(true, true, false, false)) == Shown::MotionOnly);

    // A mode reads back as itself.
    for (bool fp : { false, true })
    {
        CHECK(FromKeys(ForMode(Mode::Off, fp).keys.value()) == Shown::Off);
        CHECK(FromKeys(ForMode(Mode::NrOnly, fp).keys.value()) == Shown::NrOnly);
        CHECK(FromKeys(ForMode(Mode::NrAndFrameGeneration, fp).keys.value()) == Shown::NrAndFrameGeneration);
    }

    // A click on a different mode (or from motion-only) writes that mode's keys.
    for (bool fp : { false, true })
    {
        CHECK(Same(ForClick(Mode::NrOnly, Shown::Off, fp).keys, K(true, true, true, false)));
        CHECK(Same(ForClick(Mode::NrOnly, Shown::NrAndFrameGeneration, fp).keys, K(true, true, true, false)));
        CHECK(Same(ForClick(Mode::NrAndFrameGeneration, Shown::NrOnly, fp).keys, K(true, true, false, true)));
        CHECK(Same(ForClick(Mode::Off, Shown::MotionOnly, fp).keys, K(false, false, false, false)));
        CHECK(Same(ForClick(Mode::NrOnly, Shown::MotionOnly, fp).keys, K(true, true, true, false)));
    }

    // A click on the mode already in effect keeps the keys but still puts Finished Picture right, with the retry.
    {
        const auto again = ForClick(Mode::NrOnly, Shown::NrOnly, false);
        CHECK(!again.keys.has_value());
        CHECK(again.finishedPicture == std::optional<bool>(true) && again.retryAfterFailure);

        const auto againSet = ForClick(Mode::NrOnly, Shown::NrOnly, true);
        CHECK(!againSet.keys.has_value() && !againSet.finishedPicture.has_value() && !againSet.retryAfterFailure);

        const auto fgAgain = ForClick(Mode::NrAndFrameGeneration, Shown::NrAndFrameGeneration, true);
        CHECK(!fgAgain.keys.has_value());
        CHECK(fgAgain.finishedPicture == std::optional<bool>(false) && fgAgain.retryAfterFailure);

        for (bool fp : { false, true })
        {
            const auto offAgain = ForClick(Mode::Off, Shown::Off, fp);
            CHECK(!offAgain.keys.has_value() && !offAgain.finishedPicture.has_value() && !offAgain.retryAfterFailure);
        }
    }

    // The depth finder's state at this start, and when the selector asks for a restart.
    {
        CHECK(FinderFor(false, false, false) == Finder::Off);
        CHECK(FinderFor(false, false, true) == Finder::Off);
        CHECK(FinderFor(true, false, false) == Finder::NeedsRestart);
        CHECK(FinderFor(true, false, true) == Finder::CouldNotStart);
        CHECK(FinderFor(true, true, false) == Finder::Installed);
        CHECK(FinderFor(false, true, false) == Finder::Installed); // unticked: its hooks stay until a restart

        for (Shown shown : { Shown::NrOnly, Shown::NrAndFrameGeneration, Shown::MotionOnly })
        {
            CHECK(DepthRestartWarning(shown, Finder::NeedsRestart));
            CHECK(!DepthRestartWarning(shown, Finder::CouldNotStart));
            CHECK(!DepthRestartWarning(shown, Finder::Installed));
            CHECK(!DepthRestartWarning(shown, Finder::Off));
        }

        CHECK(!DepthRestartWarning(Shown::Off, Finder::NeedsRestart));
    }

    // Warnings.
    CHECK(WarningFor(Shown::NrOnly, true, true, false, false) == Warning::None);
    CHECK(WarningFor(Shown::NrOnly, true, true, true, false) == Warning::Dx11FrameGeneration);
    CHECK(WarningFor(Shown::NrOnly, false, false, true, false) ==
          Warning::Dx11FrameGeneration); // the one to act on first
    CHECK(WarningFor(Shown::NrOnly, false, true, false, false) == Warning::NrDisabled);
    CHECK(WarningFor(Shown::NrOnly, false, false, false, false) == Warning::NrDisabled);
    CHECK(WarningFor(Shown::NrOnly, true, false, false, false) == Warning::NeedsFinishedPicture);

    CHECK(WarningFor(Shown::NrAndFrameGeneration, true, false, false, false) == Warning::None);
    CHECK(WarningFor(Shown::NrAndFrameGeneration, true, true, true, false) == Warning::None);
    CHECK(WarningFor(Shown::NrAndFrameGeneration, false, false, true, false) == Warning::NrDisabled);

    for (int bits = 0; bits < 8; ++bits)
    {
        CHECK(WarningFor(Shown::Off, bits & 1, bits & 2, bits & 4, false) == Warning::None);
        CHECK(WarningFor(Shown::MotionOnly, bits & 1, bits & 2, bits & 4, false) == Warning::None);
    }

    // A game that calls an upscaler of its own: only Off can be chosen, and a mode already on says it stands aside
    // before anything else.
    for (Mode mode : { Mode::NrOnly, Mode::NrAndFrameGeneration })
    {
        CHECK(Selectable(mode, false));
        CHECK(!Selectable(mode, true));
    }

    CHECK(Selectable(Mode::Off, false) && Selectable(Mode::Off, true));

    // A Vulkan game has no mode left out: frame generation presents through the bridge made when the game starts. Until it
    // is up the mode says to restart; the game's own upscaler still comes first.
    CHECK(Selectable(Mode::NrAndFrameGeneration, false));
    CHECK(WarningFor(Shown::NrAndFrameGeneration, true, false, false, false, true) == Warning::VulkanNeedsRestart);
    CHECK(WarningFor(Shown::NrAndFrameGeneration, true, false, false, false, false) == Warning::None);
    CHECK(WarningFor(Shown::NrAndFrameGeneration, true, false, false, true, true) == Warning::GameUpscaler);
    CHECK(WarningFor(Shown::NrOnly, true, true, false, false, true) == Warning::None);
    CHECK(WarningFor(Shown::Off, true, true, false, false, true) == Warning::None);

    for (int bits = 0; bits < 8; ++bits)
    {
        for (Shown shown : { Shown::NrOnly, Shown::NrAndFrameGeneration, Shown::MotionOnly })
            CHECK(WarningFor(shown, bits & 1, bits & 2, bits & 4, true) == Warning::GameUpscaler);

        CHECK(WarningFor(Shown::Off, bits & 1, bits & 2, bits & 4, true) == Warning::None);
    }

    // FG only (game's upscaler): the reverse rule. Vulkan games that call an upscaler only; its own key, which comes
    // before the motion keys; NR Pass at: off Finished Picture; Off clears it.
    {
        CHECK(Selectable(Mode::FrameGenerationOnly, true, true));
        CHECK(!Selectable(Mode::FrameGenerationOnly, false, true));
        CHECK(!Selectable(Mode::FrameGenerationOnly, true, false));
        CHECK(!Selectable(Mode::FrameGenerationOnly, false, false));
        CHECK(Offered(Mode::FrameGenerationOnly, true) && !Offered(Mode::FrameGenerationOnly, false));
        CHECK(Offered(Mode::NrOnly, false) && Offered(Mode::NrAndFrameGeneration, false) && Offered(Mode::Off, false));
        // The other modes keep their rule on Vulkan too
        CHECK(!Selectable(Mode::NrOnly, true, true) && Selectable(Mode::NrOnly, false, true));

        for (bool fp : { false, true })
        {
            const auto change = ForMode(Mode::FrameGenerationOnly, fp);
            CHECK(change.keys.has_value() && change.keys->frameGenerationOnly && !change.keys->motion &&
                  !change.keys->upscaler && !change.keys->input && !change.keys->depthFinder);
            CHECK(change.finishedPicture.has_value() == fp && change.retryAfterFailure == fp);
            CHECK(FromKeys(change.keys.value()) == Shown::FrameGenerationOnly);

            for (Mode other : { Mode::Off, Mode::NrOnly, Mode::NrAndFrameGeneration })
                CHECK(!ForMode(other, fp).keys->frameGenerationOnly);
        }

        Keys both = K(true, true, false, true);
        both.frameGenerationOnly = true;
        CHECK(FromKeys(both) == Shown::FrameGenerationOnly);
        CHECK(!ForClick(Mode::FrameGenerationOnly, Shown::FrameGenerationOnly, false).keys.has_value());
        CHECK(ForClick(Mode::Off, Shown::FrameGenerationOnly, false).keys.has_value());

        // Needs the game's upscaler call (where the other modes stand aside), then a D3D12 backend, then the bridge
        CHECK(WarningFor(Shown::FrameGenerationOnly, true, false, false, false) == Warning::NeedsGameUpscaler);
        CHECK(WarningFor(Shown::FrameGenerationOnly, true, false, false, true, false, false) == Warning::NeedsOn12Backend);
        CHECK(WarningFor(Shown::FrameGenerationOnly, true, false, false, true, true, true) == Warning::VulkanNeedsRestart);
        CHECK(WarningFor(Shown::FrameGenerationOnly, true, false, false, true, false, true) == Warning::None);
        CHECK(WarningFor(Shown::FrameGenerationOnly, false, false, true, true) == Warning::None); // NR off: still FG
    }

    // Which API the game is: the answer must not follow the D3D12 swapchain of a bridged Vulkan game (XeFG's presents
    // set the swapchain API to D3D12 every frame)
    {
        using A = GameApi;
        using P = PresentApi;
        using I = Interop;

        // The rule before the interop was part of it: Vulkan only while the last present was Vulkan's
        auto before = [](P present, bool d3d11Device)
        { return present == P::Vulkan ? A::Vulkan : (d3d11Device ? A::Dx11 : A::Dx12); };

        CHECK(GameApiFor(P::Dx11, I::None, true) == A::Dx11);
        CHECK(GameApiFor(P::Dx12, I::None, false) == A::Dx12);
        CHECK(GameApiFor(P::Vulkan, I::None, false) == A::Vulkan);
        // Vulkan bridged, right after XeFG's wrapped present wrote D3D12
        CHECK(GameApiFor(P::Dx12, I::VkwDx12, false) == A::Vulkan);
        CHECK(before(P::Dx12, false) != A::Vulkan); // what the menu showed
        CHECK(GameApiFor(P::Vulkan, I::VkwDx12, false) == A::Vulkan);
        // D3D11 bridged (the D3D12 swapchain is XeFG's; the game's device is D3D11)
        CHECK(GameApiFor(P::Dx12, I::Dx11wDx12, true) == A::Dx11);
        CHECK(GameApiFor(P::Dx11, I::Dx11wDx12, true) == A::Dx11);
        // Nothing seen yet: D3D12, as before
        CHECK(GameApiFor(P::NotSelected, I::None, false) == A::Dx12);
    }

    printf(fails == 0 ? "native mode: ok\n" : "native mode: %d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
