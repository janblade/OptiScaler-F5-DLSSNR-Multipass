#pragma once
#include "DlssNr_SuperiorScaling.h"
#include "DlssNr_GuidedShaping.h"
#include "DlssNr_InterPassPolicy.h"
#include "PassProfiles.h"

namespace DlssNr::SuperiorScaling
{
// Same per-band correction and slider product as v59, using independent high/low scales.
// The table is selected per NR pass,
// independently of the private DLSS SR A/K preset.
// Scale calibrated attenuation by SuperiorShaping (0 = 100% NR strength preserved, 1 = legacy upstream cut).
// Direct DLSS SR always runs at neutral 1.0/1.0 (full NR strength, no band attenuation).
inline float ApplyShaping(float gain, float shaping)
{
    const float s = std::isfinite(shaping) ? std::clamp(shaping, 0.0f, 1.0f) : 0.0f;
    if (s <= 0.0f)
        return 1.0f;
    return 1.0f + s * (gain - 1.0f);
}
inline InterPass::ScheduleGains PassGains(const Config& cfg, unsigned pass, FrequencyScales scales, bool mainOnly)
{
    const uint32_t mode = cfg.DlssNrSuperiorMode.value_or_default();
    if (DirectSrSelected(mode) || IntrinsicOracleSelected(mode))
        return { 1.0f, 1.0f };
    const float shaping = cfg.DlssNrSuperiorShaping.value_or_default();
    if (!std::isfinite(shaping) || shaping <= 0.0f)
        return { 1.0f, 1.0f };
    const auto tuning = Profiles::PassTuning(cfg, pass);
    const auto style = GainStyleSelection(cfg, pass);
    const auto preset = GainPresetSelection(cfg, pass);
    const auto raw = BandGains(style, preset, mainOnly, scales, tuning.structure, tuning.intensity);
    return { ApplyShaping(raw.high, shaping), ApplyShaping(raw.low, shaping) };
}
inline InterPass::ScheduleGains BlockGains(const Config& cfg, unsigned first, unsigned completed,
                                           FrequencyScales scales, bool mainOnly)
{
    return { Profiles::ProductGuidedHigh(first, completed, true,
                                         [&](unsigned p) { return PassGains(cfg, p, scales, mainOnly).high; }),
             Profiles::MeanGuidedLow(first, completed,
                                     [&](unsigned p) { return PassGains(cfg, p, scales, mainOnly).low; }) };
}
inline InterPass::GainSchedule Schedule(const Config& cfg, unsigned passes, FrequencyScales scales, bool mainOnly)
{
    return InterPass::MakeGainSchedule(passes, InterPass::Eligible(8u, scales.low, passes, false), true, false,
                                       [&](unsigned p) { return PassGains(cfg, p, scales, mainOnly); });
}
} // namespace DlssNr::SuperiorScaling
