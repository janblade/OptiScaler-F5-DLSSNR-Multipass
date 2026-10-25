#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace xefg
{
// Dynamic MFG's target, the presented frame rate it follows (XeFG_Dx12::DynamicTargetFps). `setFps`:
// FramerateTargetDMFG (0: auto); `limitFps`: OptiScaler's FPS limit (0: none); `refreshFps`: the display's refresh rate
// (0: unknown); `vsync`: XeFG presents with V-Sync (State::fgPresentVsync).
inline float DynamicTargetFps(float setFps, float limitFps, float refreshFps, bool vsync)
{
    float target = 0.0f;

    // The FPS limit counts shown frames: a target above it can't be reached
    if (setFps > 0.0f)
        target = limitFps > 0.0f ? std::min(setFps, limitFps) : setFps;
    else if (limitFps > 0.0f)
        target = limitFps;
    else // Auto: just under the refresh rate (116 at 120 Hz), where a VRR display's frame cap sits
        return refreshFps > 0.0f ? std::floor(refreshFps - refreshFps * refreshFps / 3600.0f) : 0.0f;

    // V-Sync holds the shown frame rate at the refresh rate: a target above it can't be reached either, and Dynamic MFG
    // kept checking steps that can't gain (Witcher 3, target 180 at 120 Hz: one more frame or one less every 20-60 s)
    if (vsync && refreshFps > 0.0f)
        target = std::min(target, refreshFps);

    return target;
}
} // namespace xefg
