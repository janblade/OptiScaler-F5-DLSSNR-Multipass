#pragma once

#include "DlssNr_Lut.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace DlssNr
{
struct CustomLutParams
{
    float exposure = 0.0f;   // EV
    float brightness = 0.0f; // -1..1
    float gamma = 0.0f;      // -1..1
    float blacks = 0.0f;     // -1..1
    float whites = 0.0f;     // -1..1
    float contrast = 0.0f;   // -1..1
    float temperature = 0.0f; // -1..1
    float saturation = 0.0f; // -1..1

    bool operator==(const CustomLutParams&) const = default;
};

inline float CustomLutClamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

inline float CustomLutLuma(float r, float g, float b) { return r * 0.2126f + g * 0.7152f + b * 0.0722f; }

inline void CustomLutApply(const CustomLutParams& p, float r, float g, float b, float out[3])
{
    // Exposure in EV.
    const float exposureMul = std::exp2(std::clamp(p.exposure, -3.0f, 3.0f));
    r *= exposureMul;
    g *= exposureMul;
    b *= exposureMul;

    // Brightness as simple offset in normalized display space.
    const float brightness = std::clamp(p.brightness, -1.0f, 1.0f) * 0.5f;
    r += brightness;
    g += brightness;
    b += brightness;

    // Contrast around mid gray.
    const float contrast = std::clamp(p.contrast, -1.0f, 1.0f);
    const float contrastFactor = (1.0f + contrast) / std::max(1e-3f, 1.0f - contrast * 0.99f);
    r = (r - 0.5f) * contrastFactor + 0.5f;
    g = (g - 0.5f) * contrastFactor + 0.5f;
    b = (b - 0.5f) * contrastFactor + 0.5f;

    // Blacks/whites levels.
    const float black = std::clamp(p.blacks, -1.0f, 1.0f) * 0.25f;
    const float white = 1.0f - std::clamp(p.whites, -1.0f, 1.0f) * 0.25f;
    const float invRange = 1.0f / std::max(1e-3f, white - black);
    r = (r - black) * invRange;
    g = (g - black) * invRange;
    b = (b - black) * invRange;

    r = std::max(r, 0.0f);
    g = std::max(g, 0.0f);
    b = std::max(b, 0.0f);

    // Gamma/power curve. Positive = brighter mids, negative = darker mids.
    const float gammaControl = std::clamp(p.gamma, -1.0f, 1.0f);
    const float gammaPow = std::pow(2.0f, -gammaControl);
    r = std::pow(r, gammaPow);
    g = std::pow(g, gammaPow);
    b = std::pow(b, gammaPow);

    // Temperature/tint approximation in RGB. Positive = warm, negative = cool.
    const float temp = std::clamp(p.temperature, -1.0f, 1.0f);
    if (temp >= 0.0f)
    {
        r *= 1.0f + temp * 0.22f;
        g *= 1.0f + temp * 0.06f;
        b *= 1.0f - temp * 0.22f;
    }
    else
    {
        const float cool = -temp;
        r *= 1.0f - cool * 0.22f;
        g *= 1.0f - cool * 0.04f;
        b *= 1.0f + cool * 0.22f;
    }

    // Saturation via luminance lerp.
    const float sat = std::clamp(p.saturation, -1.0f, 1.0f);
    const float satFactor = 1.0f + sat;
    const float luma = CustomLutLuma(r, g, b);
    r = luma + (r - luma) * satFactor;
    g = luma + (g - luma) * satFactor;
    b = luma + (b - luma) * satFactor;

    out[0] = CustomLutClamp01(r);
    out[1] = CustomLutClamp01(g);
    out[2] = CustomLutClamp01(b);
}

inline Lut3D BuildCustomLut(const CustomLutParams& params, int size = 33)
{
    Lut3D lut;
    lut.size = std::clamp(size, 17, kMaxLutSize);
    lut.domainMin[0] = lut.domainMin[1] = lut.domainMin[2] = 0.0f;
    lut.domainMax[0] = lut.domainMax[1] = lut.domainMax[2] = 1.0f;
    lut.rgb.resize((size_t) lut.size * lut.size * lut.size * 3);

    const float inv = 1.0f / float(lut.size - 1);
    for (int bIdx = 0; bIdx < lut.size; ++bIdx)
    {
        const float b = bIdx * inv;
        for (int gIdx = 0; gIdx < lut.size; ++gIdx)
        {
            const float g = gIdx * inv;
            for (int rIdx = 0; rIdx < lut.size; ++rIdx)
            {
                const float r = rIdx * inv;
                float out[3];
                CustomLutApply(params, r, g, b, out);
                const size_t i = (((size_t) bIdx * lut.size + gIdx) * lut.size + rIdx) * 3;
                lut.rgb[i + 0] = out[0];
                lut.rgb[i + 1] = out[1];
                lut.rgb[i + 2] = out[2];
            }
        }
    }

    return lut;
}

inline std::string CustomLutKey(const CustomLutParams& params)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf), "<custom>:e=%.4f,b=%.4f,g=%.4f,bl=%.4f,wh=%.4f,c=%.4f,t=%.4f,s=%.4f", params.exposure,
                  params.brightness, params.gamma, params.blacks, params.whites, params.contrast, params.temperature,
                  params.saturation);
    return std::string(buf);
}

inline bool IsNeutralCustomLut(const CustomLutParams& params)
{
    auto z = [](float v) { return std::abs(v) <= 1e-4f; };
    return z(params.exposure) && z(params.brightness) && z(params.gamma) && z(params.blacks) && z(params.whites) &&
           z(params.contrast) && z(params.temperature) && z(params.saturation);
}

} // namespace DlssNr

inline bool DlssNr_LutEnsureCustom(DlssNr_LutState* state, const DlssNr::CustomLutParams& params)
{
    if (state == nullptr)
        return false;

    const std::string key = DlssNr::CustomLutKey(params);
    if (key == state->attemptedPath && !state->failed)
        return state->Loaded();

    state->attemptedPath = key;
    state->lut = DlssNr::BuildCustomLut(params, 33);
    state->loadedPath = key;
    state->failed = false;
    state->error.clear();
    return true;
}
