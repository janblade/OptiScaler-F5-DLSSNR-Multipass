// Host test for dlssnr/DlssNrSceneCut.h: what DLSS-NR's game input makes of a scene cut the detector found.
//
//   cl /std:c++20 /EHsc /W4 tests\nr_scene_cut_policy_smoke.cpp /I OptiScaler

#include <dlssnr/DlssNrSceneCut.h>

#include <cstdio>
#include <initializer_list>

using namespace DlssNrSceneCut;

namespace
{
int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += ok ? 0 : 1;
}
} // namespace

int main()
{
    Check(ModeFrom(0) == Mode::Off && ModeFrom(1) == Mode::Count && ModeFrom(2) == Mode::Act && ModeFrom(7) == Mode::Act,
          "the setting's values");

    {
        Policy p;
        const Outcome o = p.Found(100, 103, Mode::Count);
        Check(!o.gameFlagged && !o.reset && p.silent == 1 && p.resets == 0, "Count: a silent cut is counted, not acted on");
    }

    {
        Policy p;
        const Outcome o = p.Found(100, 103, Mode::Act);
        Check(!o.gameFlagged && o.reset && p.resets == 1, "Act: a silent cut resets the model");
    }

    for (const unsigned long long at : { 99ull, 100ull, 101ull })
    {
        Policy p;
        p.GameReset(at);
        const Outcome o = p.Found(100, 103, Mode::Act);
        Check(o.gameFlagged && !o.reset && p.flagged == 1, "a game reset a frame before, on or after the cut: flagged, no reset");
    }

    {
        Policy p;
        p.GameReset(97);
        const Outcome o = p.Found(100, 103, Mode::Act);
        Check(!o.gameFlagged && o.reset, "a game reset three frames before the cut is another one");
    }

    {
        Policy p;
        p.GameReset(102);
        const Outcome o = p.Found(100, 103, Mode::Act);
        Check(!o.gameFlagged && !o.reset, "the game reset after the cut but before the answer arrived: not flagged, no reset");
    }

    {
        Policy p;
        const Outcome first = p.Found(100, 102, Mode::Act);
        const Outcome back = p.Found(102, 104, Mode::Act); // a white flash: into it and out again
        const Outcome later = p.Found(110, 112, Mode::Act);
        Check(first.reset && !back.reset && later.reset && p.resets == 2,
              "at most one reset in kQuiet evaluates; the next cut after that resets again");
    }

    {
        Policy p;
        p.GameReset(100);
        p.Forget();
        const Outcome o = p.Found(100, 103, Mode::Act);
        Check(!o.gameFlagged && o.reset, "Forget() drops the remembered game resets");
    }

    {
        Policy p;
        for (unsigned long long e = 0; e < 40; ++e)
            p.GameReset(e);
        const Outcome o = p.Found(30, 33, Mode::Act);
        Check(o.gameFlagged, "the last sixteen game resets are remembered");
    }

    std::printf(g_failures == 0 ? "all passed\n" : "%d FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
