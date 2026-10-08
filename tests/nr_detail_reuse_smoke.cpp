// Host check of DlssNrDetailReuse.h: which frames run the full model and which reuse the last frame's detail.
// No GPU and no game needed.
// cl /std:c++20 /EHsc tests/nr_detail_reuse_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNrDetailReuse.h"

#include <cstdio>

using namespace DlssNrDetailReuse;

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

static FrameFacts Facts(unsigned long long frame, unsigned long long revision = 1)
{
    FrameFacts f;
    f.enabled = true;
    f.frame = frame;
    f.revision = revision;
    return f;
}

// One full frame, then one reuse frame, over and over. Every frame captures; history is usable from the
// second frame on; the full frame after a reuse frame composes motion.
static void Alternates()
{
    Cadence c;
    Decision d = c.Next(Facts(10));
    CHECK(d.kind == Kind::Full && d.captureHistory && !d.historyUsable && !d.composeMotion && !d.saveMotion);
    c.Captured(true);

    d = c.Next(Facts(11));
    CHECK(d.kind == Kind::Reuse && d.captureHistory && d.historyUsable && d.saveMotion);
    c.Captured(true);

    d = c.Next(Facts(12));
    CHECK(d.kind == Kind::Full && d.captureHistory && d.historyUsable && d.composeMotion && !d.saveMotion);
    c.Captured(true);

    d = c.Next(Facts(13));
    CHECK(d.kind == Kind::Reuse && d.historyUsable);

    CHECK(c.Full() == 2 && c.Reused() == 2 && c.Fallback() == 0);
}

// Never two reuse frames in a row, and an unconfirmed capture leaves nothing to reuse.
static void NoDoubleReuse()
{
    Cadence c;
    c.Next(Facts(1));
    c.Captured(true);
    CHECK(c.Next(Facts(2)).kind == Kind::Reuse);
    c.Captured(true);
    CHECK(c.Next(Facts(3)).kind == Kind::Full);
    // no Captured: the history of frame 3 was never confirmed
    const Decision d = c.Next(Facts(4));
    CHECK(d.kind == Kind::Full && !d.historyUsable);
}

// A failed capture leaves no history to reuse.
static void FailedCaptureDropsHistory()
{
    Cadence c;
    c.Next(Facts(1));
    c.Captured(false);
    const Decision d = c.Next(Facts(2));
    CHECK(d.kind == Kind::Full && d.captureHistory && !d.historyUsable && !d.composeMotion);
}

// Anything that invalidates the saved detail forces the full model, makes the history unusable and counts as a
// fallback.
static void Invalidations()
{
    const auto afterOneFull = [](Cadence& c)
    {
        c.Next(Facts(100));
        c.Captured(true);
    };

    {
        Cadence c;
        afterOneFull(c);
        FrameFacts f = Facts(101);
        f.reset = true;
        const Decision d = c.Next(f);
        CHECK(d.kind == Kind::Full && d.captureHistory && !d.historyUsable && !d.composeMotion);
        CHECK(c.Fallback() == 1 && c.Causes().reset == 1 && c.Causes().gap == 0);
    }
    {
        Cadence c;
        afterOneFull(c);
        const Decision d = c.Next(Facts(101, 2)); // settings changed
        CHECK(d.kind == Kind::Full && !d.historyUsable);
        CHECK(c.Fallback() == 1 && c.Causes().changed == 1);
    }
    {
        Cadence c;
        afterOneFull(c);
        const Decision d = c.Next(Facts(103)); // NR did not run on 101 and 102
        CHECK(d.kind == Kind::Full && !d.historyUsable);
        CHECK(c.Fallback() == 1 && c.Causes().gap == 1 && c.Causes().lastGapStep == 3 && c.Causes().lastMaxStep == 1);
    }
    {
        Cadence c;
        afterOneFull(c);
        FrameFacts f = Facts(101);
        f.blocked = true; // calibration, frame hold
        CHECK(c.Next(f).kind == Kind::Full);
        c.Captured(true);
        f = Facts(102);
        f.blocked = true;
        CHECK(c.Next(f).kind == Kind::Full); // blocked frames never reuse
        c.Captured(true);
        // The first frame after runs the model too: a held frame saved the frozen picture.
        Decision d = c.Next(Facts(103));
        CHECK(d.kind == Kind::Full && !d.historyUsable);
        c.Captured(true);
        d = c.Next(Facts(104));
        CHECK(d.kind == Kind::Reuse && d.historyUsable); // and reuse resumes
    }
}

