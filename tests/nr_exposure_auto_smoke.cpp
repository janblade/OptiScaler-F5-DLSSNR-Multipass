// Host check of DlssNr_ExposureAuto.h (Auto's exposure source ladder and the exposure scan's lifecycle). No GPU and no
// game needed.
// cl /std:c++20 /EHsc /W4 tests/nr_exposure_auto_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_ExposureAuto.h"

#include <cmath>
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

    // A handed texture not yet read clean: keep watching (and holding) -- Game exposure may give up on it, and the game
    // made its candidates at load. Read clean: done. Given up on: search what was held
    {
        Lifecycle l;
        Frame pending = At(0.0f);
        pending.handedPending = true;
        l.OnFrame(pending);
        CHECK(l.Now() == Phase::Watching && !l.LetsGo() && !l.Closed());
        pending.seconds = 5.0f;
        l.OnFrame(pending);
        CHECK(l.Now() == Phase::Watching);
        l.OnFrame(At(10.0f)); // given up on: neither handed nor pending
        CHECK(l.Now() == Phase::Searching && l.SearchNumber() == 1);

        Lifecycle k;
        k.OnFrame(pending);
        Frame clean = At(6.0f);
        clean.handed = true;
        k.OnFrame(clean);
        CHECK(k.Now() == Phase::Done && k.LetsGo() && k.Closed());
    }

    // Not a D3D12 game: not here, nothing held, nothing taken
    {
        Lifecycle l;
        Frame f = At(0.0f);
        f.supported = false;
        l.OnFrame(f);
        CHECK(l.Now() == Phase::NotHere && l.LetsGo() && l.Closed() && !l.CopiesAll());
    }

    // Given up: new candidates wait out the retry time, doubled with each search given up
    {
        Lifecycle l(Settings { 300.0f, 60.0f, 1800.0f });
        l.OnFrame(At(10.0f));
        l.OnFrame(At(310.0f));
        CHECK(l.Now() == Phase::GaveUp && !l.Closed() && !l.CopiesAll() && l.RetrySeconds() == 60.0f);
        Frame fresh = At(330.0f);
        fresh.newCandidates = true;
        l.OnFrame(fresh);
        CHECK(l.Now() == Phase::GaveUp); // held, not searched yet
        l.OnFrame(At(369.0f));
        CHECK(l.Now() == Phase::GaveUp);
        l.OnFrame(At(370.0f)); // remembered, no new candidate needed on this frame
        CHECK(l.Now() == Phase::Searching && l.SearchNumber() == 2);
        l.OnFrame(At(670.0f));
        CHECK(l.Now() == Phase::GaveUp && l.RetrySeconds() == 120.0f);
        fresh.seconds = 680.0f;
        l.OnFrame(fresh);
        l.OnFrame(At(789.0f));
        CHECK(l.Now() == Phase::GaveUp);
        l.OnFrame(At(790.0f));
        CHECK(l.Now() == Phase::Searching);
        // without new candidates a give-up stays given up
        l.OnFrame(At(1090.0f));
        l.OnFrame(At(5000.0f));
        CHECK(l.Now() == Phase::GaveUp && l.RetrySeconds() == 240.0f);
    }

    // Done and Baked take no candidate; Found and GaveUp do (held for the next search)
    {
        Lifecycle l;
        l.OnFrame(At(0.0f));
        CHECK(!l.Closed());
        Frame f = At(10.0f);
        f.verdict = Verdict::Found;
        l.OnFrame(f);
        CHECK(l.Now() == Phase::Found && !l.Closed());
    }

    // The pre-exposure rung: Dawnwalker's moves with the light; a constant one, a single change (a setting) or a few
    // values switched between (gameplay and cutscenes) do not
    {
        PreExposureMotion dawnwalker;
        for (float pre : { 1.0f, 1.0f, 0.59f, 0.45f, 0.40f })
            dawnwalker.Note(pre);
        CHECK(!dawnwalker.Moving()); // a second of it is not enough yet
        for (int i = 0; i < 120; ++i) // then the game's eye adaptation follows the light, both ways
            dawnwalker.Note(0.4f * std::exp2(1.5f * std::sin((float) i * 0.05f)));
        CHECK(dawnwalker.Moving());

        PreExposureMotion toggles;
        for (int i = 0; i < 1000; ++i)
            toggles.Note((i / 50) % 2 ? 1.0f : 0.5f);
        CHECK(!toggles.Moving());

        PreExposureMotion three;
        for (int i = 0; i < 1000; ++i)
            three.Note((i / 30) % 3 == 0 ? 1.0f : (i / 30) % 3 == 1 ? 0.5f : 0.25f);
        CHECK(!three.Moving());

        PreExposureMotion still;
        for (int i = 0; i < 1000; ++i)
            still.Note(1.0f);
        CHECK(!still.Moving());

        PreExposureMotion once;
        for (int i = 0; i < 1000; ++i)
            once.Note(i < 500 ? 1.0f : 0.5f);
        CHECK(!once.Moving());

        PreExposureMotion noise; // jitter under 1% is not motion
        for (int i = 0; i < 1000; ++i)
            noise.Note(i % 2 ? 1.0f : 1.005f);
        CHECK(!noise.Moving());

        PreExposureMotion junk;
        for (float pre : { 0.0f, -1.0f, std::nanf(""), 1.0f })
            junk.Note(pre);
        CHECK(!junk.Moving());
    }

    printf(fails == 0 ? "nr_exposure_auto_smoke: PASS\n" : "nr_exposure_auto_smoke: %d FAIL\n", fails);
    return fails == 0 ? 0 : 1;
}
