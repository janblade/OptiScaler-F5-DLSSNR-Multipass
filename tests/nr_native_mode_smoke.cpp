// Host check of DlssNr_NativeMode.h: the mode selector of "NR without a game upscaler". No GPU and no game needed.
// cl /std:c++20 /EHsc /W4 tests/nr_native_mode_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_NativeMode.h"

#include <cstdio>

using namespace DlssNrNativeMode;

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

static bool Same(const Keys& a, const Keys& b)
{
    return a.depthFinder == b.depthFinder && a.motion == b.motion && a.input == b.input && a.upscaler == b.upscaler;
}

static Keys K(bool depthFinder, bool motion, bool input, bool upscaler)
{
    return { depthFinder, motion, input, upscaler };
}

int main()
{
    // Each mode's keys, whatever NR Pass at: was.
    for (bool fp : { false, true })
    {
        CHECK(Same(ForMode(Mode::Off, fp).keys, K(false, false, false, false)));
        CHECK(Same(ForMode(Mode::NrOnly, fp).keys, K(true, true, true, false)));
        CHECK(Same(ForMode(Mode::NrAndFrameGeneration, fp).keys, K(true, true, false, true)));
    }

    // Finished Picture: Off leaves it alone; NR only turns it on; NR + frame generation turns it off. A change clears a
    // session failure, as the NR Pass at: combo does; no change writes nothing.
    {
        CHECK(!ForMode(Mode::Off, false).finishedPicture.has_value() && !ForMode(Mode::Off, false).retryAfterFailure);
        CHECK(!ForMode(Mode::Off, true).finishedPicture.has_value() && !ForMode(Mode::Off, true).retryAfterFailure);

        const auto nrOnlyFromOff = ForMode(Mode::NrOnly, false);
        CHECK(nrOnlyFromOff.finishedPicture == std::optional<bool>(true) && nrOnlyFromOff.retryAfterFailure);
        const auto nrOnlyAlreadyOn = ForMode(Mode::NrOnly, true);
        CHECK(!nrOnlyAlreadyOn.finishedPicture.has_value() && !nrOnlyAlreadyOn.retryAfterFailure);

        const auto fgFromOn = ForMode(Mode::NrAndFrameGeneration, true);
        CHECK(fgFromOn.finishedPicture == std::optional<bool>(false) && fgFromOn.retryAfterFailure);
        const auto fgAlreadyOff = ForMode(Mode::NrAndFrameGeneration, false);
        CHECK(!fgAlreadyOff.finishedPicture.has_value() && !fgAlreadyOff.retryAfterFailure);
    }

    // Read-back: every combination of the four keys. The depth finder never decides the mode.
    for (int bits = 0; bits < 16; ++bits)
    {
        const Keys k = K(bits & 1, bits & 2, bits & 4, bits & 8);
        const Shown shown = FromKeys(k);

        if (!k.motion)
            CHECK(shown == Shown::Off);
        else if (k.upscaler)
            CHECK(shown == Shown::NrAndFrameGeneration); // also with input on: the upscaler is what runs
        else if (k.input)
            CHECK(shown == Shown::NrOnly);
        else
            CHECK(shown == Shown::MotionOnly);
    }

    // The cases the old single checkbox got wrong, spelled out.
    CHECK(FromKeys(K(true, true, true, true)) == Shown::NrAndFrameGeneration);
    CHECK(FromKeys(K(false, true, false, true)) ==
          Shown::NrAndFrameGeneration); // depth unticked afterwards: mode stays
    CHECK(FromKeys(K(false, true, true, false)) == Shown::NrOnly);
    CHECK(FromKeys(K(true, false, true, false)) == Shown::Off); // no motion, nothing runs
    CHECK(FromKeys(K(true, false, false, true)) == Shown::Off);
    CHECK(FromKeys(K(true, true, false, false)) == Shown::MotionOnly);

    // A mode reads back as itself.
    for (bool fp : { false, true })
    {
        CHECK(FromKeys(ForMode(Mode::Off, fp).keys) == Shown::Off);
        CHECK(FromKeys(ForMode(Mode::NrOnly, fp).keys) == Shown::NrOnly);
        CHECK(FromKeys(ForMode(Mode::NrAndFrameGeneration, fp).keys) == Shown::NrAndFrameGeneration);
    }

    // Warnings.
    CHECK(WarningFor(Shown::NrOnly, true, true, false) == Warning::None);
    CHECK(WarningFor(Shown::NrOnly, true, true, true) == Warning::Dx11FrameGeneration);
    CHECK(WarningFor(Shown::NrOnly, false, false, true) == Warning::Dx11FrameGeneration); // the one to act on first
    CHECK(WarningFor(Shown::NrOnly, false, true, false) == Warning::NrDisabled);
    CHECK(WarningFor(Shown::NrOnly, false, false, false) == Warning::NrDisabled);
    CHECK(WarningFor(Shown::NrOnly, true, false, false) == Warning::NeedsFinishedPicture);

    CHECK(WarningFor(Shown::NrAndFrameGeneration, true, false, false) == Warning::None);
    CHECK(WarningFor(Shown::NrAndFrameGeneration, true, true, true) == Warning::None);
    CHECK(WarningFor(Shown::NrAndFrameGeneration, false, false, true) == Warning::NrDisabled);

    for (int bits = 0; bits < 8; ++bits)
    {
        CHECK(WarningFor(Shown::Off, bits & 1, bits & 2, bits & 4) == Warning::None);
        CHECK(WarningFor(Shown::MotionOnly, bits & 1, bits & 2, bits & 4) == Warning::None);
    }

    printf(fails == 0 ? "native mode: ok\n" : "native mode: %d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