// A reuse frame followed by an invalidation still composes motion when the model keeps its history:
// NGX last evaluated two frames ago.
static void ComposeAfterReuseEvenOnFallback()
{
    Cadence c;
    c.Next(Facts(1));
    c.Captured(true);
    CHECK(c.Next(Facts(2)).kind == Kind::Reuse);
    c.Captured(true);
    Decision d = c.Next(Facts(3, 2)); // settings changed, no model reset
    CHECK(d.kind == Kind::Full && d.composeMotion && !d.historyUsable);

    Cadence r;
    r.Next(Facts(1));
    r.Captured(true);
    r.Next(Facts(2));
    r.Captured(true);
    FrameFacts f = Facts(3);
    f.reset = true; // the model starts over: nothing to align with
    d = r.Next(f);
    CHECK(d.kind == Kind::Full && !d.composeMotion);
}

// Drop after a reuse frame that could not be recorded: the next frame is full, with nothing to reuse or compose.
static void DropStartsClean()
{
    Cadence c;
    c.Next(Facts(1));
    c.Captured(true);
    CHECK(c.Next(Facts(2)).kind == Kind::Reuse);
    c.Drop();
    const Decision d = c.Next(Facts(3));
    CHECK(d.kind == Kind::Full && !d.historyUsable && !d.composeMotion);
}

// A reuse frame that could not be recorded counts as a fallback, and the next frame starts clean.
static void ReuseFailedCounts()
{
    Cadence c;
    c.Next(Facts(1));
    c.Captured(true);
    CHECK(c.Next(Facts(2)).kind == Kind::Reuse);
    c.ReuseFailed();
    CHECK(c.Reused() == 0 && c.Fallback() == 1);
    const Decision d = c.Next(Facts(3));
    CHECK(d.kind == Kind::Full && !d.historyUsable && !d.composeMotion);
}

// Off: always full, no capture, no counters, and turning it on starts clean.
static void Disabled()
{
    Cadence c;
    c.Next(Facts(1));
    c.Captured(true);
    FrameFacts off = Facts(2);
    off.enabled = false;
    const Decision d = c.Next(off);
    CHECK(d.kind == Kind::Full && !d.captureHistory && !d.historyUsable && !d.composeMotion && !d.saveMotion);
    c.Captured(true); // ignored: nothing was asked
    CHECK(c.Next(Facts(3)).kind == Kind::Full); // no history survives an off frame
    CHECK(c.Fallback() == 0);
}

// A present counter steps by more than one under frame generation: within maxStep that is no gap.
static void StepTolerance()
{
    Cadence c;
    FrameFacts f = Facts(10);
    f.maxStep = 4;
    c.Next(f);
    c.Captured(true);
    f.frame = 12;
    CHECK(c.Next(f).kind == Kind::Reuse);
    c.Captured(true);
    f.frame = 16;
    CHECK(c.Next(f).kind == Kind::Full && c.Fallback() == 0);
    c.Captured(true);
    f.frame = 21; // 5 > maxStep: NR did not run in between
    CHECK(c.Next(f).kind == Kind::Full && c.Fallback() == 1);
    c.Captured(true);
    f.frame = 21; // the same frame again is not continuous either
    CHECK(c.Next(f).kind == Kind::Full && c.Fallback() == 2);
    CHECK(c.Causes().gap == 2 && c.Causes().lastGapStep == 0 && c.Causes().lastMaxStep == 4);
}

