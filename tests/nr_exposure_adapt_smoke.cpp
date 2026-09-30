// Host check of DlssNr_ExposureAdapt.h: Automatic exposure's eye adaptation. No GPU and no game needed.
// cl /std:c++20 /EHsc /W4 tests/nr_exposure_adapt_smoke.cpp
#include "../OptiScaler/shaders/dlssnr/DlssNr_ExposureAdapt.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

using namespace DlssNrExposureAdapt;

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

static bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
static float Ev(float a, float b) { return std::log2(a / b); }

static const float kNan = std::numeric_limits<float>::quiet_NaN();
static const float kInf = std::numeric_limits<float>::infinity();

// The D3D12 path in miniature: every evaluation asks the adapter for this frame's blend, and the shader eases the texture
// it wrote last frame toward the new reading with it.
struct Sim
{
    Adapter adapter;
    float eased = 0.0f;
    uint64_t evaluation = 0;
    double seconds = 0.0;
    float tau = kDefaultSeconds;

    // `frames` evaluations of `reading` at `fps`. Returns the largest distance, in EV, of the eased value from `from`.
    float Run(float reading, int frames, float fps, float from = 0.0f, bool reset = false)
    {
        float farthest = 0.0f;

        for (int i = 0; i < frames; ++i)
        {
            const Step step = adapter.Next(++evaluation, seconds, tau, reset && i == 0);
            eased = Ease(eased, reading, step.blend, step.snap);
            seconds += 1.0 / fps;

            if (from > 0.0f)
                farthest = std::max(farthest, std::fabs(Ev(eased, from)));
        }

        return farthest;
    }
};

