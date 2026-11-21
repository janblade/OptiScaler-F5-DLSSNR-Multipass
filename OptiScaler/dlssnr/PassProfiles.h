#pragma once
#include <Config.h>
#include "NrNvfp4.h"
#include <algorithm>
#include <cmath>

namespace DlssNr::Profiles
{
struct NrPassTuning
{
    float intensity = 1.0f;
    float structure = 1.0f;
    float tone = 0.0f;
    float skin = -1.0f;
    bool autoMask = true;
    bool operator==(const NrPassTuning&) const = default;
};

// The model a pass asks for: the runtime's presets 0..3, or one of DlssNr::Nvfp4's (4, 15..20: Model A with the 4-bit
// kernels, a pass-in-one model's weight set, an FP8 set). PassPreset gives the runtime preset 1 for those.
inline unsigned int RequestedPreset(const Config& cfg, unsigned int pass)
{
    if (pass == 0)
        return cfg.DlssNrPreset.value_or_default();

    if (pass == 1 && cfg.DlssNrPass2Preset.has_value())
        return cfg.DlssNrPass2Preset.value();

    if (pass == 2 && cfg.DlssNrPass3Preset.has_value())
        return cfg.DlssNrPass3Preset.value();

    return cfg.DlssNrPreset.value_or_default();
}

// Whether any of the passes asks for one of DlssNr::Nvfp4's models.
inline bool WantsNvfp4(const Config& cfg, unsigned int passes)
{
    for (unsigned int pass = 0; pass < passes; ++pass)
        if (Nvfp4::IsNvfp4(RequestedPreset(cfg, pass)))
            return true;
    return false;
}

inline unsigned int PassPreset(const Config& cfg, unsigned int pass)
{
    const unsigned int requested = RequestedPreset(cfg, pass);
    // A 4-bit model is asked for by the preset number; the runtime itself gets Model A (preset 1), whose launches our
    // kernels replace one by one.
    if (Nvfp4::IsNvfp4(requested))
        return 1u;

    const unsigned int base = std::min(requested, 3u);

    return base;
}

inline unsigned int PassStyle(const Config& cfg, unsigned int pass)
{
    const unsigned int base = std::min(cfg.DlssNrStyle.value_or_default(), 2u);

    if (pass == 1 && cfg.DlssNrPass2Style.has_value())
        return std::min(cfg.DlssNrPass2Style.value(), 2u);

    if (pass == 2 && cfg.DlssNrPass3Style.has_value())
        return std::min(cfg.DlssNrPass3Style.value(), 2u);

    if (pass >= 3 && pass < 30 && cfg.DlssNrExtraPasses[pass - 3].style.has_value())
        return std::min(cfg.DlssNrExtraPasses[pass - 3].style.value(), 2u);

    return base;
}

inline NrPassTuning PassTuning(const Config& cfg, unsigned int pass)
{
    NrPassTuning result { cfg.DlssNrIntensity.value_or_default(),
                          cfg.DlssNrLocalStructure.value_or_default(),
                          pass == 0 ? cfg.DlssNrLocalTone.value_or_default() : 0.0f,
                          cfg.DlssNrSkinStructure.value_or_default(),
                          cfg.DlssNrAutoMask.value_or_default() };
    if (pass == 1)
    {
        if (cfg.DlssNrPass2Intensity.has_value())
            result.intensity = cfg.DlssNrPass2Intensity.value();
        if (cfg.DlssNrPass2LocalStructure.has_value())
            result.structure = cfg.DlssNrPass2LocalStructure.value();
        if (cfg.DlssNrPass2LocalTone.has_value())
            result.tone = cfg.DlssNrPass2LocalTone.value();
        if (cfg.DlssNrPass2SkinStructure.has_value())
            result.skin = cfg.DlssNrPass2SkinStructure.value();
        if (cfg.DlssNrPass2AutoMask.has_value())
            result.autoMask = cfg.DlssNrPass2AutoMask.value();
    }
    if (pass == 2)
    {
        if (cfg.DlssNrPass3Intensity.has_value())
            result.intensity = cfg.DlssNrPass3Intensity.value();
        if (cfg.DlssNrPass3LocalStructure.has_value())
            result.structure = cfg.DlssNrPass3LocalStructure.value();
        if (cfg.DlssNrPass3LocalTone.has_value())
            result.tone = cfg.DlssNrPass3LocalTone.value();
        if (cfg.DlssNrPass3SkinStructure.has_value())
            result.skin = cfg.DlssNrPass3SkinStructure.value();
        if (cfg.DlssNrPass3AutoMask.has_value())
            result.autoMask = cfg.DlssNrPass3AutoMask.value();
    }
    if (pass >= 3 && pass < 30)
    {
        const auto& extra = cfg.DlssNrExtraPasses[pass - 3];
        if (extra.intensity.has_value()) result.intensity = extra.intensity.value();
        if (extra.structure.has_value()) result.structure = extra.structure.value();
        if (extra.tone.has_value()) result.tone = extra.tone.value();
        if (extra.skin.has_value()) result.skin = extra.skin.value();
        if (extra.autoMask.has_value()) result.autoMask = extra.autoMask.value();
    }
    const auto bounded = [](float value, float fallback, float minimum) {
        return std::isfinite(value) ? std::clamp(value, minimum, 2.0f) : fallback;
    };
    result.intensity = bounded(result.intensity, 1.0f, 0.0f);
    result.structure = bounded(result.structure, 1.0f, 0.0f);
    result.tone = bounded(result.tone, pass == 0 ? 1.0f : 0.0f, 0.0f);
    result.skin = bounded(result.skin, -1.0f, -1.0f);
    return result;
}
}
