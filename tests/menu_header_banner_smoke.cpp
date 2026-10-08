// Host test for menu/HeaderBanner.h: which header line and button each upscaler state gets.
//
// Build: cl /nologo /std:c++20 /EHsc /W4 tests\menu_header_banner_smoke.cpp
#include <cstdio>
#include "../OptiScaler/menu/HeaderBanner.h"

using namespace HeaderBanner;
using DlssNrNativeMode::Shown;

static int fails = 0;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("FAILED line %d: %s\n", __LINE__, #cond);                                                      \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

static Inputs Make(Feature f, bool files, bool gameCalls, Shown mode, bool nr, bool nrOnlyRunning = false)
{
    Inputs in;
    in.feature = f;
    in.upscalerFiles = files;
    in.gameCallsUpscaler = gameCalls;
    in.mode = mode;
    in.nrAvailable = nr;
    in.f5lowNrOnlyRunning = nrOnlyRunning;
    return in;
}

int main()
{
    // Offers: no feature, mode off, NR available, the game does not call an upscaler.
    {
        const Banner b = Decide(Make(Feature::None, true, false, Shown::Off, true));
        CHECK(b.line == Line::OfferWithFiles && b.action == Action::UseF5Low);
    }
    {
        const Banner b = Decide(Make(Feature::None, false, false, Shown::Off, true));
        CHECK(b.line == Line::OfferNoFiles && b.action == Action::UseF5Low);
    }

    // NR not available: today's lines, no offer.
    {
        const Banner b = Decide(Make(Feature::None, true, false, Shown::Off, false));
        CHECK(b.line == Line::SelectUpscaler && b.action == Action::None);
    }
    {
        const Banner b = Decide(Make(Feature::None, false, false, Shown::Off, false));
        CHECK(b.line == Line::NoFiles && b.action == Action::None);
    }

    // The game calls an upscaler but no feature is current yet: no offer either.
    {
        const Banner b = Decide(Make(Feature::None, true, true, Shown::Off, true));
        CHECK(b.line == Line::SelectUpscaler && b.action == Action::None);
    }

    // Optical F5Low's virtual upscaler running: the line names only what runs with it.
    {
        Inputs in = Make(Feature::F5Low, true, false, Shown::NrAndFrameGeneration, true);
        in.nrEnabled = true;
        in.frameGeneration = true;
        Banner b = Decide(in);
        CHECK(b.line == Line::F5LowNrAndFrameGen && b.action == Action::F5LowSettings);

        in.frameGeneration = false;
        CHECK(Decide(in).line == Line::F5LowNrNoFrameGen);

        in.nrEnabled = false;
        in.frameGeneration = true;
        CHECK(Decide(in).line == Line::F5LowFrameGenNoNr);

        in.frameGeneration = false;
        b = Decide(in);
        CHECK(b.line == Line::F5LowStabiliser && b.action == Action::F5LowSettings);
    }
    // ... whatever else is true: the feature wins.
    {
        const Banner b = Decide(Make(Feature::F5Low, false, true, Shown::Off, false));
        CHECK(b.line == Line::F5LowStabiliser && b.action == Action::F5LowSettings);
    }

    // The one-time notice: real time with the offer standing, a long stall counts little, and the count restarts
    // whenever the offer goes away (a game upscaler call, a mode switched on).
    {
        double quiet = 0.0;

        for (int i = 0; i < 59 * 60; ++i)
            quiet = HintQuietAfter(quiet, true, 1000.0 / 60.0);

        CHECK(!HintDue(quiet));

        for (int i = 0; i < 2 * 60; ++i)
            quiet = HintQuietAfter(quiet, true, 1000.0 / 60.0);

        CHECK(HintDue(quiet));

        // A 30 s loading stall adds at most one capped step
        quiet = HintQuietAfter(0.0, true, 30000.0);
        CHECK(quiet <= kHintStepCapMs);

        // The offer gone: back to zero
        CHECK(HintQuietAfter(50000.0, false, 16.0) == 0.0);

        // The same seconds at 30 and 144 fps
        double q30 = 0.0, q144 = 0.0;
        for (int i = 0; i < 30 * 61; ++i)
            q30 = HintQuietAfter(q30, true, 1000.0 / 30.0);
        for (int i = 0; i < 144 * 61; ++i)
            q144 = HintQuietAfter(q144, true, 1000.0 / 144.0);
        CHECK(HintDue(q30) && HintDue(q144));
    }

    // NR only running, and waiting.
    {
        const Banner b = Decide(Make(Feature::None, true, false, Shown::NrOnly, true, true));
        CHECK(b.line == Line::F5LowNrOnly && b.action == Action::F5LowSettings);
    }
    {
        const Banner b = Decide(Make(Feature::None, true, false, Shown::NrOnly, true, false));
        CHECK(b.line == Line::F5LowStatus && b.action == Action::F5LowSettings);
    }
    {
        const Banner b = Decide(Make(Feature::None, true, false, Shown::NrAndFrameGeneration, true));
        CHECK(b.line == Line::F5LowStatus && b.action == Action::F5LowSettings);
    }
    {
        const Banner b = Decide(Make(Feature::None, false, false, Shown::MotionOnly, false));
        CHECK(b.line == Line::F5LowStatus);
    }

    // A mode on while the game calls an upscaler: stands aside, no button.
    {
        const Banner b = Decide(Make(Feature::None, true, true, Shown::NrOnly, true, true));
        CHECK(b.line == Line::F5LowStandsAside && b.action == Action::None);
    }

    // A game's feature: frozen keeps its line, running is blank; neither offers Optical F5Low, in any mode.
    for (const Shown mode : { Shown::Off, Shown::NrOnly, Shown::NrAndFrameGeneration, Shown::MotionOnly })
    {
        const Banner frozen = Decide(Make(Feature::Frozen, true, true, mode, true));
        CHECK(frozen.line == Line::Frozen && frozen.action == Action::None);
        const Banner game = Decide(Make(Feature::Game, true, true, mode, true));
        CHECK(game.line == Line::Blank && game.action == Action::None);
    }

    if (fails == 0)
        std::printf("menu_header_banner_smoke: ok\n");

    return fails == 0 ? 0 : 1;
}
