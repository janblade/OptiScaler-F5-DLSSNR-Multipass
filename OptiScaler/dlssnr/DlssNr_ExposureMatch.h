#pragma once

// Which exposure scan candidate is the game's exposure, in what units, or whether the exposure is already in the
// picture (plan 2026-10-14-exposure-scan-all-apis, story 2).
//
// The scan (DlssNr_ExposureScan.h) finds small float textures and buffers and records each one's value next to our
// Automatic meter's reading of the same frame (DlssNr_ExposureScanRecord.h). This reads those histories and says which
// candidate follows the scene's light the way an exposure does. It decides nothing about the picture itself: what to
// do with a match is the auto source ladder's business (story 5).
//
// The yardstick is our meter. Its raw reading is an exposure multiplier (0.18 / (scene luminance * 0.82), dlssnr.hlsl):
// bigger = a darker scene. Against it, on log scales:
// - slope ~ +1: the candidate is a multiplier as ours is (Cyberpunk 2077: corr 0.986, the same size as ours);
// - slope ~ -1: a divisor, or a scene luminance (white point style);
// - a candidate that follows log2 of the meter linearly instead: a stop value (log2, ln or log10, either sign).
// The fit is tried at a range of row lags, because a game's eye adaptation trails the meter's raw (unadapted) reading.
//
// What is not an exposure, rejected before any fit:
// - Steady: changes in under 2% of rows. NBA 2K27's own handed exposure is this (one value per scene, 1.3195 for the
//   whole game while our meter swings ~5 EV): a real exposure can be steady, but a steady value cannot be matched;
// - NotFloat: integer bits read as float (1e-37 and the like);
// - Timer: rises (or falls) on almost every changing row;
// - Noisy: jumps across most of its range from row to row (a random seed, a dither index).
//
// Confidence: no verdict before minRows rows with a reading and minTravelEv of meter travel (a scene that never changes
// its light says nothing about units), and the best fit must hold in each half of the recording that has light to show.
// A meter that barely moves while a candidate swings is the other answer: the game exposed the colour itself (Baked).
//
// Header-only, no D3D types: tests/nr_exposure_match_smoke.cpp, on synthetic cases and on the recordings in
// tests/data/exposure_scan.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace DlssNrExposureMatch
{
enum class Kind
{
    Unsure,     // too few rows with a reading to say
    Steady,     // changes in under 2% of rows
    NotFloat,   // integer bits, not a float value
    Timer,      // counts up or down
    Noisy,      // jumps across its range row to row
    Unrelated,  // moves smoothly, but not with the light
    Multiplier, // an exposure as ours: slope ~ +1 in log-log
    Divisor,    // slope ~ -1 in log-log
    Stops       // linear in log2 of the meter: a stop value; `scale` = its stops per our log2 stop
};

struct Fit
{
    Kind kind = Kind::Unsure;
    float slope = 0.0f; // log-log for Multiplier/Divisor, value per log2 stop for Stops
    float corr = 0.0f;  // signed
    int lag = 0;        // rows the candidate trails the meter by
    float scale = 0.0f; // Stops only: +-1 (log2), +-ln 2 (ln), +-log10 2 (log10)
};

enum class What
{
    NotSure, // not enough rows or brightness variety yet
    Found,   // `index` is the exposure, `fit` its units
    Baked,   // the colour is already exposed: our meter is flat while `index` swings
    None     // enough variety, nothing follows the light
};

struct Verdict
{
    What what = What::NotSure;
    int index = -1;
    Fit fit;
    size_t rows = 0;            // rows with a meter reading
    float meterTravelEv = 0.0f; // 5th..95th percentile of the meter, in stops
};

struct Settings
{
    size_t minRows = 200;          // ~20 s at the recording's ~10 rows a second
    float minTravelEv = 2.0f;      // meter travel before units can be told
    float halfTravelEv = 1.0f;     // a half with less than this is not asked to agree
    float minCorr = 0.9f;          // whole recording
    float halfCorr = 0.8f;         // each half
    float bakedMeterEv = 0.5f;     // meter at most this flat ...
    float bakedCandidateEv = 2.0f; // ... while a candidate swings at least this much
    int minLag = -5;               // rows; a little lead is allowed, the copy may be read a frame late
    int maxLag = 30;               // ~3 s of eye adaptation
};

namespace detail
{
inline float Percentile(std::vector<float> v, float p)
{
    if (v.empty())
        return 0.0f;
    const size_t i = std::min(v.size() - 1, (size_t) (p * (float) (v.size() - 1) + 0.5f));
    std::nth_element(v.begin(), v.begin() + i, v.end());
    return v[i];
}

inline float SpreadEv(const std::vector<float>& logs) { return Percentile(logs, 0.95f) - Percentile(logs, 0.05f); }

// Least squares of y on x with y shifted `lag` rows later; slope and correlation, 0 when either side is flat.
inline void Regress(const std::vector<float>& x, const std::vector<float>& y, int lag, float& slope, float& corr)
{
    slope = 0.0f;
    corr = 0.0f;
    const int n = (int) x.size();
    const int from = std::max(0, lag), to = std::min(n, n + lag);
    if (to - from < 3)
        return;

    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    for (int i = from; i < to; ++i)
    {
        const double a = x[i - lag], b = y[i];
        sx += a;
        sy += b;
        sxx += a * a;
        syy += b * b;
        sxy += a * b;
    }
    const double m = to - from;
    const double vx = sxx - sx * sx / m, vy = syy - sy * sy / m, cxy = sxy - sx * sy / m;
    if (vx <= 1e-12 || vy <= 1e-12)
        return;
    slope = (float) (cxy / vx);
    corr = (float) (cxy / std::sqrt(vx * vy));
}

inline void BestLag(const std::vector<float>& x, const std::vector<float>& y, const Settings& s, float& slope,
                    float& corr, int& lag)
{
    slope = corr = 0.0f;
    lag = 0;
    for (int l = s.minLag; l <= s.maxLag; ++l)
    {
        float sl, r;
        Regress(x, y, l, sl, r);
        if (std::fabs(r) > std::fabs(corr) + 1e-6f ||
            (std::fabs(r) > std::fabs(corr) - 1e-6f && std::abs(l) < std::abs(lag)))
        {
            slope = sl;
            corr = r;
            lag = l;
        }
    }
}

// The stop value's scale: its slope against log2 of the meter, snapped to log2 / ln / log10 when within 25%
inline float SnapScale(float slope)
{
    const float sign = slope < 0.0f ? -1.0f : 1.0f;
    const float a = std::fabs(slope);
    if (a <= 0.0f)
        return 0.0f;
    float best = 0.0f, bestDist = 1e9f;
    for (float c : { 1.0f, 0.69314718f, 0.30103f })
    {
        const float d = std::fabs(std::log(a / c));
        if (d < bestDist)
        {
            bestDist = d;
            best = c;
        }
    }
    return bestDist <= std::log(1.25f) ? sign * best : 0.0f;
}

// Classify one candidate on rows that all carry a meter reading: logMeter = log2 of the reading
inline Fit ClassifyRows(const std::vector<float>& logMeter, const std::vector<float>& v, const Settings& s)
{
    Fit f;
    const size_t n = v.size();
    if (n < 20)
        return f;

    size_t nonzero = 0, broken = 0, positive = 0;
    for (float a : v)
    {
        if (a != 0.0f)
            ++nonzero;
        if (!std::isfinite(a) || (a != 0.0f && (std::fabs(a) < 1e-30f || std::fabs(a) > 1e30f)))
            ++broken;
        if (a > 0.0f)
            ++positive;
    }
    if (broken * 10 > nonzero && broken > 0)
    {
        f.kind = Kind::NotFloat;
        return f;
    }

    size_t changes = 0, ups = 0, downs = 0;
    std::vector<float> steps;
    steps.reserve(n);
    for (size_t i = 1; i < n; ++i)
    {
        if (v[i] != v[i - 1])
        {
            ++changes;
            (v[i] > v[i - 1] ? ups : downs) += 1;
        }
        steps.push_back(std::fabs(v[i] - v[i - 1]));
    }
    if (changes * 50 < n - 1)
    {
        f.kind = Kind::Steady;
        return f;
    }
    if (changes * 2 >= n - 1 && (ups * 50 >= changes * 49 || downs * 50 >= changes * 49))
    {
        f.kind = Kind::Timer;
        return f;
    }
    const float range = Percentile(v, 0.95f) - Percentile(v, 0.05f);
    if (range <= 0.0f)
    {
        f.kind = Kind::Steady;
        return f;
    }
    if (Percentile(steps, 0.5f) > 0.2f * range)
    {
        f.kind = Kind::Noisy;
        return f;
    }

    // Log-log, when the value is a positive quantity (a few unread zeros allowed)
    float logSlope = 0, logCorr = 0;
    int logLag = 0;
    const bool canLog = positive * 20 >= n * 19;
    if (canLog)
    {
        std::vector<float> x, y;
        x.reserve(n);
        y.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            // an unread 0 repeats the value before it, so the rows stay aligned for the lag
            if (v[i] <= 0.0f && y.empty())
                continue;
            x.push_back(logMeter[i]);
            y.push_back(v[i] > 0.0f ? std::log2(v[i]) : y.back());
        }
        BestLag(x, y, s, logSlope, logCorr, logLag);
    }

    // Linear in log2 of the meter: a stop value
    float linSlope = 0, linCorr = 0;
    int linLag = 0;
    BestLag(logMeter, v, s, linSlope, linCorr, linLag);

    const bool stops = !canLog || std::fabs(linCorr) > std::fabs(logCorr) + 0.01f;
    if (stops)
    {
        f.slope = linSlope;
        f.corr = linCorr;
        f.lag = linLag;
        const float scale = SnapScale(linSlope);
        f.kind = std::fabs(linCorr) >= s.minCorr && scale != 0.0f ? Kind::Stops : Kind::Unrelated;
        if (f.kind == Kind::Stops)
            f.scale = scale;
        return f;
    }

    f.slope = logSlope;
    f.corr = logCorr;
    f.lag = logLag;
    if (std::fabs(logCorr) >= s.minCorr && std::fabs(logSlope) >= 0.5f && std::fabs(logSlope) <= 1.5f)
        f.kind = logSlope > 0.0f ? Kind::Multiplier : Kind::Divisor;
    else
        f.kind = Kind::Unrelated;
    return f;
}

inline bool IsMatch(Kind k) { return k == Kind::Multiplier || k == Kind::Divisor || k == Kind::Stops; }
} // namespace detail

