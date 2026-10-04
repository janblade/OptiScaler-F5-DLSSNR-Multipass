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

static Candidate Make(uint64_t id, uint32_t w, uint32_t h, uint64_t peak, uint32_t clears = 1)
{
    Candidate c;
    c.id = id;
    c.width = w;
    c.height = h;
    c.format = 45; // D32_FLOAT, only reported
    c.draws = peak;
    c.peakDraws = peak;
    c.clears = clears;
    return c;
}

int main()
{
    const uint32_t W = 2560, H = 1440;

    // The scene's depth wins over a shadow map drawn to far more often: the shadow map is square.
    {
        Selector s;
        const auto pick = s.Update({ Make(1, 2560, 1440, 900), Make(2, 4096, 4096, 3000) }, W, H);
        CHECK(pick.valid && pick.id == 1);
        CHECK(pick.width == 2560 && pick.height == 1440 && pick.score == 900);
    }

    // Cleared and never drawn to (or hardly): not a candidate.
    {
        Selector s;
        CHECK(!s.Update({ Make(1, 2560, 1440, 0, 3) }, W, H).valid);
        CHECK(!s.Update({ Make(1, 2560, 1440, 7) }, W, H).valid);
        CHECK(s.Update({ Make(1, 2560, 1440, 8) }, W, H).valid);
    }

    // Depth at the game's render resolution, below the picture, still qualifies; far below does not.
    {
        Selector s;
        CHECK(s.Update({ Make(1, 1706, 960, 500) }, W, H).id == 1);  // about two thirds
        CHECK(!s.Update({ Make(1, 512, 288, 500) }, W, H).valid);    // a fifth: under the 25% floor
        CHECK(!s.Update({ Make(1, 5760, 3240, 500) }, W, H).valid);  // 2.25x: over the ceiling
        CHECK(s.Update({ Make(1, 5120, 2880, 500) }, W, H).valid);   // 2x supersampled
    }

    // The aspect check: a 4:3 buffer is not the 16:9 picture's depth.
    {
        Selector s;
        CHECK(!s.Update({ Make(1, 1920, 1440, 800) }, W, H).valid);
        // The width of a 16:10 picture against the same 16:9: 11% apart, rejected; 3% apart, accepted.
        CHECK(!s.Update({ Make(1, 2304, 1440, 800) }, W, H).valid);
        CHECK(s.Update({ Make(1, 2560, 1400, 800) }, W, H).valid);
    }

    // Most draws wins; on a tie the buffer nearest the picture's size does.
    {
        Selector s;
        CHECK(s.Update({ Make(1, 1280, 720, 400), Make(2, 2560, 1440, 700) }, W, H).id == 2);
        s.Reset();
        CHECK(s.Update({ Make(1, 1280, 720, 700), Make(2, 2560, 1440, 700) }, W, H).id == 2);
        s.Reset();
        CHECK(s.Update({ Make(1, 2560, 1440, 700), Make(2, 1280, 720, 700) }, W, H).id == 1);
    }

    // Nothing eligible, nothing picked; an empty frame too.
    {
        Selector s;
        CHECK(!s.Update({}, W, H).valid);
        CHECK(!s.Update({ Make(1, 100, 100, 1000) }, W, H).valid);
        CHECK(!s.Update({ Make(1, 2560, 1440, 100) }, 0, 0).valid);
    }

    // The last pick is kept unless another beats it clearly: a pre-pass and the main pass do not trade places.
    {
        Selector s;
        CHECK(s.Update({ Make(1, 2560, 1440, 1000), Make(2, 2560, 1440, 900) }, W, H).id == 1);
        // 2 now leads by 10%: 1 stays (900/1000 is under the 4/3 margin).
        CHECK(s.Update({ Make(1, 2560, 1440, 900), Make(2, 2560, 1440, 1000) }, W, H).id == 1);
        // 2 leads by 2x: it takes over.
        CHECK(s.Update({ Make(1, 2560, 1440, 500), Make(2, 2560, 1440, 1000) }, W, H).id == 2);
        // The kept pick disappearing (no longer eligible, or gone) hands over to the best.
        CHECK(s.Update({ Make(2, 2560, 1440, 1000), Make(3, 2560, 1440, 20) }, W, H).id == 2);
        CHECK(s.Update({ Make(3, 2560, 1440, 20) }, W, H).id == 3);
        CHECK(!s.Update({}, W, H).valid);
        CHECK(!s.Current().valid);
    }

    // A window resize: the same buffer no longer matches the new picture's shape.
    {
        Selector s;
        CHECK(s.Update({ Make(1, 2560, 1440, 900) }, W, H).valid);
        CHECK(!s.Update({ Make(1, 2560, 1440, 900) }, 1920, 1440).valid);
    }

    printf(fails == 0 ? "generic depth: ok\n" : "generic depth: %d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
