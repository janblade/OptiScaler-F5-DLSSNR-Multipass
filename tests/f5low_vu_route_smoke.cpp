// Host check of native/VirtualUpscalerMode.h: what Optical F5Low's virtual upscaler does with the picture, by the
// ini switch NativeUpscalerPass (auto, off, on). cl /std:c++20 /EHsc /W4 tests/f5low_vu_route_smoke.cpp
#include "../OptiScaler/native/VirtualUpscalerMode.h"

#include <cstdio>

using namespace native;

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
    // NR off: the picture goes on as the game made it, unless the switch asks for the backend's pass (a steadier picture
    // with NR off too)
    CHECK(DecideVuNrRoute(false, VuPassSetting::Auto) == VuNrRoute::None);
    CHECK(DecideVuNrRoute(false, VuPassSetting::Off) == VuNrRoute::None);
    CHECK(DecideVuNrRoute(false, VuPassSetting::On) == VuNrRoute::BackendPass);

    // NR on: on the game's picture (SDR, scRGB and HDR10 alike: native input converts HDR10 itself), unless the switch
    // asks for the backend's pass
    CHECK(DecideVuNrRoute(true, VuPassSetting::Auto) == VuNrRoute::InPlace);
    CHECK(DecideVuNrRoute(true, VuPassSetting::Off) == VuNrRoute::InPlace);
    CHECK(DecideVuNrRoute(true, VuPassSetting::On) == VuNrRoute::BackendPass);

    // The ini value: unset is auto, false is off, true is on
    CHECK(VuPassSettingFromIni(false, false) == VuPassSetting::Auto);
    CHECK(VuPassSettingFromIni(false, true) == VuPassSetting::Auto); // an unset key holds no value to read
    CHECK(VuPassSettingFromIni(true, false) == VuPassSetting::Off);
    CHECK(VuPassSettingFromIni(true, true) == VuPassSetting::On);

    if (fails == 0)
        printf("PASS\n");

    return fails == 0 ? 0 : 1;
}
