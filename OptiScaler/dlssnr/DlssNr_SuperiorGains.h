#pragma once
#include "DlssNr_SuperiorScaling.h"
#include "DlssNr_GuidedShaping.h"
#include "DlssNr_InterPassPolicy.h"
#include "PassProfiles.h"

namespace DlssNr::SuperiorScaling
{
// Same scale correction and slider product as v59. The table is selected per NR pass,
// independently of the private DLSS SR A/K preset.
inline InterPass::ScheduleGains PassGains(const Config& cfg, unsigned pass, float scale, bool mainOnly)
{
    const auto tuning = Profiles::PassTuning(cfg, pass);
    const auto style = Profiles::PassStyle(cfg, pass);
    const auto preset = Profiles::PassPreset(cfg, pass);
    const auto gain = [&](float p50) { return EffectiveBand(p50, scale, tuning.structure, tuning.intensity); };
    return { gain(High(style, preset, mainOnly)), gain(Low(style, preset, mainOnly)) };
}
inline InterPass::ScheduleGains BlockGains(const Config& cfg, unsigned first, unsigned completed, float scale,
                                           bool mainOnly)
{
    return { Profiles::ProductGuidedHigh(first, completed, true,
                                         [&](unsigned p) { return PassGains(cfg, p, scale, mainOnly).high; }),
             Profiles::MeanGuidedLow(first, completed,
                                     [&](unsigned p) { return PassGains(cfg, p, scale, mainOnly).low; }) };
}
inline InterPass::GainSchedule Schedule(const Config& cfg, unsigned passes, float scale, bool mainOnly)
{
    return InterPass::MakeGainSchedule(passes, InterPass::Eligible(8u, scale, passes, false), true, false,
                                       [&](unsigned p) { return PassGains(cfg, p, scale, mainOnly); });
}
} // namespace DlssNr::SuperiorScaling
