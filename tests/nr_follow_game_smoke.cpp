// Host check of DlssNr_FollowGame.h: the learned offset between Automatic's and the game's exposure. No GPU and no game needed.
// cl /std:c++20 /EHsc tests/nr_follow_game_smoke.cpp
#include "../OptiScaler/shaders/dlssnr/DlssNr_FollowGame.h"

#include <cstdio>
#include <string>
#include <utility>

using namespace DlssNrFollowGame;

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

static bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

int main()
{
    // RDR2 gameplay: Automatic ~1400 against the game's ~150, a steady -3.2 EV the other way round (auto is 9.2x).
    {
        Calibration c;
        for (unsigned i = 0; i < kWindow - 1; ++i)
            CHECK(!c.Feed(1400.0f, 150.0f));
        CHECK(!c.Locked());
        CHECK(c.Scale() == 1.0f);
        CHECK(c.Feed(1400.0f, 150.0f));
        CHECK(c.Locked());
        CHECK(Near(c.OffsetEv(), std::log2(1400.0f / 150.0f), 1e-4f));
        CHECK(Near(c.Scale(), 1400.0f / 150.0f, 1e-3f));
    }
    // Locked: later Feed readings do not move it (Track does, below).
    {
        Calibration c;
        for (unsigned i = 0; i < kWindow; ++i)
            c.Feed(1400.0f, 150.0f);
        const float locked = c.OffsetEv();
        for (unsigned i = 0; i < kWindow * 3; ++i)
            CHECK(!c.Feed(100.0f, 150.0f));
        CHECK(c.OffsetEv() == locked);
    }
    // A minority of loading-screen readings is outvoted by the median.
    {
        Calibration c;
        for (unsigned i = 0; i < kWindow; ++i)
            c.Feed(i % 5 == 0 ? 2.0f : 1400.0f, 150.0f);
        CHECK(Near(c.OffsetEv(), std::log2(1400.0f / 150.0f), 1e-4f));
    }
    // Broken readings are not taken: zero, negative, NaN, infinity, and offsets beyond kMaxEv.
    {
        Calibration c;
        c.Feed(0.0f, 150.0f);
        c.Feed(1400.0f, 0.0f);
        c.Feed(-1.0f, 150.0f);
        c.Feed(NAN, 150.0f);
        c.Feed(1400.0f, INFINITY);
        c.Feed(1e6f, 1.0f);
        CHECK(c.Readings() == 0);
        CHECK(!c.Locked());
    }
    // Reset starts over.
    {
        Calibration c;
        for (unsigned i = 0; i < kWindow; ++i)
            c.Feed(1400.0f, 150.0f);
        c.Reset();
        CHECK(!c.Locked());
        CHECK(c.Readings() == 0);
        CHECK(c.Scale() == 1.0f);
    }

    // Track: a locked offset of 0 EV (Automatic = the game), readings every 16 ms at `ev` for `ms`, from `t`.
    auto lock = [](Calibration& c) {
        for (unsigned i = 0; i < kWindow; ++i)
            c.Feed(100.0f, 100.0f);
    };
    auto hold = [](Calibration& c, float ev, unsigned long long& t, unsigned long long ms, bool held = false) {
        int started = 0, settled = 0;
        for (const unsigned long long end = t + ms; t < end; t += 16)
        {
            const TrackEvent e = c.Track(100.0f * std::exp2(ev), 100.0f, t, held);
            started += e.started ? 1 : 0;
            settled += e.settled ? 1 : 0;
        }
        return std::pair { started, settled };
    };

    // Inside the band (0.5 EV apart for 30 s): nothing moves.
    {
        Calibration c;
        lock(c);
        unsigned long long t = 1000;
        const auto [started, settled] = hold(c, 0.5f, t, 30000);
        CHECK(c.OffsetEv() == 0.0f && started == 0 && settled == 0 && !c.Easing());
    }

    // A flash or a camera cut (1.5 EV apart for 1 s, then back): shorter than kTrackPersistMs, nothing moves.
    {
        Calibration c;
        lock(c);
        unsigned long long t = 1000;
        hold(c, 1.5f, t, 1000);
        hold(c, 0.0f, t, 5000);
        CHECK(c.OffsetEv() == 0.0f);
    }

    // Staying 1.5 EV apart: after the wait it eases, no faster than kTrackRateEvPerSecond, and settles within kTrackStopEv.
    {
        Calibration c;
        lock(c);
        unsigned long long t = 1000;
        hold(c, 1.5f, t, 2500); // the smoothing (0.5 s) and the wait (1.5 s), plus a little
        const float early = c.OffsetEv();
        CHECK(early > 0.0f && early <= kTrackRateEvPerSecond * 1.0f + 1e-3f);
        CHECK(c.Easing());
        const auto [started, settled] = hold(c, 1.5f, t, 10000);
        CHECK(settled == 1 && !c.Easing());
        CHECK(Near(c.OffsetEv(), 1.5f, kTrackStopEv));
        CHECK(c.OffsetEv() < 1.5f); // it stops short, it does not overshoot
        // Settled: inside the band again, it stays put.
        const float now = c.OffsetEv();
        hold(c, 1.5f, t, 10000);
        CHECK(c.OffsetEv() == now);
    }

    // Held (Tune is measuring): nothing moves, and the disagreement does not carry over once released.
    {
        Calibration c;
        lock(c);
        unsigned long long t = 1000;
        hold(c, 1.5f, t, 10000, true);
        CHECK(c.OffsetEv() == 0.0f && !c.Easing());
        hold(c, 1.5f, t, 1000);
        CHECK(c.OffsetEv() == 0.0f); // the wait starts again after the hold
    }

    // The other way too, and broken readings are ignored.
    {
        Calibration c;
        lock(c);
        unsigned long long t = 1000;
        c.Track(NAN, 100.0f, t, false);
        c.Track(100.0f, 0.0f, t, false);
        c.Track(1e9f, 1.0f, t, false);
        hold(c, -1.2f, t, 15000);
        CHECK(Near(c.OffsetEv(), -1.2f, kTrackStopEv) && c.OffsetEv() > -1.2f);
    }

    // A gap in the readings (a pause, alt-tab) is not a disagreement that lasted: a 0.6 s flash, 5 s of nothing, then
    // agreement again -- nothing moves (without the gap check the wait was already over and it eased 0.15 EV).
    {
        Calibration c;
        lock(c);
        unsigned long long t = 1000;
        hold(c, 2.0f, t, 600);
        t += 5000;
        const auto [started, settled] = hold(c, 0.0f, t, 5000);
        CHECK(started == 0 && c.OffsetEv() == 0.0f && !c.Easing());
    }

    // An ease stopped on the way (a Tune starts, or the readings stop) says so, from where it began to where it is.
    {
        Calibration c;
        lock(c);
        unsigned long long t = 1000;
        hold(c, 1.5f, t, 3000);
        CHECK(c.Easing());
        const float now = c.OffsetEv();
        const TrackEvent e = c.Track(100.0f * std::exp2(1.5f), 100.0f, t, true);
        CHECK(e.stopped && std::string(e.why) == "a Tune is measuring" && Near(e.fromEv, 0.0f, 1e-6f) &&
              Near(e.toEv, now, 1e-6f) && !c.Easing());
        hold(c, 1.5f, t, 3000);
        CHECK(c.Easing());
        t += 1000;
        const TrackEvent g = c.Track(100.0f * std::exp2(1.5f), 100.0f, t, false);
        CHECK(g.stopped && std::string(g.why) == "the readings stopped" && !c.Easing());
    }

    // A game whose own exposure moves (RDR2) keeps its learned calibration: no easing, however long the disagreement --
    // a menu freezes its exposure too, so the median is what keeps menus and fades out.
    {
        Calibration c;
        lock(c);
        unsigned long long t = 1000;
        int moves = 0;
        for (const unsigned long long end = t + 2000; t < end; t += 16) // the game's exposure walks 1 EV
        {
            const float game = 100.0f * std::exp2((float) (t - 1000) / 2000.0f);
            moves += c.Track(game, game, t, false).gameMoves ? 1 : 0;
        }
        CHECK(moves == 1 && c.GameMoves());
        int started = 0;
        for (const unsigned long long end = t + 20000; t < end; t += 16) // then 20 s of a menu 2 EV away
            started += c.Track(400.0f * std::exp2(1.0f), 100.0f * std::exp2(1.0f), t, false).started ? 1 : 0;
        CHECK(started == 0 && c.OffsetEv() == 0.0f);
        c.Reset();
        CHECK(!c.GameMoves());
    }
    // NBA 2K27: its exposure is 1 while it loads and 1.3195 from then on -- one change, not adapting: it still eases.
    {
        Calibration c;
        lock(c);
        unsigned long long t = 1000;
        int moves = 0, started = 0;
        for (const unsigned long long end = t + 500; t < end; t += 16)
            moves += c.Track(100.0f, 100.0f, t, false).gameMoves ? 1 : 0; // loading: game exposure 1
        const float game = 100.0f / 1.3195f;                               // gameplay: 1.3195, 0.4 EV away
        for (const unsigned long long end = t + 10000; t < end; t += 16)
        {
            const TrackEvent e = c.Track(game * std::exp2(1.5f), game, t, false);
            moves += e.gameMoves ? 1 : 0;
            started += e.started ? 1 : 0;
        }
        CHECK(moves == 0 && !c.GameMoves() && started == 1 && c.OffsetEv() > 1.0f);
    }

    // Not locked: Track does nothing; Reset clears its state.
    {
        Calibration c;
        unsigned long long t = 1000;
        hold(c, 2.0f, t, 10000);
        CHECK(!c.Locked() && c.OffsetEv() == 0.0f);
        Calibration d;
        lock(d);
        hold(d, 2.0f, t, 2500);
        CHECK(d.Easing());
        d.Reset();
        CHECK(!d.Easing() && !d.Locked());
    }

    printf(fails ? "FAILED: %d\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
