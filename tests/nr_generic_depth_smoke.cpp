// Host check of GenericDepth_Select.h: the scene-depth heuristic of the native input producer. No GPU and no game needed.
// cl /std:c++20 /EHsc /W4 tests/nr_generic_depth_smoke.cpp
#include "../OptiScaler/resource_tracking/GenericDepth_Select.h"

#include <cstdio>
#include <vector>

using namespace GenericDepthSelect;

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

static Candidate Make(uint64_t id, uint32_t w, uint32_t h, uint64_t vertices, uint32_t drawcalls = 100,
                      uint32_t indirect = 0)
{
    Candidate c;
    c.id = id;
    c.width = w;
    c.height = h;
    c.format = 45; // D32_FLOAT, only reported
    c.vertices = vertices;
    c.drawcalls = drawcalls;
    c.drawcallsIndirect = indirect;
    c.clears = 1;
    return c;
}

// A buffer has to have been in use for two frames before the one it can win in: feed the same frame three times and take
// the last answer.
static Pick Settle(Selector& s, const std::vector<Candidate>& frame, uint32_t w, uint32_t h)
{
    s.Update(frame, w, h);
    s.Update(frame, w, h);
    return s.Update(frame, w, h);
}

int main()
{
    const uint32_t W = 2560, H = 1440;

    // The scene's depth wins over a shadow map drawn to far more: the shadow map is square.
    {
        Selector s;
        const auto pick = Settle(s, { Make(1, 2560, 1440, 90000), Make(2, 4096, 4096, 300000) }, W, H);
        CHECK(pick.valid && pick.id == 1);
        CHECK(pick.width == 2560 && pick.height == 1440 && pick.score == 90000);
    }

    // A buffer has to have been in use for two frames first: on the frame it shows up and the next, it cannot win.
    {
        Selector s;
        CHECK(!s.Update({ Make(1, 2560, 1440, 90000) }, W, H).valid);
        CHECK(!s.Update({ Make(1, 2560, 1440, 90000) }, W, H).valid);
        CHECK(s.Update({ Make(1, 2560, 1440, 90000) }, W, H).valid);
    }

    // Unused, or only a fullscreen pass: not a candidate. More than three vertices, or any indirect draw, is.
    {
        Selector s;
        CHECK(!Settle(s, { Make(1, 2560, 1440, 0, 0) }, W, H).valid);
        s.Reset();
        CHECK(!Settle(s, { Make(1, 2560, 1440, 3, 5), Make(2, 2560, 1440, 3, 40) }, W, H).valid);
        s.Reset();
        CHECK(Settle(s, { Make(1, 2560, 1440, 4, 40) }, W, H).valid);
        s.Reset();
        CHECK(Settle(s, { Make(1, 2560, 1440, 0, 40, 40) }, W, H).valid); // GPU-driven: no vertex count, indirect draws
    }

    // A frame whose only buffer drew eight draws or fewer is not a frame: the pick stays and the count does not advance.
    {
        Selector s;
        const auto first = Settle(s, { Make(1, 2560, 1440, 90000) }, W, H);
        CHECK(first.valid && first.id == 1);
        CHECK(s.Update({ Make(2, 2560, 1440, 9000, 8) }, W, H).id == 1);
        CHECK(s.Update({ Make(1, 2560, 1440, 90000), Make(2, 2560, 1440, 100000, 8) }, W, H).id == 1); // two buffers: a frame
    }

    // Ranked by vertices; by draw calls when over a third of them are indirect.
    {
        Selector s;
        CHECK(Settle(s, { Make(1, 2560, 1440, 50000, 100), Make(2, 2560, 1440, 80000, 100) }, W, H).id == 2);
        s.Reset();
        // Indirect-heavy buffers carry no trustworthy vertex count: 1 has fewer vertices but more draw calls.
        CHECK(Settle(s, { Make(1, 2560, 1440, 100, 900, 800), Make(2, 2560, 1440, 5000, 120, 90) }, W, H).id == 1);
        CHECK(Score(Make(1, 1, 1, 777, 90, 29)) == 777);   // 29 < 90/3 = 30: vertices
        CHECK(Score(Make(1, 1, 1, 777, 90, 30)) == 90);    // 30 is not under 30: draw calls
    }

    // NBA 2K27: a GPU-driven scene (most draws indirect, few vertices counted) against a 6-draw overlay of the same
    // size. Each is compared on the same count, not the scene's draw calls against the overlay's vertices.
    {
        const Candidate scene = Make(1, 2560, 1440, 29442, 225, 162);
        const Candidate overlay = Make(2, 2560, 1440, 1686, 6, 0);
        Selector s;
        CHECK(Settle(s, { scene, overlay }, W, H).id == 1);
        s.Reset();
        CHECK(Settle(s, { overlay, scene }, W, H).id == 1);
        s.Reset();
        CHECK(Settle(s, { Make(1, 2560, 1440, 54865608, 4463, 1975), Make(2, 2560, 1440, 5418, 7, 0) }, W, H).id == 1);

        // An overlay that held the pick while the scene was away loses it once the scene is back.
        const Candidate shadow = Make(3, 4096, 4096, 300000);
        s.Reset();
        CHECK(Settle(s, { overlay, shadow }, W, H).id == 2);
        CHECK(Settle(s, { overlay, scene, shadow }, W, H).id == 1);
    }

    // Depth at the render resolution, below the picture, still qualifies. The range is the picture's size divided by the
    // buffer's from 0.5 to 4 (ReShade stops at 1.85): a buffer from a quarter of the picture up to 2x of it.
    {
        Selector s;
        CHECK(Settle(s, { Make(1, 1706, 960, 50000) }, W, H).id == 1);   // two thirds
        s.Reset();
        CHECK(Settle(s, { Make(1, 1400, 788, 50000) }, W, H).valid);     // 1.83: inside
        s.Reset();
        CHECK(Settle(s, { Make(1, 1350, 759, 50000) }, W, H).valid);     // 1.90: inside since the range was widened
        s.Reset();
        CHECK(Settle(s, { Make(1, 1280, 720, 50000) }, W, H).valid);     // half the size (Witcher 3): inside
        s.Reset();
        CHECK(Settle(s, { Make(1, 700, 394, 50000) }, W, H).valid);      // 3.66: inside
        s.Reset();
        CHECK(!Settle(s, { Make(1, 600, 338, 50000) }, W, H).valid);     // 4.27: outside
        s.Reset();
        CHECK(Settle(s, { Make(1, 5120, 2880, 50000) }, W, H).valid);    // 2x supersampled: the edge, inside
        s.Reset();
        CHECK(!Settle(s, { Make(1, 5200, 2925, 50000) }, W, H).valid);   // just over 2x: outside
    }

    // The aspect check: shapes within 0.1 of each other's width/height.
    {
        Selector s;
        CHECK(!Settle(s, { Make(1, 1920, 1440, 50000) }, W, H).valid);   // 4:3 against 16:9: 0.44 apart
        s.Reset();
        CHECK(!Settle(s, { Make(1, 2304, 1440, 50000) }, W, H).valid);   // 16:10: 0.18 apart
        s.Reset();
        CHECK(Settle(s, { Make(1, 2560, 1400, 50000) }, W, H).valid);    // 0.05 apart
    }

    // On a tie the buffer nearest the picture's size wins.
    {
        Selector s;
        CHECK(Settle(s, { Make(1, 1706, 960, 70000), Make(2, 2560, 1440, 70000) }, W, H).id == 2);
        s.Reset();
        CHECK(Settle(s, { Make(1, 2560, 1440, 70000), Make(2, 1706, 960, 70000) }, W, H).id == 1);
    }

    // Nothing eligible, nothing picked; an empty frame and a zero-sized picture too.
    {
        Selector s;
        CHECK(!Settle(s, {}, W, H).valid);
        CHECK(!Settle(s, { Make(1, 100, 100, 90000) }, W, H).valid);
        CHECK(!Settle(s, { Make(1, 2560, 1440, 90000) }, 0, 0).valid);
    }

    // The last pick is kept unless another beats it clearly: a pre-pass and the main pass do not trade places.
    {
        Selector s;
        s.Settings().holdFrames = 0;
        CHECK(Settle(s, { Make(1, 2560, 1440, 100000), Make(2, 2560, 1440, 90000) }, W, H).id == 1);
        // 2 now leads by 10%: 1 stays (90000/100000 is over the 0.75 line).
        CHECK(s.Update({ Make(1, 2560, 1440, 90000), Make(2, 2560, 1440, 100000) }, W, H).id == 1);
        // 2 leads by 2x: it takes over.
        CHECK(s.Update({ Make(1, 2560, 1440, 50000), Make(2, 2560, 1440, 100000) }, W, H).id == 2);
        // The kept pick disappearing hands over to the best one that has been around long enough.
        CHECK(s.Update({ Make(2, 2560, 1440, 100000), Make(3, 2560, 1440, 2000) }, W, H).id == 2);
        CHECK(!s.Update({}, W, H).valid);
    }

    // The snapshot to take and the depth direction travel with the pick.
    {
        Selector s;
        Candidate c = Make(1, 2560, 1440, 90000);
        c.bestClear = 2;
        c.reversed = true;
        const auto pick = Settle(s, { c }, W, H);
        CHECK(pick.valid && pick.bestClear == 2 && pick.reversed);
    }

    // A window resize: the same buffer no longer matches the new picture's shape.
    {
        Selector s;
        CHECK(Settle(s, { Make(1, 2560, 1440, 90000) }, W, H).valid);
        CHECK(!Settle(s, { Make(1, 2560, 1440, 90000) }, 1920, 1440).valid);
    }

    // A pick is held while nothing qualifies, for holdFrames frames in a row, then dropped; a buffer that qualifies again
    // clears the count. The Witcher 3 case: the scene's depth at half the picture, a small full-size buffer, a shadow map.
    {
        Selector s;
        s.Settings().holdFrames = 2;
        const std::vector<Candidate> scene = { Make(1, 1280, 720, 5500000, 900), Make(2, 2560, 1440, 20000, 100),
                                               Make(3, 4096, 4096, 5500000, 760) };
        CHECK(Settle(s, scene, W, H).id == 1);
        CHECK(s.Update({}, W, H).id == 1);
        CHECK(s.Update({ Make(9, 100, 100, 5000) }, W, H).id == 1);
        CHECK(!s.Update({}, W, H).valid);
        // Once dropped it needs to qualify again, and warm up again.
        CHECK(Settle(s, scene, W, H).id == 1);
        CHECK(s.Update({}, W, H).id == 1);
        CHECK(Settle(s, scene, W, H).id == 1);   // qualified again: the miss count starts over
        CHECK(s.Update({}, W, H).id == 1);
        CHECK(s.Update({}, W, H).id == 1);
        CHECK(!s.Update({}, W, H).valid);
    }

    printf(fails == 0 ? "generic depth: ok\n" : "generic depth: %d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
