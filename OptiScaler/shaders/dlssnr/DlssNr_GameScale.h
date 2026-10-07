#pragma once

#include <cstdint>

// The scale Game exposure's white point is built on ([DlssNr] GameExposureScale). Header-only and free of the GPU and
// the config, so a host test (tests/nr_game_scale_smoke.cpp) can pin the rule both backends and Tune share.
namespace DlssNrGameScale
{
// With the game's exposure: the base is PreExposure / exposure, the exposure value the game reports to DLSS SR.
inline constexpr uint32_t kWithExposure = 0;
// The game's colour as is: games with built-in DLSS-NR hand the model their colour exactly as it arrives, so the base is 1
// and the Trim's 0 EV is that colour, whatever exposure the game reports.
inline constexpr uint32_t kAsIs = 1;

// What the Trim multiplies to give the model's white point, 0 without an exposure reading.
inline float WhiteBase(uint32_t scale, float preExposure, float exposure)
{
    if (scale == kAsIs)
        return 1.0f;

    return exposure > 1e-6f ? preExposure / exposure : 0.0f;
}

// The key brightness points are stored and looked up by: the game's exposure, the scene-brightness proxy, in both scales.
// Under "as is" the base is constant, so keying by it would collapse every point onto one scene.
inline float AnchorKey(float preExposure, float exposure) { return exposure > 1e-6f ? preExposure / exposure : 0.0f; }
} // namespace DlssNrGameScale
