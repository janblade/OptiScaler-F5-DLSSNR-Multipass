// Host check of DlssNr_TrimAnchors.h: parse, serialize, upsert, interpolation, and that FillConstants
// lays the pairs out in the order the shader reads them. No GPU needed.
// cl /std:c++20 /EHsc tests/nr_trim_anchors_smoke.cpp
#include "../OptiScaler/shaders/dlssnr/DlssNr_TrimAnchors.h"
#include <cstdio>
#include <cstdlib>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps * std::max(1.0f, std::fabs(b)); }

int main()
{
    using namespace DlssNrTrim;

    CHECK(Parse("").empty());
    CHECK(Parse("garbage").empty());
    CHECK(Parse("100:5;").size() == 1);
    CHECK(near(Parse("100:5;")[0].key, 100.0f) && near(Parse("100:5;")[0].trim, 5.0f));

    // sorted by key, malformed pair skipped, trim clamped to [0.25, 50]
    auto a = Parse("200:8;abc;50:2;30:999;-1:3;0:3;");
    CHECK(a.size() == 3);
    CHECK(near(a[0].key, 30.0f) && near(a[0].trim, 50.0f));
    CHECK(near(a[1].key, 50.0f) && near(a[1].trim, 2.0f));
    CHECK(near(a[2].key, 200.0f) && near(a[2].trim, 8.0f));

    // a ninth anchor is dropped
    CHECK(Parse("1:1;2:1;3:1;4:1;5:1;6:1;7:1;8:1;9:1;").size() == 8);

    // round trip
    CHECK(Parse(Serialize(a)).size() == 3);

    // no anchors, preview and bad key all give the slider
    CHECK(near(TrimForKey(100, 3, {}, false), 3));
    CHECK(near(TrimForKey(100, 3, a, true), 3));
    CHECK(near(TrimForKey(0, 3, a, false), 3));
    CHECK(near(TrimForKey(-5, 3, a, false), 3));
    // slider is clamped
    CHECK(near(TrimForKey(100, 500, {}, false), 50));
    CHECK(near(TrimForKey(100, 0.01f, {}, false), 0.25f));

    // one anchor applies everywhere
    auto one = Parse("100:5;");
    CHECK(near(TrimForKey(1, 1, one, false), 5));
    CHECK(near(TrimForKey(1e6f, 1, one, false), 5));

    // flat beyond the ends, exact at the anchors, geometric mean halfway in log space
    CHECK(near(TrimForKey(10, 1, a, false), 50));       // below first
    CHECK(near(TrimForKey(1000, 1, a, false), 8));      // above last
    CHECK(near(TrimForKey(50, 1, a, false), 2));
    CHECK(near(TrimForKey(200, 1, a, false), 8));
    // between 50->2 and 200->8: key 100 is the log midpoint, trim is the geometric mean 4
    CHECK(near(TrimForKey(100, 1, a, false), 4));

    // upsert: within 2% replaces, otherwise adds, full table refuses
    auto b = Parse("100:5;");
    CHECK(Upsert(b, 101, 7) && b.size() == 1 && near(b[0].trim, 7));
    CHECK(Upsert(b, 300, 2) && b.size() == 2);
    CHECK(!Upsert(b, 0, 2));
    auto full = Parse("1:1;2:1;3:1;4:1;5:1;6:1;7:1;8:1;");
    CHECK(!Upsert(full, 50, 2) && full.size() == 8);

    // The menu's helpers on the ini text: Tune's result added as a point, one removed, the table full.
    {
        std::string text;
        CHECK(AddPoint(text, 100.0f, 4.0f) && Parse(text).size() == 1);
        CHECK(AddPoint(text, 400.0f, 2.0f) && Parse(text).size() == 2);
        CHECK(AddPoint(text, 101.0f, 6.0f) && Parse(text).size() == 2); // within 2% of 100: replaced, not added
        CHECK(near(Parse(text)[0].trim, 6.0f));
        CHECK(!AddPoint(text, 0.0f, 3.0f) && Parse(text).size() == 2);  // an unusable key changes nothing
        CHECK(!AddPoint(text, -5.0f, 3.0f) && Parse(text).size() == 2);
        RemovePoint(text, 7);                                           // out of range: nothing
        CHECK(Parse(text).size() == 2);
        RemovePoint(text, 0);
        CHECK(Parse(text).size() == 1 && near(Parse(text)[0].key, 400.0f));
        RemovePoint(text, 0);
        CHECK(text.empty() && Parse(text).empty());

        std::string many;
        for (int i = 0; i < 8; ++i)
            CHECK(AddPoint(many, 10.0f * std::pow(2.0f, (float) i), 2.0f));
        CHECK(!AddPoint(many, 3.0f, 2.0f) && Parse(many).size() == 8);  // full, nothing within reach
        CHECK(AddPoint(many, 10.1f, 5.0f) && Parse(many).size() == 8);  // full, but within reach of one: replaced
    }
    CHECK(Upsert(full, 4.05f, 9) && full.size() == 8); // replaces the 4

    // FillConstants writes pairs in the shader's field order
    DlssNrConstants c {};
    FillConstants(c, 3.0f, a, false, 250.0f);
    CHECK(c.ExposureTrimAnchorCount == 3);
    CHECK(near(c.ExposureTrimAnchorExposure0, 30) && near(c.ExposureTrimAnchorTrim0, 50));
    CHECK(near(c.ExposureTrimAnchorExposure1, 50) && near(c.ExposureTrimAnchorTrim1, 2));
    CHECK(near(c.ExposureTrimAnchorExposure2, 200) && near(c.ExposureTrimAnchorTrim2, 8));
    CHECK(near(c.ExposureTrim, 3) && c.ExposureTrimPreview == 0);
    CHECK(near(c.AutoExposureShadowProtection, 100)); // clamped
    static_assert(sizeof(DlssNrConstants) == 256);

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
