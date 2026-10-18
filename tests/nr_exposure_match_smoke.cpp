// Host check of DlssNr_ExposureMatch.h (which exposure scan candidate is the game's exposure, in what units, or whether
// the exposure is already in the picture). Synthetic cases first, then the two recordings in tests/data/exposure_scan:
// NBA 2K27 (handed texture, steps per scene) and Cyberpunk 2077 (nothing handed, adapts with the light).
// No GPU and no game needed.
// cl /std:c++20 /EHsc /W4 tests/nr_exposure_match_smoke.cpp
// nr_exposure_match_smoke.exe [folder with the recordings, default tests/data/exposure_scan]
#include "../OptiScaler/dlssnr/DlssNr_ExposureMatch.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace DlssNrExposureMatch;

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

// ---------------------------------------------------------------------------------------------------------------------
// Synthetic scenes. Rows are the recording's (about 10 a second). The true exposure walks through `ev` stops and back,
// smoothly, the way a player walks from a dark interior into daylight; the meter reads it with a little noise.

static uint32_t rng = 12345;
static float Rand01()
{
    rng = rng * 1664525u + 1013904223u;
    return (float) (rng >> 8) / 16777216.0f;
}

static std::vector<float> TrueExposure(size_t rows, float ev, float baseEv = -2.0f)
{
    std::vector<float> e(rows);
    for (size_t i = 0; i < rows; ++i)
    {
        const float t = (float) i / (float) rows;
        // two slow swings, so both halves of the recording see the full range
        e[i] = std::exp2(baseEv + ev * 0.5f * (1.0f - std::cos(t * 4.0f * 3.14159265f)));
    }
    return e;
}

static std::vector<float> Metered(const std::vector<float>& e, float noiseEv = 0.15f)
{
    std::vector<float> m(e.size());
    for (size_t i = 0; i < e.size(); ++i)
        m[i] = e[i] * std::exp2((Rand01() - 0.5f) * 2.0f * noiseEv);
    return m;
}

static std::vector<float> Eased(const std::vector<float>& e, float tauRows)
{
    std::vector<float> out(e.size());
    float le = std::log2(e[0]);
    const float blend = 1.0f - std::exp(-1.0f / tauRows);
    for (size_t i = 0; i < e.size(); ++i)
    {
        le += (std::log2(e[i]) - le) * blend;
        out[i] = std::exp2(le);
    }
    return out;
}

template <class F> static std::vector<float> Map(const std::vector<float>& e, F f)
{
    std::vector<float> out(e.size());
    for (size_t i = 0; i < e.size(); ++i)
        out[i] = f(e[i], i);
    return out;
}

