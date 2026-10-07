// Host check of native/VirtualUpscalerHold.h: Optical F5Low's virtual upscaler (and frame generation's context with it)
// is let go only once the game's own upscaler has really taken over, and a flip of the depth direction or HDR must hold
// before the backend is rebuilt. cl /std:c++20 /EHsc /W4 tests/nr_virtual_upscaler_hold_smoke.cpp
#include "../OptiScaler/native/VirtualUpscalerHold.h"

#include <cstdio>

using namespace native::hold;

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

struct Key
{
    int width = 0;
    bool reversed = false;
    bool operator==(const Key&) const = default;
};

int main()
{
    // A menu that calls the game's upscaler for 3 s: paused only, never let go
    {
        Takeover t;
        bool letGo = false;

        for (double ms = 0.0; ms <= 3000.0; ms += 16.0)
            letGo = letGo || t.Update(true, 10000.0 + ms);

        CHECK(!letGo);
        CHECK(!t.Update(false, 13100.0));
    }

    // The game's upscaler for good: let go after kTakeoverMs, not before
    {
        Takeover t;
        CHECK(!t.Update(true, 1000.0));
        CHECK(!t.Update(true, 1000.0 + kTakeoverMs - 1.0));
        CHECK(t.Update(true, 1000.0 + kTakeoverMs));
    }

    // A break restarts the count; a start at time 0 counts too
    {
        Takeover t;
        CHECK(!t.Update(true, 0.0));
        CHECK(!t.Update(true, 4000.0));
        CHECK(!t.Update(false, 4100.0));
        CHECK(!t.Update(true, 4200.0));
        CHECK(!t.Update(true, 4200.0 + kTakeoverMs - 1.0));
        CHECK(t.Update(true, 4200.0 + kTakeoverMs));
    }

    // Rebuilds: a new size at once; a flip of the depth direction only once it holds
    {
        RebuildSettle<Key> s;
        const Key built { 1920, false };

        CHECK(!s.Now(built, built, false));
        CHECK(s.Now(built, Key { 2560, false }, false));

        const Key flipped { 1920, true };
        bool rebuilt = false;

        for (uint32_t i = 1; i < kKeySettleFrames; ++i)
            rebuilt = rebuilt || s.Now(built, flipped, true);

        CHECK(!rebuilt);
        CHECK(s.Now(built, flipped, true));

        // A flip that goes back before it settles: no rebuild, and the count starts over
        RebuildSettle<Key> t;

        for (uint32_t i = 1; i < kKeySettleFrames; ++i)
            CHECK(!t.Now(built, flipped, true));

        CHECK(!t.Now(built, built, false));
        CHECK(!t.Now(built, flipped, true));
    }

    printf(fails == 0 ? "nr_virtual_upscaler_hold_smoke: PASS\n" : "nr_virtual_upscaler_hold_smoke: %d FAIL\n", fails);
    return fails == 0 ? 0 : 1;
}
