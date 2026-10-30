// Host check of GameFsrFgRule.h: XeFG on a swapchain the game makes through its own FSR FG library.
// No GPU and no game needed.
// cl /std:c++20 /EHsc tests/game_fsrfg_rule_smoke.cpp
#include "../OptiScaler/framegen/GameFsrFgRule.h"

#include <cstdio>

using namespace GameFsrFg;

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

int main()
{
    // Not under the game's FSR FG library, or another output: nothing to decide
    CHECK(Decide(true, false, true, true, true) == Swapchain::NotUnder);
    CHECK(Decide(false, true, true, true, false) == Swapchain::NotUnder);

    // DLSSG input, the game's FFX FG calls seen, its FSR FG off: XeFG takes it (the tester's case)
    CHECK(Decide(true, true, true, true, false) == Swapchain::Take);

    // The game's FSR FG on (also one left on from before the swapchain): refused
    CHECK(Decide(true, true, true, true, true) == Swapchain::Refuse);

    // FFX hooks off or the calls go elsewhere: its FSR FG turning on would not be seen, so refused
    CHECK(Decide(true, true, true, false, false) == Swapchain::Refuse);

    // Other FG inputs can't tell DLSS-G from FSR FG: refused as before (Modern Warfare 4)
    CHECK(Decide(true, true, false, true, false) == Swapchain::Refuse);
    CHECK(Decide(true, true, false, false, false) == Swapchain::Refuse);

    // Taken: XeFG stands aside only while the game's FSR FG is on
    CHECK(StandsAside(true, true));
    CHECK(!StandsAside(true, false));
    CHECK(!StandsAside(false, true));

    if (fails == 0)
        printf("game_fsrfg_rule_smoke: PASS\n");

    return fails == 0 ? 0 : 1;
}
