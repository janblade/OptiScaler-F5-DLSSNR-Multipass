// Host test for OptiScaler/shaders/dlssnr/DlssNr_InputStyles.h: the input styles card's passes run in order (flatten,
// then structure hint) at bounded strengths, the old single InputStyle maps to the card and back, and Same counts only
// what runs.
//   cl /std:c++20 /EHsc /W4 tests\nr_input_styles_smoke.cpp

#include "../OptiScaler/shaders/dlssnr/DlssNr_InputStyles.h"

#include <cstdio>

using namespace DlssNrInput;

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

int main()
{
    Pass p[2];

    // Nothing on: no passes
    CHECK(Passes(Styles {}, p) == 0 && !Any(Styles {}));

    // Both on: flatten, then structure, each strength bounded by its style
    {
        Styles s;
        s.flatten = kFlattenSmoothCanvas;
        s.flattenStrength = 5.0f;
        s.structure = true;
        s.structureStrength = 9.0f;
        CHECK(Passes(s, p) == 2);
        CHECK(p[0].style == kStyleSmoothCanvas && p[0].strength == 1.0f);
        CHECK(p[1].style == kStyleStructureHint && p[1].strength == 3.0f);
    }

    // A style at strength 0 does not run
    {
        Styles s;
        s.flatten = kFlattenWatercolour;
        s.flattenStrength = 0.0f;
        s.structure = true;
        CHECK(Passes(s, p) == 1 && p[0].style == kStyleStructureHint);
    }

    // The old single key: each style maps to the card and back; 4 (the removed texture hint) and anything else is off
    for (uint32_t style = 1; style <= 3; ++style)
    {
        const Styles s = FromSingle(style, 0.75f, true);
        CHECK(Passes(s, p) == 1 && p[0].style == style && p[0].strength == 0.75f && s.facesOnly);
        const Pass back = ToSingle(s);
        CHECK(back.style == style && back.strength == 0.75f);
    }
    CHECK(!Any(FromSingle(0, 1.0f, false)) && !Any(FromSingle(4, 1.0f, false)) && !Any(FromSingle(7, 1.0f, false)));
    CHECK(ToSingle(Styles {}).style == 0u);

    // Same: what runs and how strong; a strength or Faces only with nothing on does not count
    {
        Styles a, b;
        b.structureStrength = 2.0f;
        b.facesOnly = true;
        CHECK(Same(a, b));
        a.structure = b.structure = true;
        CHECK(!Same(a, b)); // 1.0 against 2.0, and faces only
        a.structureStrength = 2.0f;
        CHECK(!Same(a, b)); // faces only
        a.facesOnly = true;
        CHECK(Same(a, b));
        b.flatten = kFlattenWatercolour;
        CHECK(!Same(a, b));
    }

    printf(g_failed == 0 ? "nr_input_styles_smoke: PASS\n" : "nr_input_styles_smoke: %d FAILED\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
