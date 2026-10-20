// Host test for OptiScaler/shaders/dlssnr/DlssNr_ExposureDefault.h: the white point source and Game exposure scale
// a game gets while the user has not chosen.
// cl /std:c++20 /EHsc /W4 tests/nr_exposure_default_smoke.cpp
#include "../OptiScaler/shaders/dlssnr/DlssNr_ExposureDefault.h"

#include <cstdio>

using namespace DlssNrExposureDefault;

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
    // auto: Game exposure once the game has handed one over, Automatic before and without
    CHECK(Source(kSourceAuto, true) == kSourceGame);
    CHECK(Source(kSourceAuto, false) == kSourceAutomatic);

    // A choice the user made stands, whatever the game hands over
    for (uint32_t chosen = 0; chosen <= kSourceAutomatic; ++chosen)
    {
        CHECK(Source(chosen, true) == chosen);
        CHECK(Source(chosen, false) == chosen);
    }

    // auto is not one of the real choices (a default equal to a choice would be saved as auto)
    CHECK(kSourceAuto > kSourceAutomatic && kScaleAuto > kScaleAsIs);
    CHECK(Source(9, false) == kSourceAutomatic); // out of range: the last real choice, as before

    // What the game offers, frame by frame
    {
        Offer offer;
        CHECK(!offer.Usable());
        offer.Update(false, false, false);
        CHECK(!offer.Usable()); // no texture: Automatic

        CHECK(!offer.Pending() && !offer.Confirmed());
        offer.Update(true, false, false);
        CHECK(offer.Usable()); // a texture: Game exposure, which then reads it
        CHECK(Source(kSourceAuto, offer.Usable()) == kSourceGame);
        CHECK(offer.Pending() && !offer.Confirmed()); // not read clean yet: Auto's scan keeps what it holds

        // Read without a clean value for kGiveUpFrames frames: Automatic back, for good
        for (uint32_t i = 0; i < kGiveUpFrames; ++i)
            offer.Update(true, false, true);
        CHECK(!offer.Usable() && offer.Unusable());
        CHECK(!offer.Pending() && !offer.Confirmed()); // given up on: Auto's scan searches
        offer.Update(true, true, true);
        CHECK(!offer.Usable() && !offer.Confirmed());
        CHECK(Source(kSourceAuto, offer.Usable()) == kSourceAutomatic);
    }
    {
        // A clean value in time: Game exposure stays
        Offer offer;
        for (uint32_t i = 0; i < 4; ++i)
            offer.Update(true, false, true);
        offer.Update(true, true, true);
        CHECK(offer.Confirmed() && !offer.Pending());
        for (uint32_t i = 0; i < kGiveUpFrames * 2; ++i)
            offer.Update(i % 2 == 0, false, true); // frames without the texture or a reading later do not matter
        CHECK(offer.Usable() && offer.Confirmed());
    }
    {
        // Frames while another source is chosen do not count (nothing reads the texture then)
        Offer offer;
        for (uint32_t i = 0; i < kGiveUpFrames * 2; ++i)
            offer.Update(true, false, false);
        CHECK(offer.Usable());
    }

    // Scale: as is for a measured game, the game's setting otherwise; a chosen scale stands
    CHECK(Scale(kScaleAuto, true) == kScaleAsIs);
    CHECK(Scale(kScaleAuto, false) == kScaleGameSetting);
    CHECK(Scale(kScaleGameSetting, true) == kScaleGameSetting);
    CHECK(Scale(kScaleAsIs, false) == kScaleAsIs);

    // The list, by exe name or path, any case
    CHECK(IsKnownAsIsGame("NBA2K27.exe"));
    CHECK(IsKnownAsIsGame("J:\\Games\\NBA 2K27\\NBA2K27.exe"));
    CHECK(!IsKnownAsIsGame("RDR2.exe"));
    CHECK(!IsKnownAsIsGame("re2.exe"));

    if (fails != 0)
    {
        printf("exposure default: %d failure(s)\n", fails);
        return 1;
    }

    printf("exposure default: ok\n");
    return 0;
}