int main()
{
    // Blend: the share of the way to the new reading one evaluation takes, 1 - exp(-dt / tau).
    CHECK(Near(Blend(1.0f, 1.0f), 1.0f - std::exp(-1.0f)));
    CHECK(Near(Blend(0.5f, 1.0f), 1.0f - std::exp(-0.5f)));
    CHECK(Blend(0.0f, 1.0f) == 0.0f);
    CHECK(Blend(-0.1f, 1.0f) == 0.0f); // a clock that went backwards moves nothing
    CHECK(Blend(kNan, 1.0f) == 0.0f);
    CHECK(Blend(0.016f, 0.0f) == 1.0f); // off: the reading as it is, the behaviour before eye adaptation
    CHECK(Blend(0.016f, -1.0f) == 1.0f);
    CHECK(Blend(0.016f, kNan) == 1.0f);
    CHECK(Blend(1e6f, 1.0f) == 1.0f); // a long gap arrives
    CHECK(Blend(0.016f, 1.0f) > 0.0f && Blend(0.016f, 1.0f) < 0.02f);

    // Ease: in log space, so a step up and the same step down take the same time.
    CHECK(Near(Ease(1.0f, 4.0f, 0.5f, false), 2.0f));
    CHECK(Near(Ease(4.0f, 1.0f, 0.5f, false), 2.0f));
    CHECK(Near(Ease(1.0f, 4.0f, 0.0f, false), 1.0f));
    CHECK(Near(Ease(1.0f, 4.0f, 1.0f, false), 4.0f));
    CHECK(Near(Ease(1.0f, 4.0f, 2.0f, false), 4.0f));  // clamped, never overshoots
    CHECK(Near(Ease(1.0f, 4.0f, -1.0f, false), 1.0f)); // clamped
    CHECK(Near(Ease(1.0f, 4.0f, kNan, false), 1.0f));  // a broken blend moves nothing
    CHECK(Near(Ease(1.0f, 4.0f, 0.1f, true), 4.0f));   // a snap takes the reading
    CHECK(Near(Ease(0.0f, 4.0f, 0.1f, false), 4.0f));  // nothing to ease from: the reading
    CHECK(Near(Ease(kNan, 4.0f, 0.1f, false), 4.0f));
    CHECK(Near(Ease(kInf, 4.0f, 0.1f, false), 4.0f));
    // 0 is "no reading" (a black frame, a fade): the last value is kept -- but not across a snap: a cut through a black
    // frame (or a source switch on a menu frame) leaves no reading, so the next real one snaps instead of easing from
    // the old scene.
    CHECK(Near(Ease(2.0f, 0.0f, 0.5f, false), 2.0f));
    CHECK(Ease(2.0f, 0.0f, 0.5f, true) == 0.0f);
    CHECK(Near(Ease(Ease(2.0f, 0.0f, 0.5f, true), 4.0f, 0.01f, false), 4.0f));
    CHECK(Near(Ease(2.0f, kNan, 0.5f, false), 2.0f));
    CHECK(Near(Ease(2.0f, -1.0f, 0.5f, false), 2.0f));
    CHECK(Near(Ease(2.0f, 1e9f, 0.5f, false), 2.0f)); // absurd, as the shader's WhitePoint() judges it
    CHECK(Ease(0.0f, 0.0f, 0.5f, true) == 0.0f);      // nothing at all: still "no reading"
    CHECK(Ease(kNan, kNan, 0.5f, false) == 0.0f);
    CHECK(Valid(1.0f) && !Valid(0.0f) && !Valid(kNan) && !Valid(1e9f) && !Valid(-1.0f));

    // The setting: seconds, 0 = off, clamped to the menu's range, a broken value -> the default.
    CHECK(Near(Seconds(1.0f), 1.0f));
    CHECK(Seconds(-1.0f) == 0.0f);
    CHECK(Near(Seconds(99.0f), kMaxSeconds));
    CHECK(Near(Seconds(kNan), kDefaultSeconds));

    // Adapter: the first evaluation, a cut the game signals, a gap in the evaluations, and Invalidate all snap.
    {
        Adapter a;
        Step s = a.Next(10, 0.0, 1.0f, false);
        CHECK(s.snap && s.blend == 1.0f);
        s = a.Next(11, 0.5, 1.0f, false);
        CHECK(!s.snap && Near(s.blend, 1.0f - std::exp(-0.5f)));
        s = a.Next(12, 0.6, 1.0f, true);
        CHECK(s.snap);
        s = a.Next(13, 0.7, 1.0f, false);
        CHECK(!s.snap);
        s = a.Next(15, 0.8, 1.0f, false); // evaluation 14 went elsewhere (another source, NR off): stale
        CHECK(s.snap);
        s = a.Next(16, 0.9, 1.0f, false);
        CHECK(!s.snap);
        s = a.Next(16, 1.0, 1.0f, false); // the same evaluation twice is not a continuation either
        CHECK(s.snap);
        a.Invalidate();
        s = a.Next(17, 1.1, 1.0f, false);
        CHECK(s.snap);
        s = a.Next(18, 1.2, 0.0f, false); // off: continuous, but the whole way
        CHECK(!s.snap && s.blend == 1.0f);
    }

    // Frame-rate independent: one second after a 1.19 EV step, 30, 60 and 120 fps sit at the same place, 63% of the way.
    {
        float at[3] = {};
        const float fps[3] = { 30.0f, 60.0f, 120.0f };

        for (int i = 0; i < 3; ++i)
        {
            Sim sim;
            sim.Run(1.45f, 10, fps[i]);
            sim.Run(3.30f, (int) fps[i], fps[i]);
            at[i] = Ev(sim.eased, 1.45f);
        }

        const float step = Ev(3.30f, 1.45f);
        CHECK(std::fabs(at[0] - at[2]) < 0.01f && std::fabs(at[1] - at[2]) < 0.01f);
        CHECK(std::fabs(at[2] / step - (1.0f - std::exp(-1.0f))) < 0.02f);
    }

    // NBA 2K27, a made basket (frame stats, 2026-09-27): Automatic's reading went 1.45 -> 3.30 -> 1.46, a 1.19 EV swing,
    // for a few frames of the zoom. Eased, the picture moves less than 0.5 EV and settles back.
    {
        Sim sim;
        sim.Run(1.45f, 180, 60.0f);
        const float during = sim.Run(3.30f, 18, 60.0f, 1.45f); // 0.3 s
        const float after = sim.Run(1.46f, 180, 60.0f, 1.45f);
        CHECK(Ev(3.30f, 1.45f) > 1.15f);
        CHECK(during < 0.5f);
        CHECK(after < 0.5f);
        CHECK(std::fabs(Ev(sim.eased, 1.46f)) < 0.05f);
    }

    // The same swing without eye adaptation (0 s): the full 1.19 EV at once, as before.
    {
        Sim sim;
        sim.tau = 0.0f;
        sim.Run(1.45f, 60, 60.0f);
        CHECK(sim.Run(3.30f, 1, 60.0f, 1.45f) > 1.15f);
    }

    // A real change of scene that lasts is followed: within 0.05 EV after 4 s.
    {
        Sim sim;
        sim.Run(1.45f, 60, 60.0f);
        sim.Run(3.30f, 240, 60.0f);
        CHECK(std::fabs(Ev(sim.eased, 3.30f)) < 0.05f);
    }

    // A cut the game signals snaps straight to the new scene.
    {
        Sim sim;
        sim.Run(1.45f, 60, 60.0f);
        sim.Run(3.30f, 1, 60.0f, 0.0f, true);
        CHECK(Near(sim.eased, 3.30f));
    }

    // A cut through a black frame: the game's reset lands on the black frame, and the new scene after it still snaps.
    {
        Sim sim;
        sim.Run(1.45f, 60, 60.0f);
        sim.Run(0.0f, 1, 60.0f, 0.0f, true);
        sim.Run(3.30f, 1, 60.0f);
        CHECK(Near(sim.eased, 3.30f));
    }

    // A fade to black (no reading) holds the last exposure, and the picture comes back from there, eased.
    {
        Sim sim;
        sim.Run(1.45f, 60, 60.0f);
        sim.Run(0.0f, 30, 60.0f);
        CHECK(Near(sim.eased, 1.45f));
        sim.Run(3.30f, 1, 60.0f);
        CHECK(Ev(sim.eased, 1.45f) < 0.05f);
    }

    if (fails == 0)
        printf("all passed\n");
    return fails == 0 ? 0 : 1;
}
