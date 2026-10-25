#pragma once
#include <algorithm>
#include <cmath>

namespace DlssNr::Profiles
{
// Move a calibrated band gain toward neutral 1 without extrapolating past strength 1.
inline float StrengthAdjustedGain(float gain, float strength)
{
    gain = std::isfinite(gain) ? gain : 1.0f;
    const float factor = std::isfinite(strength) ? std::clamp(strength, 0.0f, 1.0f) : 1.0f;
    if (factor == 0.0f)
        return 1.0f;
    if (factor == 1.0f)
        return gain;
    return 1.0f + factor * (gain - 1.0f);
}
// Historical Structure-only high/mid calibration, retained by the comparison fixtures.
inline float StructureAdjustedHigh(float gain, float structure) { return StrengthAdjustedGain(gain, structure); }
// Apply Structure and Intensity sequentially to the deviation from neutral gain 1.
// Clamp each slider independently to its calibrated range before multiplying.
inline float ModelGainFactor(float structure, float intensity)
{
    const float s = std::isfinite(structure) ? std::clamp(structure, 0.0f, 1.0f) : 1.0f;
    const float i = std::isfinite(intensity) ? std::clamp(intensity, 0.0f, 1.0f) : 1.0f;
    return s * i;
}
inline float ModelAdjustedGain(float gain, float structure, float intensity)
{
    return StrengthAdjustedGain(gain, ModelGainFactor(structure, intensity));
}
// Half-open model range [first, completed). A callback avoids a per-frame allocation.
template <class GainForPass>
inline float ProductGuidedHigh(unsigned first, unsigned completed, bool enabled, GainForPass gainForPass)
{
    if (completed <= first)
        return 1.0f;
    double product = 1.0;
    if (!enabled)
        product = gainForPass(completed - 1u);
    else
        for (unsigned pass = first; pass < completed; ++pass)
            product *= gainForPass(pass);
    const float result = static_cast<float>(product);
    return std::isfinite(result) ? result : 1.0f;
}
// Arithmetic mean of the effective per-model low gains; never a product.
template <class GainForPass> inline float MeanGuidedLow(unsigned first, unsigned completed, GainForPass gainForPass)
{
    if (completed <= first)
        return 1.0f;
    double sum = 0.0;
    for (unsigned pass = first; pass < completed; ++pass)
        sum += gainForPass(pass);
    const float result = static_cast<float>(sum / (completed - first));
    return std::isfinite(result) ? result : 1.0f;
}
} // namespace DlssNr::Profiles