// Held off while the picture moves too fast: the frame runs the model, its history stays usable (so the measurement
// carries on and the next frame may reuse again) and it counts as held, not as a fallback.
static void HeldWhileMoving()
{
    Cadence c;
    c.Next(Facts(1));
    c.Captured(true);
    FrameFacts f = Facts(2);
    f.hold = true;
    Decision d = c.Next(f);
    CHECK(d.kind == Kind::Full && d.captureHistory && d.historyUsable && !d.composeMotion && !d.saveMotion);
    CHECK(c.Held() == 1 && c.Fallback() == 0 && c.Reused() == 0);
    c.Captured(true);

    f = Facts(3);
    f.hold = true;
    CHECK(c.Next(f).kind == Kind::Full && c.Held() == 2 && c.Fallback() == 0);
    c.Captured(true);

    // Released: the next frame reuses again, with no fallback in between.
    d = c.Next(Facts(4));
    CHECK(d.kind == Kind::Reuse && d.historyUsable);
    CHECK(c.Held() == 2 && c.Fallback() == 0 && c.Reused() == 1);
}

// Held counts only frames a reuse was taken away from: a frame that would run the model anyway is not one, and an
// invalid history is a fallback, not a hold.
static void HeldCountsOnlyLostReuse()
{
    Cadence c;
    FrameFacts f = Facts(1);
    f.hold = true;
    CHECK(c.Next(f).kind == Kind::Full && c.Held() == 0); // nothing saved yet
    c.Captured(true);
    f = Facts(2);
    f.hold = true;
    CHECK(c.Next(f).kind == Kind::Full && c.Held() == 1);
    c.Captured(true);

    Cadence r;
    r.Next(Facts(10));
    r.Captured(true);
    f = Facts(11);
    f.hold = true;
    f.reset = true; // the model starts over: a fallback, whatever the motion
    CHECK(r.Next(f).kind == Kind::Full && r.Fallback() == 1 && r.Held() == 0);
}

// Reuse pauses while too much of the picture arrives with no detail to move, and comes back only after the share has
// stayed at or under the threshold for the whole wait.
static void Motion()
{
    const float max = 0.05f;
    const double frame = 1.0 / 30.0;

    MotionGuard g;
    CHECK(!g.Update(-1.0f, max, frame)); // no reading yet
    CHECK(!g.Update(0.02f, max, frame)); // calm
    CHECK(g.Update(0.09f, max, frame));  // over the threshold: paused
    CHECK(g.Update(-1.0f, max, frame));  // a frame with no reading keeps the state

    // At or under the threshold, but only after the whole wait of it.
    for (int i = 0; i < 8; ++i)
        CHECK(g.Update(0.01f, max, frame)); // 8 frames = 0.27 s
    CHECK(!g.Update(0.01f, max, frame));    // 0.30 s: back on

    // A burst inside that window starts the wait over.
    CHECK(g.Update(0.2f, max, frame));
    for (int i = 0; i < 8; ++i)
        g.Update(0.01f, max, frame);
    CHECK(g.Update(0.2f, max, frame));
    for (int i = 0; i < 8; ++i)
        CHECK(g.Update(0.01f, max, frame));
    CHECK(!g.Update(0.01f, max, frame));

    // A share that settles just under the threshold must still let reuse back: with a second, lower level to resume at,
    // anything in between would pause for ever, which is what steady moderate motion does (Witcher 3: 24 of 149
    // windows sat between 5% and 10% of a 10% threshold).
    {
        MotionGuard band;
        CHECK(band.Update(0.09f, max, frame)); // paused
        for (int i = 0; i < 8; ++i)
            CHECK(band.Update(0.04f, max, frame)); // just under the threshold, not yet long enough
        CHECK(!band.Update(0.04f, max, frame));    // 0.30 s of it: back on
    }

    // A reading every frame does not look like a measurement that stopped arriving, however long the pause lasts.
    {
        MotionGuard busy;
        CHECK(busy.Update(0.9f, max, frame));
        for (int i = 0; i < 60; ++i) // two seconds of readings, all far too high to resume
            CHECK(busy.Update(0.9f, max, frame));
    }

    // A gap (loading screen, a menu) is not a calm picture: it neither brings reuse back nor looks stale.
    {
        MotionGuard paused;
        CHECK(paused.Update(0.09f, max, frame));
        CHECK(paused.Update(0.01f, max, 8.0));  // one calm reading after eight seconds away
        CHECK(paused.Update(-1.0f, max, 8.0));  // and no reading after another eight
        for (int i = 0; i < 8; ++i)
            CHECK(paused.Update(0.01f, max, frame)); // the wait starts from the frames that really are calm
        CHECK(!paused.Update(0.01f, max, frame));
    }

    // 0: never paused, whatever the share, and the pause is dropped when it is turned off.
    MotionGuard off;
    CHECK(off.Update(0.9f, max, frame));
    CHECK(!off.Update(0.9f, 0.0f, frame));
    CHECK(!off.Update(0.9f, 0.0f, frame));

    // The measurement stops arriving (a readback that never completes): the pause is not kept forever.
    MotionGuard stale;
    CHECK(stale.Update(0.5f, max, frame));
    CHECK(stale.Update(-1.0f, max, 0.5));
    CHECK(!stale.Update(-1.0f, max, 0.6));
}

