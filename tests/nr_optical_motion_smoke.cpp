// Host check of the MotionSource=optical rules (dlssnr/DlssNr_OpticalMotion.h) and of its place among Optical F5Low's
// motion users (dlssnr/DlssNr_NativeMode.h).
// cl /std:c++20 /EHsc /W4 tests/nr_optical_motion_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_NativeMode.h"
#include "../OptiScaler/dlssnr/DlssNr_OpticalMotion.h"

#include <cstdio>

using namespace DlssNr::OpticalMotion;

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
    // Wanted only at Finished Picture with the model on the finished picture, Neural Rendering on
    CHECK(Wanted({ .optical = true, .nrEnabled = true, .finishedPicture = true }));
    CHECK(!Wanted({ .optical = false, .nrEnabled = true, .finishedPicture = true }));
    CHECK(!Wanted({ .optical = true, .nrEnabled = false, .finishedPicture = true }));
    CHECK(!Wanted({ .optical = true, .nrEnabled = true, .finishedPicture = false }));
    CHECK(!Wanted({ .optical = true, .nrEnabled = true, .finishedPicture = true, .modelBeforeSr = true }));

    // The flow is for exactly the present it was measured on
    int queue = 0, otherQueue = 0, picture = 0, otherPicture = 0;
    const Present offered { &queue, &picture, 41, 2560, 1440 };
    CHECK(SamePresent(offered, { &queue, &picture, 41, 2560, 1440 }));
    CHECK(!SamePresent(offered, { &queue, &picture, 42, 2560, 1440 }));      // the producer skipped this present
    CHECK(!SamePresent(offered, { &otherQueue, &picture, 41, 2560, 1440 })); // another queue: no order between them
    CHECK(!SamePresent(offered, { &queue, &otherPicture, 41, 2560, 1440 })); // a Streamline application picture
    CHECK(!SamePresent(offered, { &queue, &picture, 41, 1920, 1080 }));      // a size change
    CHECK(!SamePresent({}, { nullptr, nullptr, 0, 0, 0 }));                  // nothing offered yet

    // Source switch: the first picture on either source after a change starts NR's history over
    bool last = false;
    auto c = Choose(false, false, last); // game, as before: no reset of its own
    CHECK(!c.optical && !c.reset && !last);
    c = Choose(true, true, last); // game -> optical
    CHECK(c.optical && c.reset && last);
    c = Choose(true, true, last); // optical continues
    CHECK(c.optical && !c.reset);
    c = Choose(true, false, last); // no flow for this present: the game's motion for it, the history goes on
    CHECK(!c.optical && !c.reset && last);
    c = Choose(true, true, last); // the flow is back: no second restart
    CHECK(c.optical && !c.reset);
    c = Choose(false, true, last); // switched back to game
    CHECK(!c.optical && c.reset);
    c = Choose(false, false, last);
    CHECK(!c.optical && !c.reset);

    // A motion user of its own: with the game's upscaler and nothing else on, the motion still runs
    using DlssNrNativeMode::MotionUsed;
    CHECK(MotionUsed({ .opticalForGame = true }));
    CHECK(!MotionUsed({ .nrEnabled = true, .finishedPicture = true }));

    printf(fails == 0 ? "nr optical motion: ok\n" : "nr optical motion: %d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
