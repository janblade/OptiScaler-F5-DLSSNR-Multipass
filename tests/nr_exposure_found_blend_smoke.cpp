// Host check of shaders/dlssnr/DlssNr_ExposureFoundBlend.h (the white point moving between Automatic's own and Auto's
// found exposure, both ways). No GPU and no game needed.
// cl /std:c++20 /EHsc /W4 tests/nr_exposure_found_blend_smoke.cpp
#include "../OptiScaler/shaders/dlssnr/DlssNr_ExposureFoundBlend.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>

using namespace DlssNrExposureFoundBlend;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

constexpr double kFrame = 1.0 / 60.0;

// What the picture shows each frame: the host's base, or Automatic's own when the host gives none
static float Shown(float base, float own) { return base > 0.0f ? base : own; }

// Runs `frames` frames from `t`, with target/own per frame; returns the largest step the picture takes, in stops
static double Run(Blend& b, double& t, int frames, const std::function<float(int)>& target,
                  const std::function<float(int)>& own, float& last)
{
    double worst = 0.0;

    for (int i = 0; i < frames; ++i, t += kFrame)
    {
        const float o = own(i);
        const float now = Shown(b.Step(target(i), o, t), o);

        if (last > 0.0f && now > 0.0f)
            worst = std::max(worst, (double) std::abs(std::log2(now / last)));

        if (now > 0.0f)
            last = now;
    }

    return worst;
}

int main()
{
    // 0.84 EV apart, as in Cyberpunk 2077: no frame moves by more than the ease allows (0.84 EV over 60 frames, plus a
    // little for rounding)
    const float own = 0.4f;
    const float found = own * std::exp2(-0.84f);
    const double bound = 0.84 / 60.0 * 1.5;

    // Takeover eased; then the found value goes away (a loading screen): leaving is eased too
    {
        Blend b;
        double t = 0.0;
        float last = 0.0f;
        CHECK(Run(b, t, 30, [](int) { return 0.0f; }, [&](int) { return own; }, last) == 0.0);
        CHECK(Run(b, t, 90, [&](int) { return found; }, [&](int) { return own; }, last) <= bound);
        CHECK(std::abs(b.Shown() - found) < 1e-4f);
        CHECK(Run(b, t, 90, [](int) { return 0.0f; }, [&](int) { return own; }, last) <= bound);
        CHECK(b.Shown() == 0.0f); // Automatic's own (the shader's live value) in force again
        CHECK(b.LastEvent() == Event::None);
    }

    // The events, in order
    {
        Blend b;
        b.Step(found, own, 0.0);
        CHECK(b.LastEvent() == Event::TookOver);
        b.Step(found, own, 0.5);
        CHECK(b.LastEvent() == Event::None);
        b.Step(0.0f, own, 2.0);
        CHECK(b.LastEvent() == Event::Leaving);
        CHECK(b.Step(0.0f, own, 3.5) == 0.0f);
        CHECK(b.LastEvent() == Event::Left);
    }

    // Takeover while Automatic has no reading (its meter starts over): waits, never steps from nothing
    {
        Blend b;
        CHECK(b.Step(found, 0.0f, 0.0) == 0.0f);
        CHECK(b.Step(found, 0.0f, 0.1) == 0.0f);
        CHECK(b.Step(found, own, 0.2) == own); // from Automatic's own, the first frame of the ease
        CHECK(b.LastEvent() == Event::TookOver);
    }

    // Found in force, Automatic's reading starts over (a change of source): the white point holds, not the slider;
    // then the found value goes too, and leaving waits for Automatic's reading
    {
        Blend b;
        double t = 0.0;
        float last = 0.0f;
        Run(b, t, 90, [&](int) { return found; }, [&](int) { return own; }, last);
        CHECK(std::abs(b.Step(found, 0.0f, t) - found) < 1e-4f);
        t += kFrame;
        CHECK(std::abs(b.Step(0.0f, 0.0f, t) - found) < 1e-4f); // leaving, nothing to ease to yet: holds
        CHECK(Run(b, t, 10, [](int) { return 0.0f; }, [](int) { return 0.0f; }, last) == 0.0);
        CHECK(Run(b, t, 90, [](int) { return 0.0f; }, [&](int) { return own; }, last) <= bound);
        CHECK(b.Shown() == 0.0f);
    }

    // Leaving with no reading of Automatic's own for longer than the hold: gives up holding
    {
        Blend b;
        b.Step(found, own, 0.0);
        b.Step(found, own, 1.5);
        CHECK(b.Step(0.0f, 0.0f, 2.0) > 0.0f);
        CHECK(b.Step(0.0f, 0.0f, 2.0 + kHoldSeconds - 0.1) > 0.0f);
        CHECK(b.Step(0.0f, 0.0f, 2.0 + kHoldSeconds + 0.1) == 0.0f);
        CHECK(b.LastEvent() == Event::Left);
    }

    // Found again halfway through leaving: eased back from where the picture is, no jump
    {
        Blend b;
        double t = 0.0;
        float last = 0.0f;
        Run(b, t, 90, [&](int) { return found; }, [&](int) { return own; }, last);
        CHECK(Run(b, t, 30, [](int) { return 0.0f; }, [&](int) { return own; }, last) <= bound);
        CHECK(Run(b, t, 90, [&](int) { return found; }, [&](int) { return own; }, last) <= bound);
        CHECK(std::abs(b.Shown() - found) < 1e-4f);
    }

    // One bad sample (0, NaN, inf) in the middle: no visible step
    for (const float bad : { 0.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
    {
        Blend b;
        double t = 0.0;
        float last = 0.0f;
        Run(b, t, 90, [&](int) { return found; }, [&](int) { return own; }, last);
        CHECK(Run(b, t, 30, [&](int i) { return i == 10 ? bad : found; }, [&](int) { return own; }, last) <= bound);
    }

    // The found value moving (the game adapting) passes straight through once the takeover is done
    {
        Blend b;
        double t = 0.0;
        float last = 0.0f;
        Run(b, t, 90, [&](int) { return found; }, [&](int) { return own; }, last);
        CHECK(std::abs(b.Step(found * 2.0f, own, t) - found * 2.0f) < 1e-4f);
    }

    printf(fails == 0 ? "PASS\n" : "%d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
