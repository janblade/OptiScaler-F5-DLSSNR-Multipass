// Host check of Compress screen edges' layout (shaders/dlssnr/DlssNr_Spatial.h and external/peripheral_warp): the packed
// size, the 16-pixel grid, the 1:1 middle at 100% model resolution, pack / unpack round trips, offsets and shifts, the
// limits the menu shares with the runtime, and what a bad value turns into. No GPU and no game needed.
// cl /std:c++20 /EHsc /W4 tests/nr_spatial_mapping_smoke.cpp
#include "../OptiScaler/shaders/dlssnr/DlssNr_Spatial.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace DlssNr::Spatial;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);                                                           \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

static bool Near(float a, float b, float tolerance = .01f)
{
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance;
}

// Pack then unpack must give the pixel back, in the frame and beyond its edges (motion lands there).
static bool RoundTrips(const pw::Axis& axis)
{
    for (float pixel : { -64.0f, 0.5f, axis.bandCenter - axis.halfBand, axis.bandCenter,
                         axis.bandCenter + axis.halfBand, axis.nativeExtent - .5f, axis.nativeExtent + 64.0f })
        if (!Near(pw::Unpack(pw::Pack(pixel, axis), axis), pixel))
            return false;
    // And monotonic: the mapping never folds back on itself.
    float previous = pw::Pack(-64, axis);
    for (int i = 0; i <= 256; ++i)
    {
        const float packed = pw::Pack((axis.nativeExtent + 128) * i / 256.0f - 64, axis);
        if (!std::isfinite(packed) || packed < previous - .01f)
            return false;
        previous = packed;
    }
    return true;
}

// The middle band moves by whole texels and keeps its size: every step inside it is one packed pixel.
static bool MiddleIsOneToOne(const pw::Axis& axis)
{
    const float shift = pw::Pack(axis.bandCenter, axis) - axis.bandCenter;
    if (!Near(shift, std::round(shift), .001f))
        return false;
    for (float pixel = axis.bandCenter - axis.halfBand + 1; pixel < axis.bandCenter + axis.halfBand - 1; pixel += 7)
        if (!Near(pw::Pack(pixel + 1, axis) - pw::Pack(pixel, axis), 1.0f, .001f))
            return false;
    return true;
}

static Settings On()
{
    Settings s;
    s.enabled = true;
    return s;
}

