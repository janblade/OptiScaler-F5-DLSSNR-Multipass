// Host smoke test for XeFG's Dynamic MFG controllers (XeFG_DynamicCount, XeFG_DynamicCap) on simulated frame timings.
// Build: cl /std:c++20 /EHsc /W4 tests\xefg_dynamic_count_smoke.cpp
#include "../OptiScaler/framegen/xefg/XeFG_DynamicCount.h"
#include "../OptiScaler/framegen/xefg/XeFG_DynamicCap.h"
#include "../OptiScaler/framegen/xefg/XeFG_DynamicTarget.h"

#include <cstdio>

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
};

struct Frame
{
    double frameMs = 0.0;
    double waitMs = 0.0;
};

static Frame Run(const Game& g, int count)
{
    const double work = g.workMs + (count > 0 ? g.onMs + (count - 1) * g.genMs : 0.0);
    double frame = work;

    if (g.limitFps > 0.0)
        frame = std::max(work, (count + 1) * 1000.0 / g.limitFps);

    return { frame, frame - work };
}

struct Result
{
    int count = 0;
    int changesLate = 0; // count changes in the second half
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
            ++r.changesLate;

        count = next;
    }

    r.count = count;
    r.presented = (count + 1) * 1000.0 / Run(g, count).frameMs;
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
            ++r.changesLate;

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

        xefg::ShownPerFrame shown;
        Check(shown.Value() == 0.0, "no present seen: not known");
        shown.Update(0, 0);
        Check(shown.Value() == 0.0, "no change: not known");
        for (uint64_t frame = 1; frame <= 50; ++frame)
            shown.Update(frame * 6, frame);
        Check(std::abs(shown.Value() - 6.0) < 1e-6, "6 shown per present");
        for (uint64_t frame = 51; frame <= 250; ++frame)
            shown.Update(300 + (frame - 50) * 1, frame);
        Check(std::abs(shown.Value() - 1.0) < 0.01, "follows the shown frames down to 1");
        shown.Restart();
        Check(shown.Value() == 0.0, "restarted");
        shown.Update(600, 251);
        Check(shown.Value() == 100.0, "counts presents since the restart");
    }

    std::printf(failures == 0 ? "xefg_dynamic_count_smoke: PASS\n" : "xefg_dynamic_count_smoke: FAIL (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