static void Synthetic()
{
    const size_t rows = 1200; // two minutes
    const auto e = TrueExposure(rows, 6.0f);
    const auto meter = Metered(e);

    // Units, from the slope against the meter (itself an exposure multiplier: bigger = darker scene)
    {
        const Fit f = Classify(meter, Map(e, [](float x, size_t) { return 0.7f * x; }));
        CHECK(f.kind == Kind::Multiplier);
        CHECK(f.corr > 0.95f);
        CHECK(std::fabs(f.slope - 1.0f) < 0.1f);
    }
    {
        const Fit f = Classify(meter, Map(e, [](float x, size_t) { return 3.0f / x; }));
        CHECK(f.kind == Kind::Divisor);
        CHECK(std::fabs(f.slope + 1.0f) < 0.1f);
    }
    {
        // a stop value, crossing zero: no log of it can be taken
        const Fit f = Classify(meter, Map(e, [](float x, size_t) { return std::log2(x) + 0.5f; }));
        CHECK(f.kind == Kind::Stops);
        CHECK(std::fabs(f.scale - 1.0f) < 1e-6f); // snapped to log2
    }
    {
        // EV100 style: a stop value with a large offset never crosses zero, and fits log-log almost as tightly at a
        // slope no exposure has; it is still a stop value
        for (float offset : { 6.0f, 10.0f, 15.0f })
        {
            const Fit f = Classify(meter, Map(e, [offset](float x, size_t) { return std::log2(x) + offset; }));
            CHECK(f.kind == Kind::Stops);
            CHECK(std::fabs(f.scale - 1.0f) < 1e-6f);
        }
    }
    {
        // natural log of a luminance: stops the other way, ln scale
        const Fit f = Classify(meter, Map(e, [](float x, size_t) { return -std::log(x); }));
        CHECK(f.kind == Kind::Stops);
        CHECK(std::fabs(f.scale + std::log(2.0f)) < 1e-6f);
    }

    // Eye adaptation: the game's value trails the meter's raw reading; the lag search finds it
    {
        const Fit f = Classify(meter, Eased(e, 12.0f));
        CHECK(f.kind == Kind::Multiplier);
        CHECK(f.lag >= 6 && f.lag <= 24);
    }

    // What is not an exposure
    CHECK(Classify(meter, Map(e, [](float, size_t i) { return 0.1f * (float) i; })).kind == Kind::Timer);
    CHECK(Classify(meter, Map(e, [](float, size_t) { return Rand01(); })).kind == Kind::Noisy);
    CHECK(Classify(meter, Map(e, [](float, size_t) { return 1.0f; })).kind == Kind::Steady);
    CHECK(Classify(meter, Map(e, [](float, size_t i) { return i < 600 ? 1.0f : 1.3195f; })).kind == Kind::Steady);
    CHECK(Classify(meter, Map(e, [](float, size_t i) { return 1e-38f * (float) (1 + i % 7); })).kind == Kind::NotFloat);
    {
        // smooth, but its own: an unrelated slow value (a time of day, a fog density)
        const auto other = TrueExposure(rows, 3.0f, 0.0f);
        std::vector<float> shifted(rows);
        for (size_t i = 0; i < rows; ++i)
            shifted[i] = other[(i + rows / 3) % rows] * (1.0f + 0.3f * std::sin((float) i * 0.013f));
        CHECK(Classify(meter, shifted).kind == Kind::Unrelated);
    }

    // Match: the exposure among junk, and copies of it (Cyberpunk keeps three): the first of the best
    {
        std::vector<std::vector<float>> c;
        c.push_back(Map(e, [](float, size_t) { return 1.0f; }));
        c.push_back(Map(e, [](float, size_t i) { return 0.1f * (float) i; }));
        c.push_back(Map(e, [](float, size_t) { return Rand01(); }));
        c.push_back(Map(e, [](float x, size_t) { return 0.5f * x; }));
        c.push_back(Map(e, [](float x, size_t) { return 0.5f * x; }));
        const Verdict v = Match(meter, c);
        CHECK(v.what == What::Found);
        CHECK(v.index == 3);
        CHECK(v.fit.kind == Kind::Multiplier);
        CHECK(v.meterTravelEv > 5.0f);
    }

    // Nothing that follows the light: enough variety, no answer
    {
        std::vector<std::vector<float>> c;
        c.push_back(Map(e, [](float, size_t) { return 1.0f; }));
        c.push_back(Map(e, [](float, size_t) { return Rand01(); }));
        CHECK(Match(meter, c).what == What::None);
    }

    // Not enough brightness variety yet (1 EV): not sure, whatever the candidate does
    {
        const auto small = TrueExposure(rows, 1.0f);
        std::vector<std::vector<float>> c { Map(small, [](float x, size_t) { return x; }) };
        CHECK(Match(Metered(small, 0.05f), c).what == What::NotSure);
    }

    // Too short: not sure
    {
        std::vector<float> m(meter.begin(), meter.begin() + 100);
        std::vector<std::vector<float>> c { std::vector<float>(e.begin(), e.begin() + 100) };
        CHECK(Match(m, c).what == What::NotSure);
    }

    // Baked in: the game already exposed the colour, so our meter barely moves while its exposure swings
    {
        const auto flat = Metered(std::vector<float>(rows, 0.5f), 0.1f);
        std::vector<std::vector<float>> c;
        c.push_back(Map(e, [](float, size_t) { return 1.0f; }));
        c.push_back(Map(e, [](float, size_t) { return Rand01(); }));
        c.push_back(e);
        const Verdict v = Match(flat, c);
        CHECK(v.what == What::Baked);
        CHECK(v.index == 2);
    }

    // Flat meter and nothing swinging: a game that never changes its light, not sure
    {
        const auto flat = Metered(std::vector<float>(rows, 0.5f), 0.1f);
        std::vector<std::vector<float>> c { std::vector<float>(rows, 1.0f) };
        CHECK(Match(flat, c).what == What::NotSure);
    }

    // The two halves must agree: a value that follows the light one way, then the other way, is not the exposure
    {
        std::vector<std::vector<float>> c { Map(e, [](float x, size_t i) { return i < 600 ? x : 1.0f / x; }) };
        CHECK(Match(meter, c).what != What::Found);
    }

    // One cut between two brightness levels (a load from a dark area to a bright one): travel enough, but anything that
    // steps at the same cut (an area id) fits it -- not a verdict
    {
        std::vector<float> twoLevels(rows);
        for (size_t i = 0; i < rows; ++i)
            twoLevels[i] = i < rows / 2 ? 0.25f : 4.0f;
        std::vector<std::vector<float>> c { Map(twoLevels,
                                                [](float, size_t i) { return i < rows / 2 ? 3.0f : 7.0f; }) };
        CHECK(Match(Metered(twoLevels, 0.05f), c).what == What::NotSure);
    }

    // Baked needs the candidate's swing over as many levels: a two-level value beside a flat meter is not the exposure
    {
        const auto flat = Metered(std::vector<float>(rows, 0.5f), 0.1f);
        std::vector<std::vector<float>> c { Map(e, [](float, size_t i) { return i < rows / 2 ? 0.25f : 4.0f; }) };
        CHECK(Match(flat, c).what == What::NotSure);
    }

    // Rows without a meter reading (0: Automatic not running, a black frame) are left out, not read as dark
    {
        std::vector<float> m = meter;
        for (size_t i = 0; i < 200; ++i)
            m[i] = 0.0f;
        std::vector<std::vector<float>> c { Map(e, [](float x, size_t) { return 2.0f * x; }) };
        const Verdict v = Match(m, c);
        CHECK(v.what == What::Found);
        CHECK(v.rows == rows - 200);
    }

    // As an exposure, and the anchor: Automatic gives the level, the candidate the movement, with no lag
    {
        CHECK(std::fabs(AsExposure({ Kind::Multiplier }, 4.0f) - 4.0f) < 1e-6f);
        CHECK(std::fabs(AsExposure({ Kind::Divisor }, 4.0f) - 0.25f) < 1e-6f);
        Fit stops { Kind::Stops };
        stops.scale = 1.0f;
        CHECK(std::fabs(AsExposure(stops, 3.0f) - 8.0f) < 1e-5f);
        stops.scale = -std::log(2.0f); // -ln(luminance)
        CHECK(std::fabs(AsExposure(stops, -std::log(8.0f)) - 8.0f) < 1e-4f);
        CHECK(AsExposure({ Kind::Unrelated }, 4.0f) == 0.0f);

        // the true exposure steps 1 -> 8 at row 600; the game's value (0.5x) has it at once, our meter eases over
        // seconds
        std::vector<float> step(rows);
        for (size_t i = 0; i < rows; ++i)
            step[i] = i < 600 ? 1.0f : 8.0f;
        const auto cand = Map(step, [](float x, size_t) { return 0.5f * x; });
        const auto slow = Eased(step, 30.0f);
        const Fit f { Kind::Multiplier };
        const float k = AnchorScale(f, slow, cand);
        CHECK(std::fabs(k - 2.0f) < 0.05f);                           // the median ignores the easing's transient
        CHECK(std::fabs(k * AsExposure(f, cand[600]) - 8.0f) < 0.2f); // right on the step's row
        CHECK(AnchorScale(f, std::vector<float>(rows, 0.0f), cand) == 0.0f); // no reading, no anchor
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Recordings (tests/data/exposure_scan, trimmed copies of the scan's CSV: the columns keep the scan's candidate
// numbers)

struct Recording
{
    std::vector<std::string> columns; // c1, c14, ...
    std::vector<float> meter;
    std::vector<float> game;
    std::vector<std::vector<float>> candidates;
    bool ok = false;
};

static Recording Load(const std::string& path)
{
    Recording r;
    std::ifstream in(path);
    if (!in)
        return r;

    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r') // a checkout with CRLF endings
            line.pop_back();

        if (line.empty() || line[0] == '#')
            continue;

        std::vector<std::string> cells;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ','))
            cells.push_back(cell);

        if (cells.size() < 5)
            continue;

        if (cells[0] == "seconds")
        {
            r.columns.assign(cells.begin() + 5, cells.end());
            r.candidates.resize(r.columns.size());
            continue;
        }

        r.meter.push_back(std::stof(cells[2]));
        r.game.push_back(std::stof(cells[3]));
        for (size_t i = 0; i < r.columns.size(); ++i)
            r.candidates[i].push_back(i + 5 < cells.size() ? std::stof(cells[i + 5]) : 0.0f);
    }

    r.ok = !r.meter.empty() && !r.columns.empty();
    return r;
}