int main()
{
    // Off: nothing changes, and the ordinary size is the grid size the runtime already uses.
    {
        Layout l = Build(Settings {}, 3840, 2160, 1.0f);
        CHECK(!l.requested && !l.active && l.status == Status::Off);
        CHECK(l.ordinaryW == 3840 && l.ordinaryH == 2160 && l.modelW == 3840 && l.modelH == 2160);
        l = Build(Settings {}, 1920, 1080, 0.67f);
        CHECK(l.ordinaryW == AlignedExtent((unsigned) (1920 * 0.67f + 0.5f), 1920));
        CHECK(l.ordinaryW % 16 == 0 && l.ordinaryH % 16 == 0 && l.modelW == l.ordinaryW);
        l = Build(Settings {}, 1920, 1080, 0.67f, false);
        CHECK(l.ordinaryW == (unsigned) (1920 * 0.67f + 0.5f));
    }

    // The grid rule itself: native stays native, shrinking never rounds up past native, nothing under 16.
    CHECK(AlignedExtent(1080, 1080) == 1080);
    CHECK(AlignedExtent(1079, 1080) == 1072);
    CHECK(AlignedExtent(1083, 1084) == 1084); // would round to 1088, above native
    CHECK(AlignedExtent(1075, 1080) == 1072);
    CHECK(AlignedExtent(3, 1080) == 16);
    CHECK(AlignedExtent(2170, 1080) == 2176);

    // Default layout at 4K and 100%: 80 / 90, the packed size on the grid, the middle exactly 1:1.
    {
        const Layout l = Build(On(), 3840, 2160, 1.0f);
        CHECK(l.active && l.status == Status::Active);
        CHECK(l.modelW == 3456 && l.modelH == 1952); // 1944 rounds to the 16 grid
        CHECK(l.modelW % 16 == 0 && l.modelH % 16 == 0);
        CHECK(l.ordinaryW == 3840 && l.ordinaryH == 2160);
        CHECK(MiddleIsOneToOne(l.warp.x) && MiddleIsOneToOne(l.warp.y));
        CHECK(Near(pw::Pack(0, l.warp.x), 0) && Near(pw::Pack(3840, l.warp.x), 3456));
        CHECK(Near(pw::Pack(0, l.warp.y), 0) && Near(pw::Pack(2160, l.warp.y), 1952));
        CHECK(Near(pw::Pack(1920, l.warp.x), 1728));
        CHECK(RoundTrips(l.warp.x) && RoundTrips(l.warp.y));
        CHECK(Near(l.centerBounds.left, .1f) && Near(l.centerBounds.right, .9f));
        CHECK(Near(l.workBounds.left, .05f) && Near(l.workBounds.right, .95f)); // the 90% working region
        CHECK(Near(l.workBounds.top, .05f, .002f) && Near(l.workBounds.bottom, .95f, .002f));
        // The centre is wider than the packed picture's share of the edge, so the edge pixels are fewer than native.
        CHECK(pw::Pack(192, l.warp.x) < 192 - 1);
    }

    // The 1:1 middle at 100% holds on frames that are not on the grid, asymmetric layouts, offsets and shifts.
    {
        Settings s = On();
        for (const auto size : { std::pair<uint32_t, uint32_t> { 1920, 1080 }, { 1921, 1081 }, { 2560, 1440 },
                                 { 1366, 768 }, { 1280, 800 }, { 3440, 1440 }, { 1000, 1000 } })
        {
            Layout l = Build(s, size.first, size.second, 1.0f);
            CHECK(l.active);
            CHECK(l.modelW <= size.first && l.modelH <= size.second); // shrinking never goes above native
            CHECK(l.modelW % 16 == 0 || l.modelW == size.first);
            CHECK(l.modelH % 16 == 0 || l.modelH == size.second);
            CHECK(MiddleIsOneToOne(l.warp.x) && MiddleIsOneToOne(l.warp.y));
            CHECK(RoundTrips(l.warp.x) && RoundTrips(l.warp.y));
        }

        s.centerX = 70; s.workX = 85; s.centerY = 90; s.workY = 95;
        s.offsetX = 3; s.offsetY = -2;
        s.shiftX = 1; s.shiftY = -0.5f;
        const Layout l = Build(s, 2560, 1440, 1.0f);
        CHECK(l.active);
        CHECK(MiddleIsOneToOne(l.warp.x) && MiddleIsOneToOne(l.warp.y));
        CHECK(RoundTrips(l.warp.x) && RoundTrips(l.warp.y));
        CHECK(l.centerBounds.left > .15f && l.centerBounds.top < .05f + .03f); // moved off centre
        CHECK(l.workBounds.left >= 0.0f && l.workBounds.right <= 1.0f + 1e-4f);
    }

    // Sign of the edge balance: a positive shift gives the right (bottom) edge more packed pixels and the left (top)
    // fewer; a negative shift the opposite. This is what the menu's help text says.
    {
        Settings s = On();
        const Layout none = Build(s, 2560, 1440, 1.0f);
        s.shiftX = 2.0f;
        const Layout right = Build(s, 2560, 1440, 1.0f);
        s.shiftX = -2.0f;
        const Layout left = Build(s, 2560, 1440, 1.0f);
        CHECK(none.active && right.active && left.active);
        CHECK(right.warp.x.allotted[1] > none.warp.x.allotted[1] && right.warp.x.allotted[0] < none.warp.x.allotted[0]);
        CHECK(left.warp.x.allotted[0] > none.warp.x.allotted[0] && left.warp.x.allotted[1] < none.warp.x.allotted[1]);
        // The same in the picture: the right edge's last native 100 pixels take more packed pixels when shifted right.
        const float rightEdgeNone = pw::Pack(2560, none.warp.x) - pw::Pack(2460, none.warp.x);
        const float rightEdgeRight = pw::Pack(2560, right.warp.x) - pw::Pack(2460, right.warp.x);
        CHECK(rightEdgeRight > rightEdgeNone);
        CHECK(RoundTrips(right.warp.x) && RoundTrips(left.warp.x));
    }

    // Offsets move the middle: positive is right / down.
    {
        Settings s = On();
        s.offsetX = 4;
        s.offsetY = -3;
        const Layout l = Build(s, 2560, 1440, 1.0f);
        CHECK(l.active);
        CHECK(l.centerBounds.left > .1f + .03f && l.centerBounds.right > .9f);
        CHECK(l.centerBounds.top < .1f - .02f);
        CHECK(MiddleIsOneToOne(l.warp.x) && MiddleIsOneToOne(l.warp.y));
    }

    // Away from 100% the picture is resampled, so the middle only has to keep its proportions, and the packed size
    // follows the model resolution that is applied.
    {
        Layout l = Build(On(), 1921, 1081, .85f);
        CHECK(l.active && l.ordinaryW % 16 == 0);
        CHECK(l.modelW % 16 == 0 && l.modelH % 16 == 0);
        CHECK(l.modelW < l.ordinaryW && l.modelH < l.ordinaryH);
        CHECK(RoundTrips(l.warp.x) && RoundTrips(l.warp.y));

        l = Build(On(), 1920, 1080, .5f);
        CHECK(l.active && l.ordinaryW == 960 && l.modelW == 864);
        CHECK(Near((float) l.modelW / (float) l.ordinaryW, 0.9f, .02f));
        CHECK(RoundTrips(l.warp.x) && RoundTrips(l.warp.y));

        // Supersampling: the packed picture is above native, and the ordinary grid is too.
        l = Build(On(), 1921, 1081, 2.0f);
        CHECK(l.active && l.ordinaryW > 1921 && l.modelW > 1921 && l.modelW < l.ordinaryW);
        CHECK(l.modelW % 16 == 0 && l.modelH % 16 == 0);
        CHECK(RoundTrips(l.warp.x) && RoundTrips(l.warp.y));

        // Auto's continuous ratio landing a hair under 100% still counts as native.
        l = Build(On(), 1920, 1080, 0.9998f);
        CHECK(l.active && l.ordinaryW == 1920 && MiddleIsOneToOne(l.warp.x));
    }

    // A quarter of the frame is the floor, checked after the rounding.
    {
        Layout l = Build(On(), 1920, 1080, .25f);
        CHECK(!l.active && l.requested && l.status == Status::TooSmall);
        CHECK(l.modelW == l.ordinaryW);
        Settings s = On();
        s.centerX = s.centerY = 20;
        s.workX = s.workY = 25;
        l = Build(s, 1920, 1080, 1.0f);
        CHECK(l.status == Status::TooSmall || l.status == Status::Active); // either, never half-built
        if (l.active)
            CHECK(l.modelW >= 1920 / 4 && l.modelH >= 1080 / 4);
    }

    // Nothing to compress, and one axis alone.
    {
        Settings s = On();
        s.workX = s.workY = 100;
        Layout l = Build(s, 1920, 1080, 1.0f);
        CHECK(!l.active && l.status == Status::NothingToCompress && l.modelW == 1920);

        s.workY = 90;
        l = Build(s, 1920, 1080, 1.0f);
        CHECK(l.active && l.modelW == 1920 && l.modelH < 1080);
        CHECK(Near(pw::Pack(777.5f, l.warp.x), 777.5f)); // the 100% axis is the identity
        CHECK(RoundTrips(l.warp.x) && RoundTrips(l.warp.y));
    }

    // Invalid values turn compression off with a reason; the picture is the ordinary one.
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        auto bad = [](Settings s) {
            const Layout l = Build(s, 1920, 1080, 1.0f);
            return !l.active && l.requested && l.status == Status::BadSettings && l.modelW == 1920;
        };
        Settings s = On();
        s.centerX = nan; CHECK(bad(s));
        s = On(); s.workY = inf; CHECK(bad(s));
        s = On(); s.centerX = 0; CHECK(bad(s));
        s = On(); s.centerY = -5; CHECK(bad(s));
        s = On(); s.centerX = 90; s.workX = 90; CHECK(bad(s));  // centre must be under work
        s = On(); s.workX = 24; CHECK(bad(s));
        s = On(); s.workX = 101; CHECK(bad(s));
        s = On(); s.offsetX = 50; CHECK(bad(s));
        s = On(); s.offsetY = nan; CHECK(bad(s));
        s = On(); s.shiftX = 50; CHECK(bad(s));
        s = On(); s.shiftY = -inf; CHECK(bad(s));
        CHECK(Build(On(), 1, 1, 1.0f).status == Status::BadSettings);
        CHECK(Build(On(), 1920, 1080, nan).status == Status::BadSettings);
        CHECK(Build(On(), 1920, 1080, 3.0f).status == Status::BadSettings);

        // The ends of the shift range leave an edge with no pixels: said so, not silently clipped.
        s = On();
        s.shiftX = WorkShiftLimits(s, false).first;
        const Layout l = Build(s, 3840, 2160, 1.0f);
        CHECK(!l.active && l.requested && l.status == Status::ThinEdge);
    }

    // Equal settings compare equal, NaN included, so a hand-edited ini does not look like a change every frame.
    {
        Settings s = On();
        s.centerX = std::numeric_limits<float>::quiet_NaN();
        const Settings same = s;
        CHECK(s == same && Build(s, 1920, 1080, 1.0f) == Build(same, 1920, 1080, 1.0f));
        s.workX = 91;
        CHECK(s != same);
        CHECK(Build(On(), 1920, 1080, 1.0f) != Build(On(), 1920, 1081, 1.0f));
        CHECK(Build(On(), 1920, 1080, 1.0f) != Build(On(), 1920, 1080, 0.75f));
    }

    // The named layouts: valid wherever the packed picture stays over a quarter of the frame, and not otherwise.
    {
        for (int i = 0; i != 3; ++i)
        {
            Settings s = On();
            ApplyNamedLayout(s, i);
            CHECK(MatchedLayout(s) == i);
            for (int percent = 25; percent <= 200; percent += 5)
            {
                const float scale = percent / 100.0f;
                const Layout l = Build(s, 1920, 1080, scale);
                const float nominal = scale * s.workX * .01f;
                if (nominal >= .27f)
                {
                    CHECK(l.active);
                    if (l.active)
                        CHECK(RoundTrips(l.warp.x) && RoundTrips(l.warp.y));
                }
                else if (nominal < .24f)
                    CHECK(!l.active && l.status == Status::TooSmall);
            }
        }
        Settings custom = On();
        CHECK(MatchedLayout(custom) == kDefaultLayout);
        custom.offsetX = 1;
        CHECK(MatchedLayout(custom) == -1);
        custom = On();
        custom.centerX = 81;
        CHECK(MatchedLayout(custom) == -1);
    }

    // What the menu's edits can reach is always a layout the runtime accepts (when the picture is not too small), and
    // Constrain leaves a valid one alone.
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        Settings wild = On();
        wild.centerX = 150; wild.centerY = nan; wild.workX = 3; wild.workY = 70;
        wild.offsetX = -90; wild.offsetY = 90; wild.shiftX = 70; wild.shiftY = nan;
        Constrain(wild, 1.0f);
        // Never a value the runtime calls invalid. A middle only half a percent under the working size can still leave
        // no room for the edge once the packed size is rounded to the grid; that is reported as a thin edge.
        const Status wildStatus = Build(wild, 1920, 1080, 1.0f).status;
        CHECK(wildStatus == Status::Active || wildStatus == Status::ThinEdge);
        wild.workY = 90;
        Constrain(wild, 1.0f);
        CHECK(Build(wild, 1920, 1080, 1.0f).active);

        Settings s = On();
        ApplyNamedLayout(s, 2);
        s.offsetX = 5; s.shiftY = 1.5f;
        const Settings before = s;
        Constrain(s, 1.0f);
        CHECK(s == before);
        // A model resolution change lifts the working size to the new floor.
        Constrain(s, 0.5f);
        CHECK(s.workX >= 50.0f && s.workY >= 50.0f);
        CHECK(Build(s, 1920, 1080, 0.5f).active);
        CHECK(WorkShiftLimits(s, false).first <= 0.0f && WorkShiftLimits(s, false).second >= 0.0f);
        CHECK(Near(MinimumWorkPercent(0.5f), 50.0f) && Near(MinimumWorkPercent(2.0f), 25.0f) &&
              Near(MinimumWorkPercent(0.1f), 100.0f));
    }

    // The shader constants: the sizes, the guide regions, the motion scale.
    {
        const Layout l = Build(On(), 3840, 2160, 1.0f);
        const DlssNr::GuideRegions guides { { 16, 8, 2560, 1440 }, { 64, 32, 3840, 2160 } };
        Constants c = MakeConstants(l, 101, guides, 3.0f, -2.0f);
        CHECK(c.mode == 101 && c.width == l.modelW && c.height == l.modelH);
        CHECK(c.depthRect[0] == 16 && c.depthRect[2] == 2560 && c.motionRect[0] == 64 && c.motionRect[2] == 3840);
        CHECK(c.motionScale[0] == 3 && c.motionScale[1] == -2);
        c = MakeConstants(l, 100, guides, 1, 1);
        CHECK(c.mode == 100 && c.width == l.modelW && c.height == l.modelH);
        c = MakeConstants(l, 102, guides, 1, 1);
        CHECK(c.mode == 102 && c.width == 3840 && c.height == 2160);
        CHECK(c.warp.nativeWidth == 3840 && c.warp.workWidth == (float) l.modelW);
    }

    // What is published: boxes only while active, the sizes either way.
    {
        const Layout l = Build(On(), 2560, 1440, 1.0f);
        Published p = Publish(l, Status::Active, false);
        CHECK(p.status == Status::Active && p.modelW == l.modelW && p.center.left == l.centerBounds.left);
        p = Publish(l, Status::TurnedOffDispatch, false);
        CHECK(p.status == Status::TurnedOffDispatch && p.modelW == 0 && p.ordinaryW == 2560);
    }

    // The bookkeeping both backends share: a change of layout or of what is packed resets the model's history and
    // lifts a held fallback; the same frame again changes nothing; Retry lifts it too; the proxy backend keeps it off.
    {
        Tracker tracker;
        Settings s = On();
        auto frame = tracker.Begin(s, 1920, 1080, 1.0f, true, false, 7);
        CHECK(frame.active && frame.status == Status::Active && !frame.resetHistory && frame.changed);
        frame = tracker.Begin(s, 1920, 1080, 1.0f, true, false, 7);
        CHECK(frame.active && !frame.resetHistory && !frame.changed);

        tracker.TurnOff(Status::TurnedOffDispatch);
        frame = tracker.Begin(s, 1920, 1080, 1.0f, true, false, 7);
        CHECK(!frame.active && frame.status == Status::TurnedOffDispatch && !frame.resetHistory && frame.changed);
        frame = tracker.Begin(s, 1920, 1080, 1.0f, true, false, 7);
        CHECK(!frame.active && frame.status == Status::TurnedOffDispatch && !frame.changed); // held, not retried

        s.workX = s.workY = 85;
        frame = tracker.Begin(s, 1920, 1080, 1.0f, true, false, 7);
        CHECK(frame.active && frame.resetHistory); // a new layout gets its try, with a fresh history
        tracker.TurnOff(Status::TurnedOffModel);
        frame = tracker.Begin(s, 1920, 1080, 1.0f, true, false, 8);
        CHECK(frame.active && frame.resetHistory); // so does a different input format or size
        tracker.TurnOff(Status::TurnedOffModel);
        tracker.Retry();
        frame = tracker.Begin(s, 1920, 1080, 1.0f, true, false, 8);
        CHECK(frame.active && !frame.resetHistory);

        frame = tracker.Begin(s, 1920, 1080, 1.0f, true, true, 8);
        CHECK(!frame.active && frame.status == Status::Proxy);
        frame = tracker.Begin(Settings {}, 1920, 1080, 1.0f, true, true, 8);
        CHECK(!frame.active && frame.status == Status::Off && frame.resetHistory); // switched off: history restarts

        // A request that is invalid from the start never reset anything, and says why once.
        Tracker invalid;
        Settings bad = On();
        bad.workX = 120;
        frame = invalid.Begin(bad, 1920, 1080, 1.0f, true, false, 1);
        CHECK(!frame.active && frame.status == Status::BadSettings && frame.changed);
        frame = invalid.Begin(bad, 1920, 1080, 1.0f, true, false, 1);
        CHECK(!frame.changed && !frame.resetHistory);

        CHECK(Mix(Mix(0, 1), 2) != Mix(Mix(0, 2), 1));
    }

    if (fails == 0)
        std::puts("PASS: NR compress screen edges layout (16 grid, 1:1 middle, round trips, offsets, shifts, limits, "
                  "invalid values, named layouts, constants)");
    return fails == 0 ? 0 : 1;
}
