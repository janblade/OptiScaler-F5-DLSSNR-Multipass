#pragma once

// What Optical F5Low's virtual upscaler (native/VirtualUpscalerDriver.h) does with the picture on a frame. Pure, so a
// host test can run it (tests/f5low_vu_route_smoke.cpp).
//
// The backend (DLSS, FSR, XeSS) exists so frame generation has an upscaler feature to take motion and depth from. Its pass
// over the picture is not needed for that: the picture is already full size and antialiased. By default it is skipped:
// with Neural Rendering on, NR runs on the picture in place, as in "NR only" mode (HDR10 included: that entry converts it
// itself), and the backend is only counted as a frame. The player can ask for the backend's pass (the menu's "Run the
// upscaler on the picture", NativeUpscalerPass true), with NR on or off: at the same size and without jitter it adds no
// detail, but its temporal pass steadies shimmer in a game with weak antialiasing.

namespace native
{

// [DlssNr] NativeUpscalerPass: unset = Auto (no pass), false = Off (no pass), true = On (the backend's pass).
enum class VuPassSetting
{
    Auto,
    Off,
    On
};

enum class VuNrRoute
{
    None,        // Neural Rendering is off and no pass asked for: the picture goes on as the game made it
    InPlace,     // NR runs on the picture in place; the backend is only counted
    BackendPass, // the backend evaluates, NR (when on) runs on its output, and that is copied back over the picture
};

inline VuPassSetting VuPassSettingFromIni(bool hasValue, bool value)
{
    return !hasValue ? VuPassSetting::Auto : value ? VuPassSetting::On : VuPassSetting::Off;
}

inline VuNrRoute DecideVuNrRoute(bool nrEnabled, VuPassSetting setting)
{
    if (setting == VuPassSetting::On)
        return VuNrRoute::BackendPass;

    return nrEnabled ? VuNrRoute::InPlace : VuNrRoute::None;
}

} // namespace native
