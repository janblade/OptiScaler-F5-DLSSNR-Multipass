#pragma once

// Automatic exposure's eye adaptation: the exposure the picture uses eases toward each new reading instead of jumping to
// it.
//
// Why: the meter re-reads every evaluation, and before this the encode and resolve used that reading as it was. In NBA
// 2K27 a made basket's zoom swung it 1.45 -> 3.30 -> 1.46 (1.19 EV) within seconds, so the model's input brightness
// jumped with the camera and NR's tone flipped with it ("too yellow"). A game's own exposure adapts over time; this
// gives Automatic the same.
//
// The law: exponential in log space, eased = previous * (reading / previous) ^ blend, blend = 1 - exp(-dt / tau), with
// dt the time since the last evaluation. Log space, so brightening and darkening by the same stops take the same time;
// the exponential in dt, so the response per second is the same at any frame rate. tau is the menu's "Eye adaptation"
// in seconds, 0 = off (the reading as it is).
//
// It snaps -- takes the reading at once -- on a cut the game signals (the DLSS reset), and whenever the eased value is
// stale: the first evaluation, one after evaluations Automatic did not run on (another source, finished picture), and
// after Invalidate (the meter was discarded). A cut is a new scene, and easing across it would carry the old scene's
// exposure into the new one for a second, the failure the removed cut-snapping meter in ResolveWhitePoint describes.
// NR switched off counts no evaluations at all; the time that passed meanwhile does the same job (a blend of about 1
// after a few seconds).
//
// 0 is the meter's "no reading" (a black frame, a fade): the last value is kept -- except on a snap, which then writes
// 0 as well, so the next real reading snaps instead of easing from before the cut (a cut through a black frame, a
// source switch on a menu frame, a new texture's undefined contents). A 0 reads as no reading downstream too: the
// white point falls back to the host's last value, as before.
//
// Pure CPU: the D3D12 and Vulkan paths work out each evaluation's Step, the shader (precompile/dlssnr_exposure_adapt.hlsl,
// DXIL and SPIR-V) applies Ease with it to the texture it wrote the evaluation before, tests/nr_exposure_adapt_smoke.cpp
// checks both halves. Ease is mirrored in that shader line for line; change them together.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace DlssNrExposureAdapt
{
constexpr float kDefaultSeconds = 1.0f;
constexpr float kMaxSeconds = 5.0f;

// The setting as used: 0 (off) to kMaxSeconds, a broken value -> the default.
inline float Seconds(float setting)
{
    if (!std::isfinite(setting))
        return kDefaultSeconds;
    return std::clamp(setting, 0.0f, kMaxSeconds);
}

// The share of the way to the new reading one evaluation takes. Off (tau 0 or broken): all of it. A clock that stood
// still or went backwards: none.
inline float Blend(float dtSeconds, float tauSeconds)
{
    if (!std::isfinite(tauSeconds) || tauSeconds <= 0.0f)
        return 1.0f;
    if (!std::isfinite(dtSeconds) || dtSeconds <= 0.0f)
        return 0.0f;
    return 1.0f - std::exp(-dtSeconds / tauSeconds);
}

// An exposure the white point may be built from: the same test as the shader's WhitePoint().
inline bool Valid(float exposure) { return std::isfinite(exposure) && exposure > 1e-8f && exposure < 1e8f; }

// The eased exposure: `previous` is last evaluation's eased value, `reading` this evaluation's meter.
inline float Ease(float previous, float reading, float blend, bool snap)
{
    if (!Valid(reading))
        return !snap && Valid(previous) ? previous : 0.0f;
    if (snap || !Valid(previous))
        return reading;

    const float t = std::isfinite(blend) ? std::clamp(blend, 0.0f, 1.0f) : 0.0f;
    return std::exp2(std::log2(previous) + (std::log2(reading) - std::log2(previous)) * t);
}

// One evaluation's instructions for the shader.
struct Step
{
    float blend = 1.0f;
    bool snap = true;
};

// Keeps the clock and says when the eased value is stale. `evaluation` counts NR evaluations (whether or not Automatic
// ran on them), so a gap in it means the texture was not updated in between.
class Adapter
{
  public:
    Step Next(uint64_t evaluation, double seconds, float tauSeconds, bool reset)
    {
        Step s;
        const bool continuous = primed_ && evaluation == last_ + 1 && !reset;

        s.snap = !continuous;
        s.blend = continuous ? Blend((float) (seconds - lastSeconds_), tauSeconds) : 1.0f;

        primed_ = true;
        last_ = evaluation;
        lastSeconds_ = seconds;
        return s;
    }

    // The next evaluation snaps.
    void Invalidate() { primed_ = false; }

  private:
    bool primed_ = false;
    uint64_t last_ = 0;
    double lastSeconds_ = 0.0;
};
} // namespace DlssNrExposureAdapt