// One candidate against the meter, row by row (the recording's columns). Rows without a meter reading (0) are left out.
inline Fit Classify(const std::vector<float>& meter, const std::vector<float>& value, const Settings& s = {})
{
    std::vector<float> lm, v;
    for (size_t i = 0; i < meter.size() && i < value.size(); ++i)
        if (meter[i] > 0.0f && std::isfinite(meter[i]))
        {
            lm.push_back(std::log2(meter[i]));
            v.push_back(value[i]);
        }
    return detail::ClassifyRows(lm, v, s);
}

inline Verdict Match(const std::vector<float>& meter, const std::vector<std::vector<float>>& candidates,
                     const Settings& s = {})
{
    using namespace detail;
    Verdict out;

    std::vector<size_t> rows;
    std::vector<float> lm;
    for (size_t i = 0; i < meter.size(); ++i)
        if (meter[i] > 0.0f && std::isfinite(meter[i]))
        {
            rows.push_back(i);
            lm.push_back(std::log2(meter[i]));
        }
    out.rows = rows.size();
    out.meterTravelEv = SpreadEv(lm);
    if (rows.size() < s.minRows)
        return out;

    auto column = [&](const std::vector<float>& c, size_t from, size_t to)
    {
        std::vector<float> v;
        v.reserve(to - from);
        for (size_t k = from; k < to; ++k)
            v.push_back(rows[k] < c.size() ? c[rows[k]] : 0.0f);
        return v;
    };

    // Flat meter: either the game already exposed the colour (a candidate swings), or nothing to go on
    if (out.meterTravelEv < s.bakedMeterEv)
    {
        float widest = 0.0f;
        for (size_t c = 0; c < candidates.size(); ++c)
        {
            const std::vector<float> v = column(candidates[c], 0, rows.size());
            const Kind k = ClassifyRows(lm, v, s).kind;
            if (k != Kind::Unrelated && !IsMatch(k))
                continue;
            std::vector<float> logs;
            for (float a : v)
                if (a > 0.0f)
                    logs.push_back(std::log2(a));
            if (logs.size() * 20 < v.size() * 19)
                continue;
            const float swing = SpreadEv(logs);
            if (swing >= s.bakedCandidateEv && swing > widest)
            {
                widest = swing;
                out.what = What::Baked;
                out.index = (int) c;
            }
        }
        return out;
    }
    if (out.meterTravelEv < s.minTravelEv)
        return out;

    const size_t half = rows.size() / 2;
    const std::vector<float> lm1(lm.begin(), lm.begin() + half), lm2(lm.begin() + half, lm.end());
    const bool ask1 = SpreadEv(lm1) >= s.halfTravelEv, ask2 = SpreadEv(lm2) >= s.halfTravelEv;
    Settings halfSettings = s;
    halfSettings.minCorr = s.halfCorr;

    out.what = What::None;
    for (size_t c = 0; c < candidates.size(); ++c)
    {
        const Fit f = ClassifyRows(lm, column(candidates[c], 0, rows.size()), s);
        if (!IsMatch(f.kind) || (out.index >= 0 && std::fabs(f.corr) <= std::fabs(out.fit.corr)))
            continue;
        if (ask1 && ClassifyRows(lm1, column(candidates[c], 0, half), halfSettings).kind != f.kind)
            continue;
        if (ask2 && ClassifyRows(lm2, column(candidates[c], half, rows.size()), halfSettings).kind != f.kind)
            continue;
        out.what = What::Found;
        out.index = (int) c;
        out.fit = f;
    }
    return out;
}

