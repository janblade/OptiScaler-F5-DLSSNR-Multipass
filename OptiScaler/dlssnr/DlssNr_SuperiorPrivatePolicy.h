#pragma once
#include <algorithm>
#include <cmath>

namespace DlssNr::SuperiorPrivate
{
constexpr double FadeSeconds = .3;
constexpr double QuietSeconds = .05;
constexpr double FreshSeconds = .5;
constexpr unsigned MotionSamples = 576;
constexpr unsigned MovingSamples = MotionSamples * 3 / 4;

struct Fade
{
    float weight = 1.f;
    double previous = -1.;
    bool wasVisible = true;
    float Advance(bool visible, double now)
    {
        const bool starting = !visible && wasVisible && weight == 1.f;
        const double elapsed = previous >= 0. && !starting ? std::max(0., now - previous) : 0.;
        wasVisible = visible;
        previous = now;
        const auto step = static_cast<float>(elapsed / FadeSeconds);
        weight = std::clamp(weight + (visible ? step : -step), 0.f, 1.f);
        return weight < 1e-6f ? (weight = 0.f) : weight;
    }
};

struct CnnMotion
{
    double observed = -1., quietStart = -1.;
    bool quiet = false, idle = false;
    Fade fade;
    void Observe(unsigned moving, double time)
    {
        if (time <= observed)
            return;
        if (moving >= MovingSamples)
            quietStart = -1.;
        else if (!quiet || observed < 0. || time - observed > FreshSeconds)
            quietStart = time;
        observed = time;
        quiet = moving < MovingSamples;
    }
    void Invalidate()
    {
        quiet = false;
        quietStart = -1.;
    }
    bool Advance(double now)
    {
        const bool target = quiet && quietStart >= 0. && now >= observed && now - observed <= FreshSeconds &&
                            observed - quietStart >= QuietSeconds;
        if (fade.Advance(target == idle, now) != 0.f)
            return false;
        idle = target;
        fade = Fade { 1.f, now };
        return true;
    }
};
} // namespace DlssNr::SuperiorPrivate
