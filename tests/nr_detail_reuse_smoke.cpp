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
        CHECK(c.Fallback() == 1);
    }
    {
        Cadence c;
        afterOneFull(c);
        const Decision d = c.Next(Facts(101, 2)); // settings changed
        CHECK(d.kind == Kind::Full && !d.historyUsable);
        CHECK(c.Fallback() == 1);
    }
    {
        Cadence c;
        afterOneFull(c);
        const Decision d = c.Next(Facts(103)); // NR did not run on 101 and 102
        CHECK(d.kind == Kind::Full && !d.historyUsable);
        CHECK(c.Fallback() == 1);
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
}

int main()
{
    Alternates();
    NoDoubleReuse();
    FailedCaptureDropsHistory();
    Invalidations();
    ComposeAfterReuseEvenOnFallback();
    DropStartsClean();
    ReuseFailedCounts();
    Disabled();
    StepTolerance();

    if (fails == 0)
        printf("nr_detail_reuse_smoke: all passed\n");
    return fails == 0 ? 0 : 1;
}