// The candidate column called `name`; a missing one fails the test and reads as an empty column
static const std::vector<float>& Column(const Recording& r, const char* name)
{
    static const std::vector<float> none;

    for (size_t i = 0; i < r.columns.size(); ++i)
        if (r.columns[i] == name)
            return r.candidates[i];

    printf("FAIL: no column %s\n", name);
    ++fails;
    return none;
}

static void Recordings(const std::string& folder)
{
    // NBA 2K27: the handed texture (c14) holds one value per scene (1.3195 through the whole game) while our meter
    // swings ~5 EV, so nothing here follows the light; the handed texture is used directly upstream, the scan is not
    // needed. Automatic ran for ~36 s only: not enough rows for a verdict.
    {
        const Recording r = Load(folder + "/nba2k27_handed.csv");
        CHECK(r.ok);
        if (r.ok)
        {
            CHECK(Classify(r.meter, Column(r, "c14")).kind == Kind::Steady);
            CHECK(Classify(r.meter, Column(r, "c15")).kind == Kind::Steady);
            CHECK(Classify(r.meter, Column(r, "c2")).kind == Kind::NotFloat);
            CHECK(Classify(r.meter, Column(r, "c4")).kind == Kind::NotFloat);
            CHECK(Match(r.meter, r.candidates).what != What::Found);
        }
    }

    // Cyberpunk 2077: c61/c62 (1x1 R16_FLOAT) and c63 (its buffer copy) are the game's exposure multiplier, ~9 EV of
    // travel, corr 0.986 against our meter at lag 0. The other movers: c9 and c11 move smoothly but not with the light
    // (c11 at corr ~0.6), c24 is integer bits, c13 and c30 change a handful of times; c4 is a constant.
    {
        const Recording r = Load(folder + "/cyberpunk_interior_daylight.csv");
        CHECK(r.ok);
        if (r.ok)
        {
            const Verdict v = Match(r.meter, r.candidates);
            CHECK(v.what == What::Found);
            CHECK(v.index >= 0 &&
                  (r.columns[v.index] == "c61" || r.columns[v.index] == "c62" || r.columns[v.index] == "c63"));
            CHECK(v.fit.kind == Kind::Multiplier);
            CHECK(v.fit.corr > 0.95f);
            CHECK(v.meterTravelEv > 6.0f);

            for (const char* exposure : { "c61", "c62", "c63" })
                CHECK(Classify(r.meter, Column(r, exposure)).kind == Kind::Multiplier);
            CHECK(Classify(r.meter, Column(r, "c4")).kind == Kind::Steady);
            CHECK(Classify(r.meter, Column(r, "c9")).kind == Kind::Unrelated);
            CHECK(Classify(r.meter, Column(r, "c11")).kind == Kind::Unrelated);
            CHECK(Classify(r.meter, Column(r, "c13")).kind == Kind::Steady);
            CHECK(Classify(r.meter, Column(r, "c24")).kind == Kind::NotFloat);
            CHECK(Classify(r.meter, Column(r, "c30")).kind == Kind::Steady);

            // Automatic gives the level: k near 1, the game's exposure is about our meter's size
            const std::vector<float>& best = Column(r, "c61");
            const float k = AnchorScale(Fit { Kind::Multiplier }, r.meter, best);
            CHECK(k > 0.5f && k < 2.0f);
            printf("  cyberpunk: %s, slope %.3f, corr %.3f, lag %d, %.1f EV, k %.3f\n", r.columns[v.index].c_str(),
                   v.fit.slope, v.fit.corr, v.fit.lag, v.meterTravelEv, k);
        }
    }
}

int main(int argc, char** argv)
{
    Synthetic();
    Recordings(argc > 1 ? argv[1] : "tests/data/exposure_scan");

    printf(fails == 0 ? "nr_exposure_match_smoke: PASS\n" : "nr_exposure_match_smoke: %d FAIL\n", fails);
    return fails == 0 ? 0 : 1;
}
