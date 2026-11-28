// Host check of the MotionSource=optical rules (dlssnr/DlssNr_OpticalMotion.h), Neural Rendering's and frame
// generation's, and of its place among Optical F5Low's motion users (dlssnr/DlssNr_NativeMode.h).
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

    // Frame generation's choice ([FrameGen] MotionSource), apart from Neural Rendering's
    const FgKeys fgOn { .optical = true, .fgEnabled = true, .upscalerInput = true, .xefgApiOutput = true,
                        .nativeDx12 = true };
    CHECK(FgWanted(fgOn));
    CHECK(FgRefused(fgOn) == FgRefusal::None);
    {
        auto k = fgOn;
        k.optical = false; // the key off: not wanted, but nothing refuses it either
        CHECK(!FgWanted(k) && FgRefused(k) == FgRefusal::None);
        k = fgOn;
        k.fgEnabled = false;
        CHECK(!FgWanted(k));
        k = fgOn;
        k.nativeDx12 = false; // D3D11 or Vulkan bridge
        CHECK(!FgWanted(k) && FgRefused(k) == FgRefusal::NotNativeDx12);
        k = fgOn;
        k.upscalerInput = false; // FSR FG / Streamline inputs give their own motion
        CHECK(!FgWanted(k) && FgRefused(k) == FgRefusal::NotUpscalerInput);
        k = fgOn;
        k.xefgApiOutput = false; // FSR frame generation, DLSS-G
        CHECK(!FgWanted(k) && FgRefused(k) == FgRefusal::Output);
        k = fgOn;
        k.resourceFlip = true;
        CHECK(!FgWanted(k) && FgRefused(k) == FgRefusal::ResourceFlip);
        k = fgOn;
        k.contextFits = false; // XeFG made for render-size motion: a restart
        CHECK(!FgWanted(k) && FgRefused(k) == FgRefusal::NeedsRestart);
    }

    // The two choices are independent: frame generation's alone runs the flow, Neural Rendering's stays the game's
    CHECK(FgWanted(fgOn) && !Wanted({ .optical = false, .nrEnabled = true, .finishedPicture = true }));
    // ... and Neural Rendering's alone leaves frame generation on the game's
    CHECK(Wanted({ .optical = true, .nrEnabled = true, .finishedPicture = true }) && !FgWanted({}));

    // At the call: never OptiScaler's own virtual upscaler call (full Optical F5Low mode), nor several views
    CHECK(FgAtCall(true, false, false));
    CHECK(!FgAtCall(false, false, false));
    CHECK(!FgAtCall(true, true, false));
    CHECK(!FgAtCall(true, false, true));

    // At the present: the flow only for the latest frame, this present, the frame generation picture's size
    CHECK(FgTagAtPresent(false, true, true, true) == FgTag::Nothing); // the call tagged the game's
    CHECK(FgTagAtPresent(true, true, true, true) == FgTag::Flow);
    CHECK(FgTagAtPresent(true, false, true, true) == FgTag::GameFallback); // frame generation runs behind
    CHECK(FgTagAtPresent(true, true, false, true) == FgTag::GameFallback); // no flow for this present
    CHECK(FgTagAtPresent(true, true, true, false) == FgTag::GameFallback); // game renders at another size

    // The fallback interpolates across its frame only when the game's motion is the flow's kind
    CHECK(!FgFallbackResets(false, false));
    CHECK(FgFallbackResets(true, false));
    CHECK(!FgFallbackResets(false, true)); // jitter: under a pixel, no reset

    printf(fails == 0 ? "nr optical motion: ok\n" : "nr optical motion: %d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
