// Host check of DlssNr_ExposureMeter.h: Automatic exposure's percentile meter. No GPU and no game needed.
// cl /std:c++20 /EHsc /W4 tests/nr_exposure_meter_smoke.cpp
#include "../OptiScaler/shaders/dlssnr/DlssNr_ExposureMeter.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace DlssNrExposureMeter;

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

// The bins' fixed point and the bin walk are exact to about 1/1024 EV per tile.
static bool NearEv(float got, float want, float evTolerance = 0.01f)
{
    return got > 0.0f && std::fabs(std::log2(got / want)) <= evTolerance;
}

static std::vector<float> Flat(float value, size_t n = 4096) { return std::vector<float>(n, value); }

int main()
{
    // A flat frame: every window gives the one value. PreExposure is divided out.
    CHECK(NearEv(PercentileSceneLuma(Flat(0.5f), 1.0f, 0.0f, 10.0f, 90.0f), 0.5f));
    CHECK(NearEv(PercentileSceneLuma(Flat(0.5f), 1.0f, 0.0f, 0.0f, 100.0f), 0.5f));
    CHECK(NearEv(PercentileSceneLuma(Flat(8.0f), 4.0f, 0.0f, 10.0f, 90.0f), 2.0f));

    // A log-average, not an arithmetic one: half the tiles at 1 and half at 16 give 4, not 8.5.
    {
        std::vector<float> t = Flat(1.0f, 2048);
        t.resize(4096, 16.0f);
        CHECK(NearEv(PercentileSceneLuma(t, 1.0f, 0.0f, 0.0f, 100.0f), 4.0f));
    }

    // The tails are cut: 5% lamps at 1000 and 95% at 1; the 10..90 window sees only the 1s, the full window does not.
    {
        std::vector<float> t = Flat(1.0f, 3891);
        t.resize(4096, 1000.0f);
        CHECK(NearEv(PercentileSceneLuma(t, 1.0f, 0.0f, 10.0f, 90.0f), 1.0f));
        CHECK(PercentileSceneLuma(t, 1.0f, 0.0f, 0.0f, 100.0f) > 1.3f);
    }

    // The same with the dark tail: 5% near-black at 0.001 are left out by the low percentile.
    {
        std::vector<float> t = Flat(1.0f, 3891);
        t.resize(4096, 0.001f);
        CHECK(NearEv(PercentileSceneLuma(t, 1.0f, 0.0f, 10.0f, 90.0f), 1.0f));
    }

    // Black tiles (letterbox) are not counted: a letterboxed frame reads as its picture.
    {
        std::vector<float> t = Flat(0.0f, 800);
        t.resize(4096, 0.25f);
        CHECK(NearEv(PercentileSceneLuma(t, 1.0f, 1e-6f, 10.0f, 90.0f), 0.25f));
    }

    // No reading: a black frame, an empty grid, or under 5% of the grid left.
    CHECK(PercentileSceneLuma(Flat(0.0f), 1.0f, 1e-6f, 10.0f, 90.0f) == 0.0f);
    CHECK(PercentileSceneLuma(Flat(0.5f, 100), 1.0f, 0.0f, 10.0f, 90.0f) == 0.0f);
    CHECK(NearEv(PercentileSceneLuma(Flat(0.5f, 300), 1.0f, 0.0f, 10.0f, 90.0f), 0.5f));
    CHECK(PercentileSceneLuma({}, 1.0f, 0.0f, 10.0f, 90.0f) == 0.0f);

    // Not a number or an absurd tile: counted as black / clamped, never a broken result.
    {
        std::vector<float> t = Flat(0.5f, 4000);
        t.resize(4096, std::numeric_limits<float>::quiet_NaN());
        CHECK(NearEv(PercentileSceneLuma(t, 1.0f, 0.0f, 0.0f, 100.0f), 0.5f));
    }
    {
        const float v = PercentileSceneLuma(Flat(1e30f), 1.0f, 0.0f, 10.0f, 90.0f);
        CHECK(std::isfinite(v) && v > 0.0f);
    }

    // The window: a backwards or empty one is the whole range; a broken one the default; the range is clamped.
    {
        const Window w = Clamp(80.0f, 20.0f);
        CHECK(w.low == 0.0f && w.high == 100.0f);
        const Window e = Clamp(50.0f, 50.0f);
        CHECK(e.low == 0.0f && e.high == 100.0f);
        const Window n = Clamp(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity());
        CHECK(n.low == kDefaultLowPercent && n.high == kDefaultHighPercent);
        const Window c = Clamp(-5.0f, 500.0f);
        CHECK(c.low == 0.0f && c.high == 100.0f);
    }

    // A window narrower than one tile is the whole range too (the shader's `hi - lo < 1`).
    CHECK(NearEv(PercentileSceneLuma(Flat(0.5f, 300), 1.0f, 0.0f, 50.0f, 50.2f), 0.5f));

    // Brightening the scene by 1 EV moves the reading by 1 EV: the meter is in log space end to end.
    {
        std::vector<float> a = Flat(0.1f, 2000), b = Flat(0.2f, 2000);
        a.resize(4096, 0.4f);
        b.resize(4096, 0.8f);
        const float la = PercentileSceneLuma(a, 1.0f, 0.0f, 10.0f, 90.0f);
        const float lb = PercentileSceneLuma(b, 1.0f, 0.0f, 10.0f, 90.0f);
        CHECK(std::fabs(std::log2(lb / la) - 1.0f) < 0.01f);
    }

    if (fails == 0)
        printf("all passed\n");
    return fails == 0 ? 0 : 1;
}
