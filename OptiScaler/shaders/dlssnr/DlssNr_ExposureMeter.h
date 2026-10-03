#pragma once

// Automatic exposure's percentile meter: the scene brightness is the log-average of the tiles between a low and a high
// percentile of brightness, instead of the plain average of every tile with a highlight knee over it.
//
// Why: Unreal's default auto exposure builds a log-luminance histogram and averages between two percentiles (10 and 90
// since 4.25), Unity HDRP's Automatic Histogram does the same with its histogram percentages, and FSR 3.1's own helper
// takes a log average. A log statistic is not pulled up by a lamp or the sky the way a linear mean is, and cutting the
// brightest and darkest tails leaves out what should not decide the exposure, so no hand-tuned knee is needed.
//
// It works on the meter's 64x64 grid of tile means (a tile is the plain mean of its pixels), the same input the other
// meter has, and every tile counts the same (the tiles differ by at most one pixel row). The grid and the black-tile rule
// are shared with the other meter; only the statistic differs. The GPU does this in dlssnr.hlsl (gMode 13, the
// percentile branch), line for line: a 64-bin histogram over -24..+24 EV with an integer count and a fixed-point sum of
// the log per bin, then a walk over the bins that takes a bin's share of the window by its count. This is the reference
// the host test (tests/nr_exposure_meter_smoke.cpp) holds the shader's arithmetic to.
//
// Header-only and free of D3D types so it can be exercised on the host.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace DlssNrExposureMeter
{
constexpr uint32_t kAverage = 0;    // the plain mean with the highlight knee ("Ignore bright highlights")
constexpr uint32_t kPercentile = 1; // the log-average between two percentiles
constexpr uint32_t kBins = 64;
constexpr float kMinEv = -24.0f;
constexpr float kMaxEv = 24.0f;
constexpr float kFixedPointPerEv = 1024.0f;
constexpr float kDefaultLowPercent = 10.0f;  // Unreal's default
constexpr float kDefaultHighPercent = 90.0f; // Unreal's default
constexpr float kMinMeteredShare = 0.05f;    // under this share of the tiles left, there is no reading

// The settings as used: a window that is empty or backwards falls back to the whole range, a broken value to the default.
struct Window
{
    float low;
    float high;
};

inline Window Clamp(float low, float high)
{
    low = std::isfinite(low) ? std::clamp(low, 0.0f, 100.0f) : kDefaultLowPercent;
    high = std::isfinite(high) ? std::clamp(high, 0.0f, 100.0f) : kDefaultHighPercent;
    return high > low ? Window { low, high } : Window { 0.0f, 100.0f };
}

// The scene luminance the meter reports for `tiles` (tile means in the frame's own units; `preExposure` is divided out),
// or 0 when under kMinMeteredShare of `gridTiles` is left after the black tiles (<= blackLevel, in the tiles' units) are
// dropped: 0 is "no reading", as in the shader.
inline float PercentileSceneLuma(const std::vector<float>& tiles, float preExposure, float blackLevel, float lowPercent,
                                 float highPercent, uint32_t gridTiles = 4096)
{
    const Window window = Clamp(lowPercent, highPercent);
    uint32_t count[kBins] = {};
    uint32_t sum[kBins] = {};
    uint32_t total = 0;

    for (float tile : tiles)
    {
        tile = std::isfinite(tile) ? std::max(tile, 0.0f) : 0.0f;

        if (tile <= blackLevel)
            continue;

        const float scene = std::max(tile / preExposure, 1e-8f);
        const float ev = std::clamp(std::log2(scene), kMinEv, kMaxEv);
        const uint32_t bin = std::min((uint32_t) ((ev - kMinEv) * ((float) kBins / (kMaxEv - kMinEv))), kBins - 1u);
        ++count[bin];
        sum[bin] += (uint32_t) ((ev - kMinEv) * kFixedPointPerEv + 0.5f);
        ++total;
    }

    if (total == 0 || (float) total < kMinMeteredShare * (float) gridTiles)
        return 0.0f;

    float lo = (float) total * window.low * 0.01f;
    float hi = (float) total * window.high * 0.01f;

    if (hi - lo < 1.0f)
    {
        lo = 0.0f;
        hi = (float) total;
    }

    float cumulative = 0.0f;
    float weight = 0.0f;
    float evSum = 0.0f;

    for (uint32_t b = 0; b < kBins; ++b)
    {
        const float c = (float) count[b];
        const float a = std::max(cumulative, lo);
        const float e = std::min(cumulative + c, hi);

        if (c > 0.0f && e > a)
        {
            const float binMeanEv = (float) sum[b] / (kFixedPointPerEv * c) + kMinEv;
            evSum += binMeanEv * (e - a);
            weight += e - a;
        }

        cumulative += c;
    }

    return weight > 0.0f ? std::exp2(evSum / weight) : 0.0f;
}
} // namespace DlssNrExposureMeter
