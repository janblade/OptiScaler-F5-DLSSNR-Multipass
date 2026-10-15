// Host check of FGFollowGameRule.h: OptiScaler's FG following the game's DLSS Frame Generation setting
// (FG Input DLSSG, Enabled auto). No GPU and no game needed.
// cl /std:c++20 /EHsc tests/fg_follow_game_smoke.cpp
#include "../OptiScaler/framegen/FGFollowGameRule.h"

#include <cstdio>

using namespace FGFollowGame;

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
    // The game's request: off/on and a sane count from whatever the struct carried
    CHECK((FromGame(true, 3) == Request { false, 3 }));
    CHECK((FromGame(false, 0) == Request { true, 1 }));
    CHECK((FromGame(false, 3) == Request { true, 3 }));
    CHECK((FromGame(false, 0xFFFFFFFFu) == Request { true, kMaxGameCount }));

    // Who decides: DLSSG input on Streamline 2+, and only while Enabled is auto
    CHECK(CanFollow(true, 2));
    CHECK(!CanFollow(true, 1));
    CHECK(!CanFollow(false, 2));
    CHECK(Follows(false, true));
    CHECK(!Follows(true, true));
    CHECK(!Follows(false, false));

    const Request gameOn3 { true, 3 };
    const Request gameOff { false, 3 };

    // Following: the game's on/off wins over the menu value; not following: the menu value wins
    CHECK(Enabled(true, gameOn3, false));
    CHECK(!Enabled(true, gameOff, true));
    CHECK(Enabled(false, gameOff, true));
    CHECK(!Enabled(false, gameOn3, false));

    // Following never starts XeFG where its own checks refuse: exclusive full screen, or a float (scRGB) swapchain.
    // Ignore Init Checks lifts that, as in the menu; other outputs are not concerned.
    CHECK(!FollowBlocked(true, false, false, false));
    CHECK(FollowBlocked(true, false, true, false));
    CHECK(FollowBlocked(true, false, false, true));
    CHECK(!FollowBlocked(true, true, true, true));
    CHECK(!FollowBlocked(false, false, true, true));

    // Count: the game's when following, else the menu's; clamped to the output's max, unknown max -> 1
    CHECK(Count(true, gameOn3, 1, 3) == 3);
    CHECK(Count(true, gameOn3, 1, 2) == 2);
    CHECK(Count(true, gameOn3, 1, 0) == 1);
    CHECK(Count(false, gameOn3, 2, 3) == 2);
    CHECK(Count(false, gameOn3, 5, 3) == 3);
    CHECK(Count(false, gameOn3, 0, 3) == 1);

    // Max told to the game: the live output's, never below 1
    CHECK(ReportedMax(0) == 1);
    CHECK(ReportedMax(1) == 1);
    CHECK(ReportedMax(3) == 3);

    CHECK(Presented(false, 2) == 1);
    CHECK(Presented(true, 2) == 3);
    CHECK(Presented(true, -1) == 1);

    // Re-evaluation: turning on while following; a count change only for an output that needs a recreate
    CHECK(NeedsFGChange(true, gameOff, gameOn3, false));
    CHECK(!NeedsFGChange(false, gameOff, gameOn3, false));
    CHECK(!NeedsFGChange(true, gameOn3, gameOff, true));
    CHECK(!NeedsFGChange(true, gameOn3, Request { true, 2 }, false));
    CHECK(NeedsFGChange(true, gameOn3, Request { true, 2 }, true));
    CHECK(!NeedsFGChange(true, gameOn3, gameOn3, true));

    printf(fails == 0 ? "fg_follow_game_smoke: OK\n" : "fg_follow_game_smoke: %d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
