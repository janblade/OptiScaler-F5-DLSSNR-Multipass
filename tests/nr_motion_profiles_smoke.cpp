// Host test for OptiScaler/motion/MotionProfiles.h, no GPU: Default is the settings structs' own defaults, a profile
// sets only its listed knobs, the Custom list reads back to the same values, bad entries are skipped and values are
// bounded. Build (vcvars64, from the repo root):
//   cl /std:c++20 /EHsc /W4 tests\nr_motion_profiles_smoke.cpp

#define NOMINMAX
#include <windows.h>

#include "../OptiScaler/motion/OpticalFlow_Dx12.h"
#include "../OptiScaler/motion/TrustMask_Dx12.h"
#include "../OptiScaler/motion/MotionProfiles.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

using namespace native::motion;

static int g_failed = 0;

#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #x);                                                                \
            ++g_failed;                                                                                                \
        }                                                                                                              \
    } while (0)

using Flow = OpticalFlowDx12::Settings;
using Trust = TrustMaskDx12::Settings;

static std::string Knobs(const Flow& f, const Trust& t) { return WriteKnobs(f, t); }

int main()
{
    const std::string defaults = Knobs(Flow {}, Trust {});

    // Default: the structs' defaults, whatever the settings were before; the debug view (a menu choice) is kept
    {
        Flow f;
        Trust t;
        f.radius = 3;
        f.lambda = 0.5f;
        t.uncoverReach = 2.0f;
        t.debugView = 4;
        ApplyProfile(Profile::Default, "radius=2", f, t);
        CHECK(Knobs(f, t) == defaults);
        CHECK(t.debugView == 4);
    }

    // Every genre profile sets only what its list names (an empty list is Default); offered only with a list
    for (const Profile profile : kAllProfiles)
    {
        if (profile == Profile::Custom)
            continue;

        Flow f;
        Trust t;
        ApplyProfile(profile, "", f, t);
        Flow expectFlow;
        Trust expectTrust;
        ParseKnobs(ProfileKnobs(profile), expectFlow, expectTrust);
        CHECK(Knobs(f, t) == Knobs(expectFlow, expectTrust));
        CHECK(ProfileOffered(profile) == (profile == Profile::Default || ProfileKnobs(profile)[0] != '\0'));
    }
    CHECK(ProfileOffered(Profile::Custom));

    // Ini words round-trip; anything else is Default
    for (const Profile profile : kAllProfiles)
        CHECK(ProfileFromIniText(ProfileIniText(profile)) == profile);
    CHECK(ProfileFromIniText("") == Profile::Default);
    CHECK(ProfileFromIniText("Anime") == Profile::Default); // Config lower-cases it first
    CHECK(ProfileFromIniText("nonsense") == Profile::Default);

    // Custom: parse(write(x)) == x, odd floats included
    {
        Flow f;
        Trust t;
        f.radius = 2;
        f.smoothRadius = 0;
        f.useHistory = false;
        f.confidenceKnee = 0.0123456789f;
        f.stillEpsilon = 0.0007f;
        f.zeroMargin = 0.001f;
        f.perceptualLuma = false;
        t.uncoverBackground = false;
        t.flowTolerance = 2.75f;
        const std::string text = Knobs(f, t);

        Flow g;
        Trust u;
        ApplyProfile(Profile::Custom, text, g, u);
        CHECK(Knobs(g, u) == text);
        CHECK(g.confidenceKnee == f.confidenceKnee && g.stillEpsilon == f.stillEpsilon);
        CHECK(g.radius == 2 && !g.useHistory && !g.perceptualLuma && !u.uncoverBackground);
    }

    // Bad entries are skipped, good ones beside them still apply; missing keys keep their defaults
    {
        Flow f;
        Trust t;
        const int set = ParseKnobs("radius=2,bogus=1,smooth=,=3,cells=abc,knee=0.01x,lambda=nan,still=12;look=0 depthtol=0.2",
                                   f, t);
        CHECK(set == 4);
        CHECK(f.radius == 2 && f.stillFrames == 12 && !f.lookWeights && t.depthTolerance == 0.2f);
        CHECK(f.smoothRadius == Flow {}.smoothRadius && f.coarseCells == Flow {}.coarseCells);
        CHECK(f.confidenceKnee == Flow {}.confidenceKnee && f.lambda == Flow {}.lambda);
    }

    // Values are bounded to each knob's range; ints round
    {
        Flow f;
        Trust t;
        CHECK(SetKnob("radius", 99.0f, f, t) && f.radius == 3);
        CHECK(SetKnob("cells", -5.0f, f, t) && f.coarseCells == 1);
        CHECK(SetKnob("still", 7.6f, f, t) && f.stillFrames == 8);
        CHECK(SetKnob("knee", 1.0f, f, t) && f.confidenceKnee == 0.05f);
        CHECK(SetKnob("history", 0.0f, f, t) && !f.useHistory);
        CHECK(!SetKnob("debugView", 1.0f, f, t));
    }

    // No key twice (a second member would never be set), every range the right way round, every default inside its
    // range (a Custom list of the defaults reads back as the defaults)
    {
        std::set<std::string> names;
        int count = 0;
        bool ordered = true, defaultsInside = true;
        Flow f;
        Trust t;
        VisitKnobs(f, t,
                   [&](const char* name, auto& member, float lo, float hi)
                   {
                       names.insert(name);
                       ++count;
                       ordered &= lo <= hi;
                       const float v = (float) member;
                       defaultsInside &= v >= lo && v <= hi;
                   });
        CHECK((int) names.size() == count);
        CHECK(ordered);
        CHECK(defaultsInside);
    }

    printf(g_failed == 0 ? "nr_motion_profiles_smoke: PASS\n" : "nr_motion_profiles_smoke: %d FAILED\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