// A candidate's value as an exposure multiplier in our meter's sense, up to a constant factor; 0 when it has none.
inline float AsExposure(const Fit& f, float value)
{
    switch (f.kind)
    {
    case Kind::Multiplier:
        return value > 0.0f ? value : 0.0f;
    case Kind::Divisor:
        return value > 0.0f ? 1.0f / value : 0.0f;
    case Kind::Stops:
        return f.scale != 0.0f ? std::exp2(value / f.scale) : 0.0f;
    default:
        return 0.0f;
    }
}

// The constant factor: the median over rows of our meter's reading / the candidate as an exposure. Automatic gives the
// level, the candidate the movement (no lag, no HUD or bloom in it, no feedback from our own edit):
// exposure = AnchorScale * AsExposure(candidate now). 0 when no row has both.
inline float AnchorScale(const Fit& f, const std::vector<float>& meter, const std::vector<float>& value)
{
    std::vector<float> ratios;
    for (size_t i = 0; i < meter.size() && i < value.size(); ++i)
    {
        const float e = AsExposure(f, value[i]);
        if (meter[i] > 0.0f && std::isfinite(meter[i]) && e > 0.0f && std::isfinite(e))
            ratios.push_back(meter[i] / e);
    }
    return ratios.empty() ? 0.0f : detail::Percentile(ratios, 0.5f);
}
} // namespace DlssNrExposureMatch
