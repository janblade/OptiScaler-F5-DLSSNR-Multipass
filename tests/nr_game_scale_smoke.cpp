// Host test for shaders/dlssnr/DlssNr_GameScale.h: the white point base of Game exposure's two scales, and the key
// brightness points use in both.
//
// Build: cl /nologo /std:c++20 /EHsc /W4 tests\nr_game_scale_smoke.cpp
#include <cmath>
#include <cstdio>
#include <string>
#include "../OptiScaler/shaders/dlssnr/DlssNr_GameScale.h"

using namespace DlssNrGameScale;

static int fails = 0;
static void Check(bool ok, const std::string& label)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", label.c_str());
        ++fails;
    }
}

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main()
{
    Check(kWithExposure == 0 && kAsIs == 1, "the scale numbers are the ini's: 0 with exposure, 1 as is");

    // NBA 2K27: PreExposure 1, exposure 1.3195.
    Check(Near(WhiteBase(kWithExposure, 1.0f, 1.3195f), 0.7579f), "with exposure: NBA's base is 0.7579");
    Check(WhiteBase(kAsIs, 1.0f, 1.3195f) == 1.0f, "as is: NBA's base is exactly 1");

    // With exposure is PreExposure / exposure, exactly as before the setting existed.
    Check(WhiteBase(kWithExposure, 2.0f, 0.5f) == 2.0f / 0.5f, "with exposure divides pre-exposure by exposure");
    Check(WhiteBase(kWithExposure, 0.25f, 8.0f) == 0.25f / 8.0f, "with exposure is the same division for any pair");

    // As is ignores both numbers.
    Check(WhiteBase(kAsIs, 4.0f, 0.1f) == 1.0f && WhiteBase(kAsIs, 0.0f, 100.0f) == 1.0f, "as is ignores the exposure");
    Check(WhiteBase(kAsIs, 1.0f, 0.0f) == 1.0f, "as is needs no exposure reading");

    // No reading: with exposure has no base (callers fall through), as is does not need one.
    Check(WhiteBase(kWithExposure, 1.0f, 0.0f) == 0.0f, "exposure 0 gives no base with exposure");
    Check(WhiteBase(kWithExposure, 1.0f, 1e-7f) == 0.0f, "an exposure below 1e-6 gives no base with exposure");

    // Brightness points are keyed by the game's exposure in both scales.
    Check(Near(AnchorKey(1.0f, 1.3195f), 0.7579f), "the anchor key is PreExposure / exposure");
    Check(AnchorKey(1.0f, 0.0f) == 0.0f, "no exposure, no anchor key");
    Check(AnchorKey(2.0f, 0.5f) == WhiteBase(kWithExposure, 2.0f, 0.5f), "the key equals the with-exposure base");
    Check(AnchorKey(1.0f, 1.3195f) != WhiteBase(kAsIs, 1.0f, 1.3195f), "as is keeps its points off the constant base");

    if (fails != 0)
        return 1;

    std::printf("PASS: nr_game_scale_smoke\n");
    return 0;
}
