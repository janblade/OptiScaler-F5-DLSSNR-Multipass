// Host check of DlssNr_ExposureAuto.h (Auto's exposure source ladder and the exposure scan's lifecycle). No GPU and no
// game needed.
// cl /std:c++20 /EHsc /W4 tests/nr_exposure_auto_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_ExposureAuto.h"

#include <cstdio>
#include <initializer_list>

using namespace DlssNrExposureAuto;

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

static Frame At(float seconds)
{
    Frame f;
    f.seconds = seconds;
    return f;
}

int main()
{
    // The ladder: first that holds
    CHECK(Ladder(true, true, Phase::Found) == Rung::Handed);
    CHECK(Ladder(false, true, Phase::Found) == Rung::PreExposure);
    CHECK(Ladder(false, false, Phase::Found) == Rung::Found);
    CHECK(Ladder(false, false, Phase::Baked) == Rung::Baked);
    for (Phase p : { Phase::Watching, Phase::Done, Phase::Searching, Phase::GaveUp })
        CHECK(Ladder(false, false, p) == Rung::Automatic);

    // Before NR: watching, copying (the game makes its candidates at load)
    {
        Lifecycle l;
        CHECK(l.Now() == Phase::Watching);
        CHECK(l.CopiesAll() && !l.LetsGo());
    }

    // NBA 2K27 / RDR2: a texture handed over -> done, every candidate let go at once
    {
        Lifecycle l;
        Frame f = At(0.0f);
        f.handed = true;
        l.OnFrame(f);
        CHECK(l.Now() == Phase::Done);
        CHECK(l.LetsGo() && !l.CopiesAll() && !l.CopiesMatchOnly());
    }

    // Dawnwalker: a moving pre-exposure is as good as a texture
    {
        Lifecycle l;
        Frame f = At(0.0f);
        f.preExposure = true;
        l.OnFrame(f);
        CHECK(l.Now() == Phase::Done);
    }

    // Cyberpunk 2077: nothing handed -> search, then found -> copy the match only
    {
        Lifecycle l;
        l.OnFrame(At(0.0f));
        CHECK(l.Now() == Phase::Searching && l.CopiesAll());
        CHECK(l.SearchNumber() == 1);
        l.OnFrame(At(100.0f)); // not sure yet
        CHECK(l.Now() == Phase::Searching);
        Frame f = At(120.0f);
        f.verdict = Verdict::Found;
        l.OnFrame(f);
        CHECK(l.Now() == Phase::Found);
        CHECK(l.CopiesMatchOnly() && !l.CopiesAll() && !l.LetsGo());

        // the match goes (a feature release let go of it, the game remade it): search again, from no history
        Frame gone = At(200.0f);
        gone.matchAlive = false;
        l.OnFrame(gone);
        CHECK(l.Now() == Phase::Searching);
        CHECK(l.SearchNumber() == 2);
    }

    // The colour already exposed -> baked, everything let go, for the session
    {
        Lifecycle l;
        l.OnFrame(At(0.0f));
        Frame f = At(50.0f);
        f.verdict = Verdict::Baked;
        l.OnFrame(f);
        CHECK(l.Now() == Phase::Baked && l.LetsGo());
        l.OnFrame(At(5000.0f));
        CHECK(l.Now() == Phase::Baked);
    }

    // No verdict (or None) for searchSeconds -> gave up, Automatic; new candidates start a new search
    {
        Lifecycle l(Settings { 300.0f });
        l.OnFrame(At(10.0f));
        Frame none = At(200.0f);
        none.verdict = Verdict::None;
        l.OnFrame(none);
        CHECK(l.Now() == Phase::Searching); // None is still searching until the time is up: more light may come
        l.OnFrame(At(309.0f));
        CHECK(l.Now() == Phase::Searching);
        l.OnFrame(At(310.0f));
        CHECK(l.Now() == Phase::GaveUp && l.LetsGo());
        l.OnFrame(At(400.0f));
        CHECK(l.Now() == Phase::GaveUp);
        Frame loading = At(500.0f);
        loading.newCandidates = true;
        l.OnFrame(loading);
        CHECK(l.Now() == Phase::Searching && l.SearchNumber() == 2);
        l.OnFrame(At(799.0f));
        CHECK(l.Now() == Phase::Searching); // the new search has its own time
        l.OnFrame(At(800.0f));
        CHECK(l.Now() == Phase::GaveUp);
    }

    // Handed later (the first calls carried no texture): done, whatever the scan was doing
    {
        Lifecycle l;
        l.OnFrame(At(0.0f));
        Frame f = At(30.0f);
        f.verdict = Verdict::Found;
        l.OnFrame(f);
        Frame handed = At(40.0f);
        handed.handed = true;
        l.OnFrame(handed);
        CHECK(l.Now() == Phase::Done && l.LetsGo());
    }

    printf(fails == 0 ? "nr_exposure_auto_smoke: PASS\n" : "nr_exposure_auto_smoke: %d FAIL\n", fails);
    return fails == 0 ? 0 : 1;
}
