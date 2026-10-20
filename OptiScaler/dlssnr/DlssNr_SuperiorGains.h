#pragma once
#include "DlssNr_SuperiorScaling.h"
#include "DlssNr_GuidedShaping.h"
#include "DlssNr_InterPassPolicy.h"
#include "PassProfiles.h"

namespace DlssNr::SuperiorScaling
{
// Same scale correction and slider product as v59. The table is selected per NR pass,
// independently of the private DLSS SR A/K preset.
inline InterPass::ScheduleGains PassGains(const Config& cfg, unsigned pass, float scale)
{
    const auto tuning = Profiles::PassTuning(cfg, pass);
    const auto style = Profiles::PassStyle(cfg, pass);
    const auto preset = Profiles::PassPreset(cfg, pass);
    const float exponent = std::log2(1.0f / std::clamp(scale, .25f, 1.0f));
    const auto gain = [&](float p50) {
        const float scaled = std::pow(std::max(0.0f, p50), exponent);
        return Profiles::ModelAdjustedGain(std::isfinite(scaled) ? scaled : 1.0f,
                                          tuning.structure, tuning.intensity);
    };
    return { gain(High(style, preset)), gain(Low(style, preset)) };
}
inline InterPass::ScheduleGains BlockGains(const Config& cfg, unsigned first, unsigned completed, float scale)
{
    return { Profiles::ProductGuidedHigh(first, completed, true,
                 [&](unsigned p) { return PassGains(cfg, p, scale).high; }),
             Profiles::MeanGuidedLow(first, completed,
                 [&](unsigned p) { return PassGains(cfg, p, scale).low; }) };
}
inline InterPass::GainSchedule Schedule(const Config& cfg, unsigned passes, float scale)
{
    return InterPass::MakeGainSchedule(passes, InterPass::Eligible(8u, scale, passes, false), true, false,
                                      [&](unsigned p) { return PassGains(cfg, p, scale); });
}
// NR consumes vector units belonging to the motion region it actually receives. The private
// DLAA receives work-size vectors already converted to pixels, and therefore uses scale 1.
inline float MotionScale(float gameScale, unsigned motionExtent, unsigned referenceExtent)
{
    return referenceExtent ? gameScale * float(motionExtent) / float(referenceExtent) : gameScale;
}
}