// Reuse stops below the minimum frame rate and comes back 15% above it; pauses do not count; 0 = no minimum.
static void FrameRate()
{
    const auto run = [](FrameRateGate& g, double fps, double minimum, int frames)
    {
        bool allowed = true;
        for (int i = 0; i < frames; ++i)
            allowed = g.Update(1.0 / fps, minimum);
        return allowed;
    };

    FrameRateGate g;
    CHECK(g.Update(0.0, 25.0));          // no reading yet
    CHECK(run(g, 60.0, 25.0, 60));       // well above
    CHECK(std::abs(g.Fps() - 60.0) < 0.5);
    CHECK(!run(g, 20.0, 25.0, 40));      // below: off
    CHECK(!run(g, 27.0, 25.0, 60));      // above the minimum but within 15%: stays off
    CHECK(run(g, 30.0, 25.0, 60));       // 15% above: back on
    CHECK(run(g, 26.0, 25.0, 60));       // above the minimum: stays on
    CHECK(g.Update(5.0, 25.0));          // a pause (loading) is not a frame rate
    CHECK(std::abs(g.Fps() - 26.0) < 0.5);
    CHECK(run(g, 10.0, 0.0, 30));        // no minimum

    // One slow frame in a fast stream does not stop it.
    FrameRateGate h;
    run(h, 60.0, 25.0, 60);
    CHECK(h.Update(1.0 / 15.0, 25.0));
}

// Where NR runs decides whether reuse is offered at all. A finished picture needs vectors measured on it (Optical
// F5Low's native input); the game's own vectors leave it blocked, as before.
static void Route()
{
    CHECK(UnavailableOnRoute(false, false, false) == nullptr);
    CHECK(UnavailableOnRoute(false, false, true) == nullptr);
    CHECK(UnavailableOnRoute(true, false, false) != nullptr);
    CHECK(UnavailableOnRoute(true, false, true) != nullptr); // before SR stays out, whatever the vectors
    CHECK(UnavailableOnRoute(false, true, false) != nullptr);
    CHECK(UnavailableOnRoute(false, true, true) == nullptr);
}

int main()
{
    Route();
    Alternates();
    NoDoubleReuse();
    FailedCaptureDropsHistory();
    Invalidations();
    ComposeAfterReuseEvenOnFallback();
    DropStartsClean();
    ReuseFailedCounts();
    Disabled();
    StepTolerance();
    HeldWhileMoving();
    HeldCountsOnlyLostReuse();
    Motion();
    FrameRate();

    if (fails == 0)
        printf("nr_detail_reuse_smoke: all passed\n");
    return fails == 0 ? 0 : 1;
}
