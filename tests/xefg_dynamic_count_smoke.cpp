// Host smoke test for XeFG's Dynamic MFG controllers (XeFG_DynamicCount, XeFG_DynamicCap) on simulated frame timings.
// Build: cl /std:c++20 /EHsc /W4 tests\xefg_dynamic_count_smoke.cpp
#include "../OptiScaler/framegen/xefg/XeFG_DynamicCount.h"
#include "../OptiScaler/framegen/xefg/XeFG_DynamicCap.h"
#include "../OptiScaler/framegen/xefg/XeFG_DynamicTarget.h"
#include "../OptiScaler/framegen/xefg/XeFG_ShownRate.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <vector>

static int failures = 0;

static void Check(bool ok, const char* what)
{
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

// A game: its own work per real frame, each generated frame's GPU cost (the first one also pays for frame generation
// being on), and a limiter that holds the shown rate (0: none)
struct Game
{
    double workMs = 20.0;
    double genMs = 0.5;
    double onMs = 1.0;
    double limitFps = 0.0;
    double dropsOverFps = 0.0; // V-Sync that drops generated frames over this rate instead of holding the game
    double floorMs = 0.0; // a GPU bound game: no real frame is shorter, the CPU waits out the rest (seen as waits)
};

struct Frame
{
    double frameMs = 0.0;
    double waitMs = 0.0;
};

// A try of one step less and the step back after it: the controllers do this now and then at a cap the waits don't
// show, it is not flipping. Bounded below by the probes' own back-off.
template <typename R> static bool IsProbe(R reason) { return reason == R::Probe || reason == R::Undo; }

static Frame Run(const Game& g, int count)
{
    const double work = g.workMs + (count > 0 ? g.onMs + (count - 1) * g.genMs : 0.0);
    double frame = std::max(work, g.floorMs);

    if (g.limitFps > 0.0)
        frame = std::max(frame, (count + 1) * 1000.0 / g.limitFps);

    return { frame, frame - work };
}

struct Result
{
    int count = 0;
    int changesLate = 0; // count changes in the second half, a probe (one step less now and then) and its undo apart
    int probeChanges = 0; // the probes of the second half and their undo
    int undos = 0;        // steps down of the second half that did not hold and were taken back
    double presented = 0.0;
    double lateMs[9] = {}; // time at each count in the second half
};

// `vsync`: the game's limit is the display's refresh rate (V-Sync) rather than OptiScaler's FPS limit. `jitter`: each
// frame's own work varies by up to this share, from a fixed pseudo-random sequence.
static Result SimCount(const Game& g, double targetFps, int maxCount, int startCount, int rounding, int minCount,
                       bool vsync = false, double jitter = 0.0, double seconds = 120.0)
{
    XeFG_DynamicCount dc;
    int count = startCount;
    double now = 0.0;
    unsigned int seed = 12345u;
    Result r;

    while (now < seconds * 1000.0)
    {
        Game frameGame = g;

        if (jitter > 0.0)
        {
            seed = seed * 1103515245u + 12345u;
            frameGame.workMs *= 1.0 + jitter * (((seed >> 16) & 0x7fff) / 16383.5 - 1.0);
        }

        const auto f = Run(frameGame, count);
        now += f.frameMs;

        const int next = dc.Update({ now, f.frameMs, f.waitMs, targetFps, vsync ? g.limitFps : g.dropsOverFps,
                                     vsync ? 0.0 : g.limitFps, maxCount, count, rounding, minCount });

        if (now >= seconds * 500.0)
            r.lateMs[std::clamp(count, 0, 8)] += f.frameMs;

        if (next != count && now >= seconds * 500.0)
        {
            ++(IsProbe(dc.LastReason()) ? r.probeChanges : r.changesLate);
            r.undos += dc.LastReason() == XeFG_DynamicCount::Reason::Undo;
        }

        count = next;
    }

    r.count = count;
    r.presented = (count + 1) * 1000.0 / Run(g, count).frameMs;
    return r;
}

// The DLL's path with the shown frames measured (XeFG_Dx12::DynamicCount): each call reads the totals of the presents
// made before it, `current` is the count of the last call's answer, and XeFG applies a count one frame later than
// that and then shows exactly count + 1 frames per present.
static Result SimMeasured(const Game& g, double targetFps, int maxCount, int startCount, int rounding, int minCount,
                          int lateFrames = 1, double jitter = 0.0, double seconds = 120.0)
{
    unsigned int seed = 12345u;
    XeFG_DynamicCount dc;
    xefg::ShownPerFrame shownPerFrame;
    std::atomic<uint64_t> totals { 0 };
    int shownCount = -1;
    std::vector<int> answers(lateFrames + 1, startCount); // the last answers, newest first
    double now = 0.0;
    Result r;

    while (now < seconds * 1000.0)
    {
        const int current = answers[0];
        const int applied = answers[lateFrames];
        Game frameGame = g;

        if (jitter > 0.0)
        {
            seed = seed * 1103515245u + 12345u;
            frameGame.workMs *= 1.0 + jitter * (((seed >> 16) & 0x7fff) / 16383.5 - 1.0);
        }

        const auto f = Run(frameGame, applied);
        now += f.frameMs;

        if (current != shownCount)
        {
            shownPerFrame.Restart();
            shownCount = current;
        }

        shownPerFrame.Update(totals.load());

        const int next = dc.Update({ now, f.frameMs, f.waitMs, targetFps, 0.0, 0.0, maxCount, current, rounding, minCount,
                                     shownPerFrame.Value(), shownPerFrame.Sample() });

        if (next != current && now >= seconds * 500.0)
            ++(IsProbe(dc.LastReason()) ? r.probeChanges : r.changesLate);

        xefg::AddShownPresent(totals, (uint32_t) (applied + 1));
        answers.insert(answers.begin(), next);
        answers.pop_back();
    }

    r.count = answers[0];
    return r;
}

static Result SimCap(const Game& g, double targetFps, int maxCount, int startCount, double seconds = 120.0)
{
    XeFG_DynamicCap dc;
    int count = startCount;
    double now = 0.0;
    Result r;

    // The target is the limit: the limiter sleeps the rest of each real frame
    Game capped = g;
    capped.limitFps = targetFps;

    while (now < seconds * 1000.0)
    {
        const auto f = Run(capped, count);
        now += f.frameMs;

        const int next = dc.Update({ now, f.frameMs, f.waitMs, targetFps, maxCount, count });

        if (next != count && now >= seconds * 500.0)
        {
            ++(IsProbe(dc.LastReason()) ? r.probeChanges : r.changesLate);
            r.undos += dc.LastReason() == XeFG_DynamicCap::Reason::Undo;
        }

        count = next;
    }

    r.count = count;
    r.presented = (count + 1) * 1000.0 / Run(capped, count).frameMs;
    return r;
}

int main()
{
    std::printf("Dynamic MFG, no cap:\n");
    {
        // 40 fps real, 116 target, around: 3X lands nearest (about 113), 4X about 147
        Game g { 25.0, 0.5, 1.0, 0.0 };
        const auto from6 = SimCount(g, 116.0, 5, 5, 1, 0);
        const auto from2 = SimCount(g, 116.0, 5, 1, 1, 0);
        std::printf("    from 6X: %dX at %.1f fps, from 2X: %dX at %.1f fps\n", from6.count + 1, from6.presented,
                    from2.count + 1, from2.presented);
        Check(from6.count == 2 && from2.count == 2, "settles at 3X, nearest the target, from above and below");
        Check(from6.changesLate == 0 && from2.changesLate == 0, "holds still once settled");
        Check(from6.probeChanges <= 4 && from2.probeChanges <= 4, "probes of one step less stay rare: 2 tries at most in a minute");
    }
    {
        // "at least" counts the target reached from 97% of it: at 120, 3X (about 113, 94%) isn't, 4X is the lowest
        Game g { 25.0, 0.5, 1.0, 0.0 };
        const auto r = SimCount(g, 120.0, 5, 1, 2, 0);
        std::printf("    at least 120: %dX at %.1f fps\n", r.count + 1, r.presented);
        Check(r.count == 3 && r.presented >= 120.0 * 0.97, "at least the target picks 4X");
    }
    {
        // The game alone is near the target: 1X when allowed, else 2X
        Game g { 9.0, 0.5, 1.5, 0.0 };
        const auto off = SimCount(g, 116.0, 3, 3, 1, 0);
        const auto on = SimCount(g, 116.0, 3, 3, 1, 1);
        std::printf("    near target: allowed %dX at %.1f fps, not allowed %dX\n", off.count + 1, off.presented,
                    on.count + 1);
        Check(off.count == 0, "1X (frame generation off) when the game alone is nearest");
        Check(on.count == 1, "2X when 1X isn't allowed");
    }
    {
        // Out of reach: 15 fps real, generated frames costly (4 ms each); target 240
        Game g { 66.0, 4.0, 4.0, 0.0 };
        const auto r = SimCount(g, 240.0, 5, 1, 1, 0);
        std::printf("    out of reach: %dX at %.1f fps, %d late changes\n", r.count + 1, r.presented, r.changesLate);
        Check(r.count >= 3, "climbs while each frame still earns its rate");
        Check(r.changesLate <= 1, "no flipping while the target can't be reached");
    }

    std::printf("Dynamic MFG under an FPS limit:\n");
    {
        // Limit 120 with target 120: 50 fps real, 3X reaches it with room, 2X doesn't
        Game g { 20.0, 0.5, 1.0, 120.0 };
        const auto r = SimCount(g, 120.0, 5, 5, 1, 0);
        std::printf("    limit 120: %dX at %.1f fps, %d late changes\n", r.count + 1, r.presented, r.changesLate);
        Check(r.count == 2 && r.presented >= 120.0 * 0.97, "steps down to 3X, the lowest that holds the limit");
        Check(r.changesLate == 0, "settles under the limit");
    }
    {
        // The same rate held by V-Sync at 120 Hz instead of a limit
        Game g { 20.0, 0.5, 1.0, 120.0 };
        const auto r = SimCount(g, 116.0, 5, 5, 1, 0, true);
        std::printf("    V-Sync 120 Hz, target 116: %dX at %.1f fps, %d late changes\n", r.count + 1, r.presented,
                    r.changesLate);
        Check(r.count >= 1 && r.count <= 2 && r.presented >= 116.0 * 0.83, "steps down under V-Sync");
        Check(r.changesLate <= 1, "settles under V-Sync");
    }

    std::printf("Dynamic MFG with noisy frame times:\n");
    {
        // As the first case, each frame's work up to 10% off
        Game g { 25.0, 0.5, 1.0, 0.0 };
        const auto r = SimCount(g, 116.0, 5, 5, 1, 0, false, 0.10);
        const double share3x = r.lateMs[2] / 60000.0;
        std::printf("    +-10%%: %dX at %.1f fps, %d late changes, %.1f%% of the second half at 3X\n", r.count + 1,
                    r.presented, r.changesLate, share3x * 100.0);
        Check(r.count == 2, "still settles at 3X");
        // A try of one step less and its undo are by design (spaced by a doubling wait); noise must not keep it off 3X
        Check(r.changesLate <= 2 && share3x >= 0.95, "noise doesn't make it flip: 95% or more of the time at 3X");
    }

    std::printf("A step down that does not hold is not tried again every minute (GPU bound game, its waits look like idle time):\n");
    {
        // 5 ms of CPU work then 18 ms until the GPU is done; the limiter holds 140 fps. 3X reaches 140, 2X only 111.
        Game g;
        g.workMs = 5.0;
        g.floorMs = 18.0;
        g.limitFps = 140.0;
        const auto r = SimCount(g, 140.0, 5, 5, 1, 1, false, 0.0, 1200.0);
        std::printf("    %dX at %.1f fps, %d undone steps down in the second 10 minutes\n", r.count + 1, r.presented, r.undos);
        Check(r.presented >= 138.0, "stays at the rate it holds");
        Check(r.undos <= 3, "a step down that failed is tried again rarely: 3 times at most in 10 minutes");
        const auto c = SimCap(g, 140.0, 5, 5, 1200.0);
        std::printf("    cap controller: %dX at %.1f fps, %d undone steps down\n", c.count + 1, c.presented, c.undos);
        Check(c.undos <= 3, "the cap controller too");
    }
    std::printf("Use target as cap:\n");
    {
        // 50 fps real, cap 120: 3X is the lowest that reaches it
        Game g { 20.0, 0.5, 1.0, 0.0 };
        const auto from6 = SimCap(g, 120.0, 5, 5);
        const auto from2 = SimCap(g, 120.0, 5, 1);
        std::printf("    from 6X: %dX at %.1f fps, from 2X: %dX at %.1f fps\n", from6.count + 1, from6.presented,
                    from2.count + 1, from2.presented);
        Check(from6.count == 2 && from2.count == 2, "the lowest count that holds the cap");
        Check(from6.changesLate == 0 && from2.changesLate == 0, "holds still at the cap");
        Check(from6.presented >= 120.0 * 0.97, "presented at the cap");
    }

    std::printf("Target under V-Sync:\n");
    {
        Check(xefg::DynamicTargetFps(180.0f, 0.0f, 120.0f, true) == 120.0f,
              "a target above the refresh rate: the refresh");
        Check(xefg::DynamicTargetFps(180.0f, 0.0f, 120.0f, false) == 180.0f, "without V-Sync: as set");
        Check(xefg::DynamicTargetFps(90.0f, 0.0f, 120.0f, true) == 90.0f, "under the refresh rate: as set");
        Check(xefg::DynamicTargetFps(0.0f, 180.0f, 120.0f, true) == 120.0f, "the FPS limit above it: the refresh");
        Check(xefg::DynamicTargetFps(180.0f, 60.0f, 120.0f, true) == 60.0f, "the FPS limit below it: the limit");
        Check(xefg::DynamicTargetFps(0.0f, 0.0f, 120.0f, true) == 116.0f, "auto: just under the refresh");
        Check(xefg::DynamicTargetFps(180.0f, 0.0f, 0.0f, true) == 180.0f, "refresh unknown: as set");

        // Witcher 3's case (6X run, ForceVsync, 120 Hz, "at least", target 180): XeFG drops the generated frames that
        // don't fit the refresh rather than holding the game, so (count + 1) x the real rate reads above 120 and the
        // controller never sees the cap; with the target as set it climbs to the most for frames that are dropped
        Game g { 30.0, 0.5, 1.0, 0.0, 120.0 };
        const auto unclamped = SimCount(g, 180.0, 5, 3, 2, 0, false, 0.10, 600.0);
        const auto clamped =
            SimCount(g, xefg::DynamicTargetFps(180.0f, 0.0f, 120.0f, true), 5, 3, 2, 0, false, 0.10, 600.0);
        std::printf("    180 as set: %dX; clamped to 120: %dX at %.1f fps estimated, %d late changes\n",
                    unclamped.count + 1, clamped.count + 1, clamped.presented, clamped.changesLate);
        Check(unclamped.count == 5, "(as set it climbs to 6X, for frames V-Sync drops)");
        // 3X: 3 x 31.7 = 95, 4X: 4 x 31.3 = 125
        Check(clamped.count == 3 && clamped.presented >= 120.0 * 0.97,
              "clamped: 4X, the lowest that reaches the refresh rate");
        Check(clamped.changesLate <= 2, "clamped: settles");
    }

    std::printf("Measured shown frames:\n");
    {
        // A tester's Forza Motorsport log: 6X asked for, 1.4 frames shown per real frame. The estimate counts the shown
        // frames, not count x real.
        XeFG_DynamicCount dc;
        dc.Update({ 1000.0, 10.0, 0.0, 168.0, 0.0, 0.0, 5, 5, 1, 0, 1.4 });
        Check(std::abs(dc.PresentedFps() - 140.0) < 0.5, "Dynamic MFG: presented rate from the shown frames");

        XeFG_DynamicCount nominal;
        nominal.Update({ 1000.0, 10.0, 0.0, 168.0, 0.0, 0.0, 5, 5, 1, 0 });
        Check(std::abs(nominal.PresentedFps() - 600.0) < 0.5, "Dynamic MFG: nothing measured, count x real");

        XeFG_DynamicCount over;
        over.Update({ 1000.0, 10.0, 0.0, 168.0, 0.0, 0.0, 5, 1, 1, 0, 9.0 });
        Check(std::abs(over.PresentedFps() - 200.0) < 0.5, "Dynamic MFG: never above what the count asks for");

        XeFG_DynamicCap cap;
        cap.Update({ 1000.0, 10.0, 0.0, 168.0, 5, 5, 1.4 });
        Check(std::abs(cap.PresentedFps() - 140.0) < 0.5, "cap: presented rate from the shown frames");

        constexpr int kPresentBits = (int) xefg::kShownPresentBits;
        const auto pack = [](uint64_t frames, uint64_t presents) { return (frames << kPresentBits) | presents; };

        xefg::ShownPerFrame shown;
        Check(shown.Value() == 0.0, "no present seen: not known");
        shown.Update(pack(0, 0));
        Check(shown.Value() == 0.0 && shown.Sample() < 0.0, "the first call only takes the baselines");
        shown.Update(pack(0, 0));
        Check(shown.Value() == 0.0, "no change: not known");
        shown.Restart();
        for (uint64_t frame = 1; frame <= 50; ++frame)
            shown.Update(pack(frame * 6, frame));
        Check(std::abs(shown.Value() - 6.0) < 1e-6, "6 shown per present");
        for (uint64_t frame = 51; frame <= 250; ++frame)
            shown.Update(pack(300 + (frame - 50) * 1, frame));
        Check(std::abs(shown.Value() - 1.0) < 0.01, "follows the shown frames down to 1");

        // The first samples after a restart are not used (XeFG applies a count a frame late)
        shown.Restart();
        Check(shown.Value() == 0.0, "restarted");
        shown.Update(pack(500, 251));
        shown.Update(pack(503, 252));
        Check(shown.Value() == 0.0 && shown.Sample() < 0.0, "the first 2 samples after a restart are skipped");
        shown.Update(pack(506, 253));
        Check(shown.Value() == 3.0 && shown.Sample() == 3.0, "the next one is the value");

        // A stall: presents that sent nothing pull the value down (they are counted, not dropped)
        for (uint64_t frame = 254; frame < 274; ++frame)
            shown.Update(pack(506, frame));
        Check(shown.Value() > 0.0 && shown.Value() < 0.5, "a run of 0-frame presents lowers the value");

        // New baselines after Dynamic was off: the totals moved on without us
        shown.Rebase();
        shown.Update(pack(900000, 90000));
        Check(shown.Value() == 0.0 && shown.Sample() < 0.0, "rebased: the jump in the totals is not a sample");
        shown.Update(pack(900000 + 4 * 3, 90003));
        shown.Update(pack(900000 + 4 * 6, 90006));
        shown.Update(pack(900000 + 4 * 7, 90007));
        Check(shown.Value() == 4.0, "rebased: counts from the baselines");

        // Both totals in one word, wrapping: presents past 24 bits, differences still right
        std::atomic<uint64_t> totals { pack(77, xefg::kShownPresentMask) };
        xefg::ShownPerFrame wrap;
        wrap.Update(totals.load());
        wrap.Restart();
        xefg::AddShownPresent(totals, 5);
        xefg::AddShownPresent(totals, 5);
        xefg::AddShownPresent(totals, 5);
        wrap.Update(totals.load());
        Check(wrap.Value() == 0.0, "the wrap is not a sample");
        xefg::AddShownPresent(totals, 7);
        wrap.Update(totals.load());
        Check(wrap.Value() == 0.0 && wrap.Sample() < 0.0, "skipped sample across the wrap");
        xefg::AddShownPresent(totals, 7);
        wrap.Update(totals.load());
        Check(wrap.Value() == 7.0, "7 per present across the wrap of the presents");
        Check((totals.load() >> kPresentBits) == 77 + 15 + 7 + 7 && (totals.load() & xefg::kShownPresentMask) == 4,
              "the frames did not take the presents' carry");
    }

    std::printf("Dynamic MFG with measured shown frames, in the DLL's call order:\n");
    {
        // XeFG shows exactly count + 1 per present, 0 to 2 frames after the count was set. An up step must not be judged
        // by the presents made at the old count (it read as no gain and was undone, flipping 5X/6X). Without noise
        // the measured path must end exactly where the nominal one does; with noise a case near a band edge may tip
        // either way (4 of 4050), where the stale first sample flipped 31.
        int cases = 0;
        int different = 0;
        int differentSteady = 0;
        const double works[] = { 12.0, 15.0, 20.0, 25.0, 30.0 };
        const double gens[] = { 0.3, 0.5, 1.0 };
        const double targets[] = { 72.0, 90.0, 116.0, 144.0, 240.0 };

        for (const double work : works)
            for (const double gen : gens)
                for (const double target : targets)
                    for (int rounding = 0; rounding <= 2; ++rounding)
                        for (const int start : { 1, 3, 5 })
                            for (const int late : { 0, 1, 2 })
                                for (const double jitter : { 0.0, 0.1 })
                            {
                                const Game g { work, gen, 1.0, 0.0 };
                                const auto nominal = SimCount(g, target, 5, start, rounding, 0, false, jitter);
                                const auto measured = SimMeasured(g, target, 5, start, rounding, 0, late, jitter);
                                ++cases;
                                const bool differs =
                                    nominal.count != measured.count || nominal.changesLate != measured.changesLate;
                                different += differs;
                                differentSteady += differs && jitter == 0.0;
                            }

        std::printf("    %d cases, %d where measured differs from nominal (%d without noise)\n", cases, different,
                    differentSteady);
        Check(differentSteady == 0, "measured shown frames reach the same count as the nominal, with the same changes");
        Check(different <= 8, "with noise: no more than a few cases tip the other way");

        // 40 fps real, 116 target: 3X is nearest; from 2X the up step to 3X has to stand
        const Game g { 25.0, 0.5, 1.0, 0.0 };
        const auto up = SimMeasured(g, 116.0, 5, 1, 1, 0);
        Check(up.count == 2 && up.changesLate == 0, "an up step on measured values stands");
    }

    std::printf(failures == 0 ? "xefg_dynamic_count_smoke: PASS\n" : "xefg_dynamic_count_smoke: FAIL (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
