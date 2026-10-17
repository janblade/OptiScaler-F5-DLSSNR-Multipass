// Host test for OptiScaler/dlssnr/DlssNr_LookPresets.h: the menu's look presets.
// cl /std:c++20 /EHsc /W4 tests/nr_look_presets_smoke.cpp
#include "../OptiScaler/dlssnr/DlssNr_LookPresets.h"

#include <cstdio>
#include <cstring>

using namespace DlssNrLook;

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

int main()
{
    CHECK(kLookCount == 8);

    // kLooks is indexed by Look (Get)
    for (size_t i = 0; i < kLookCount; ++i)
        CHECK((size_t) kLooks[i].id == i);

    // Every look reads as itself, and as no other (two looks never show green together)
    for (size_t i = 0; i < kLookCount; ++i)
    {
        CHECK(Matches(kLooks[i].settings, kLooks[i].settings));

        for (size_t j = 0; j < kLookCount; ++j)
            if (i != j)
                CHECK(!Matches(kLooks[i].settings, kLooks[j].settings));

        CHECK(kLooks[i].name != nullptr && kLooks[i].name[0] != '\0');
        CHECK(kLooks[i].tip != nullptr && kLooks[i].tip[0] != '\0');
    }

    // Any owned value changed: the look is no longer in effect
    {
        const Settings& base = kLooks[0].settings;
        auto changed = [&](auto edit)
        {
            Settings s = base;
            edit(s);
            return !Matches(s, base);
        };

        CHECK(changed([](Settings& s) { s.pass1.style = 2; }));
        CHECK(changed([](Settings& s) { s.pass1.intensity += 0.1f; }));
        CHECK(changed([](Settings& s) { s.pass1.structure += 0.1f; }));
        CHECK(changed([](Settings& s) { s.pass1.tone += 0.1f; }));
        CHECK(changed([](Settings& s) { s.pass1.skin += 0.1f; }));
        CHECK(changed([](Settings& s) { s.pass1.autoMask = !s.pass1.autoMask; }));
        CHECK(changed([](Settings& s) { s.pass2 = Profile {}; }));
        CHECK(changed([](Settings& s) { s.laterOverrides = true; }));
        CHECK(changed([](Settings& s) { s.curve = 5; }));
        CHECK(changed([](Settings& s) { s.detail = 1.5f; }));
        CHECK(changed([](Settings& s) { s.colour = 0.5f; }));
        CHECK(changed([](Settings& s) { s.skinSplit = true; }));
        CHECK(changed([](Settings& s) { s.skinTone = false; }));
        CHECK(changed([](Settings& s) { s.skinDetail = 0.5f; }));
        CHECK(changed([](Settings& s) { s.skinColour = 0.5f; }));
        CHECK(changed([](Settings& s) { s.envDetail = 0.5f; }));
        CHECK(changed([](Settings& s) { s.envColour = 0.5f; }));
        CHECK(changed([](Settings& s) { s.inputs.flatten = DlssNrInput::kFlattenSmoothCanvas; }));
        CHECK(changed([](Settings& s) { s.inputs.structure = true; }));
        CHECK(changed([](Settings& s) { s.inputs = DlssNrInput::FromSingle(kInputStructureHint, 1.0f, true); }));
        CHECK(changed([](Settings& s) { s.inputs = DlssNrInput::FromSingle(kInputStructureHint, 0.5f, false); }));
        // A strength or Faces only with no style on changes nothing
        {
            Settings off = base;
            off.inputs.structureStrength = 0.5f;
            off.inputs.facesOnly = true;
            CHECK(Matches(off, base));
        }

        // Within the sliders' rounding it still matches
        Settings s = base;
        s.pass1.intensity += 0.001f;
        CHECK(Matches(s, base));
    }

    // The values the user chose
    {
        const Settings& natural = Get(Look::Natural).settings;
        CHECK(natural.pass1.style == 1 && natural.pass1.intensity == 1.4f && natural.pass1.structure == 1.4f &&
              natural.pass1.tone == 1.4f);
        CHECK(natural.pass2.has_value() && natural.pass3.has_value());
        CHECK(natural.curve == kCurveHlg);

        const Settings& native = Get(Look::Native).settings;
        CHECK(native.curve == kCurveLinear && native.pass1.tone == 0.0f && native.pass1.skin == 0.75f);
        CHECK(!native.pass2.has_value() && !native.pass3.has_value());

        const Settings& soft = Get(Look::SoftCanvas).settings;
        CHECK(soft.inputs.flatten == DlssNrInput::kFlattenSmoothCanvas && soft.curve == kCurveHlg && soft.detail == 2.0f);

        CHECK(Get(Look::DetailOnly).settings.colour == 0.0f);
        CHECK(Get(Look::Painterly).settings.inputs.flatten == DlssNrInput::kFlattenWatercolour &&
              Get(Look::Painterly).settings.detail == 1.5f);
        CHECK(Get(Look::Faces).settings.skinSplit && Get(Look::Faces).settings.envDetail < 1.0f);
        CHECK(Get(Look::Cinematic).settings.pass1.style == 2);

        // Structure: the structure hint input at 1.25 (2 was too strong in NBA), the model's own structure and Detail strength
        // left at 1 so the boost is not stacked, skin structure 0.75, HLG for the darks
        const Settings& structure = Get(Look::Structure).settings;
        CHECK(structure.inputs.structure && structure.inputs.structureStrength == 1.25f && structure.inputs.flatten == 0u);
        CHECK(structure.curve == kCurveHlg && structure.detail == 1.0f && structure.colour == 1.0f);
        CHECK(structure.pass1.style == 0 && structure.pass1.structure == 1.0f && structure.pass1.skin == 0.75f);
        CHECK(!structure.inputs.facesOnly && !structure.skinSplit);

        CHECK(Get(Look::Painterly).experimental && Get(Look::SoftCanvas).experimental &&
              Get(Look::Structure).experimental);
        CHECK(!Get(Look::Native).experimental && !Get(Look::Natural).experimental);
    }

    // Built on Standard is today's four second-row looks, bit for bit. The literals are the old definitions, copied
    // before the split into base + modifier.
    {
        auto lit = [](Profile p1, uint32_t curve)
        {
            Settings s;
            s.pass1 = p1;
            s.curve = curve;
            return s;
        };
        auto exact = [](const Settings& a, const Settings& b)
        {
            return a.pass1.style == b.pass1.style && a.pass1.intensity == b.pass1.intensity &&
                   a.pass1.structure == b.pass1.structure && a.pass1.tone == b.pass1.tone &&
                   a.pass1.skin == b.pass1.skin && a.pass1.autoMask == b.pass1.autoMask &&
                   a.pass2.has_value() == b.pass2.has_value() && a.pass3.has_value() == b.pass3.has_value() &&
                   a.laterOverrides == b.laterOverrides && a.curve == b.curve && a.detail == b.detail &&
                   a.colour == b.colour && a.skinSplit == b.skinSplit && a.skinTone == b.skinTone &&
                   a.skinDetail == b.skinDetail && a.skinColour == b.skinColour && a.envDetail == b.envDetail &&
                   a.envColour == b.envColour && DlssNrInput::Same(a.inputs, b.inputs) &&
                   a.inputs.facesOnly == b.inputs.facesOnly;
        };

        Settings faces = lit({ 0, 1.0f, 1.0f, 1.0f, 1.0f, true }, kCurveSoftKnee);
        faces.skinSplit = true;
        faces.envDetail = 0.6f;

        Settings painterly = lit({ 0, 1.0f, 1.0f, 1.0f, -1.0f, true }, kCurveSoftKnee);
        painterly.inputs = DlssNrInput::FromSingle(kInputWatercolour, 1.0f, false);
        painterly.detail = 1.5f;

        Settings soft = lit({ 0, 1.0f, 1.0f, 1.0f, -1.0f, true }, kCurveHlg);
        soft.inputs = DlssNrInput::FromSingle(kInputSmoothCanvas, 1.0f, false);
        soft.detail = 2.0f;

        Settings structure = lit({ 0, 1.0f, 1.0f, 1.0f, 0.75f, true }, kCurveHlg);
        structure.inputs = DlssNrInput::FromSingle(kInputStructureHint, 1.25f, false);

        CHECK(exact(Compose(Base::Standard, Get(Look::Faces).modifier), faces));
        CHECK(exact(Compose(Base::Standard, Get(Look::Painterly).modifier), painterly));
        CHECK(exact(Compose(Base::Standard, Get(Look::SoftCanvas).modifier), soft));
        CHECK(exact(Compose(Base::Standard, Get(Look::Structure).modifier), structure));
        CHECK(exact(Get(Look::Faces).settings, faces));
        CHECK(exact(Get(Look::Painterly).settings, painterly));
        CHECK(exact(Get(Look::SoftCanvas).settings, soft));
        CHECK(exact(Get(Look::Structure).settings, structure));
    }

    // Built on Natural or Cinematic
    {
        const Settings facesNat = ComposeLook(Look::Faces, Base::Natural);
        CHECK(facesNat.pass1.style == 1 && facesNat.pass1.intensity == 1.4f && facesNat.pass1.structure == 1.4f &&
              facesNat.pass1.tone == 1.4f);
        CHECK(facesNat.pass1.skin == 1.0f && facesNat.pass2 && facesNat.pass2->skin == 1.0f && facesNat.pass3 &&
              facesNat.pass3->skin == 1.0f);
        CHECK(facesNat.pass2->style == 1 && facesNat.pass3->structure == 1.5f && facesNat.pass3->tone == 0.46f);
        CHECK(facesNat.skinSplit && facesNat.envDetail == 0.6f && facesNat.curve == kCurveHlg);

        const Settings structNat = ComposeLook(Look::Structure, Base::Natural);
        CHECK(structNat.pass1.structure <= 1.0f && structNat.pass2->structure <= 1.0f &&
              structNat.pass3->structure <= 1.0f);
        CHECK(structNat.curve == kCurveHlg && structNat.inputs.structure && structNat.inputs.structureStrength == 1.25f);
        CHECK(structNat.pass1.skin == 0.75f && structNat.pass3->skin == 0.75f);
        // The cap only lowers: Natural's intensity and tone stay
        CHECK(structNat.pass1.intensity == 1.4f && structNat.pass1.tone == 1.4f && structNat.pass3->tone == 0.46f);

        const Settings paintCin = ComposeLook(Look::Painterly, Base::Cinematic);
        CHECK(paintCin.pass1.style == 2 && paintCin.pass1.intensity == 1.2f && paintCin.curve == kCurveSoftKnee &&
              paintCin.detail == 1.5f && paintCin.inputs.flatten == DlssNrInput::kFlattenWatercolour);

        CHECK(ComposeLook(Look::SoftCanvas, Base::Cinematic).curve == kCurveHlg);
        CHECK(ComposeLook(Look::SoftCanvas, Base::Natural).curve == kCurveHlg);
        CHECK(ComposeLook(Look::Faces, Base::Cinematic).curve == kCurveSoftKnee);
        CHECK(ComposeLook(Look::Faces, Base::Cinematic).pass1.style == 2);

        // Only the second-row looks are modifiers
        for (size_t i = 0; i < kLookCount; ++i)
            CHECK(kLooks[i].isModifier == (i >= (size_t) Look::Faces));
    }

    // At most one green button: no two (base, look) pairs are equal, and none equals a first-row look
    for (size_t i = 0; i < kLookCount; ++i)
    {
        if (!kLooks[i].isModifier)
            continue;

        for (size_t b = 0; b < kBaseCount; ++b)
        {
            const Settings a = ComposeLook(kLooks[i].id, (Base) b);

            for (size_t j = 0; j < kLookCount; ++j)
                if (!kLooks[j].isModifier)
                    CHECK(!Matches(a, kLooks[j].settings));

            for (size_t k = 0; k < kLookCount; ++k)
                for (size_t c = 0; c < kBaseCount; ++c)
                    if (kLooks[k].isModifier && (k != i || c != b))
                        CHECK(!Matches(a, ComposeLook(kLooks[k].id, (Base) c)));
        }
    }

    // DerivedBase: the base of the matching pair, or none
    {
        for (size_t i = 0; i < kLookCount; ++i)
        {
            if (!kLooks[i].isModifier)
            {
                CHECK(!DerivedBase(kLooks[i].settings).has_value());
                continue;
            }

            for (size_t b = 0; b < kBaseCount; ++b)
            {
                const auto got = DerivedBase(ComposeLook(kLooks[i].id, (Base) b));
                CHECK(got.has_value() && *got == (Base) b);
            }
        }

        Settings edited = ComposeLook(Look::Faces, Base::Natural);
        edited.detail = 1.3f;
        CHECK(!DerivedBase(edited).has_value());
    }

    // Tiers make the composition composed and leave a composed curve alone, so a look stays in effect after one
    CHECK(TierCurve(kCurveReplace) == kCurveComposed);
    CHECK(TierCurve(kCurveReplaceBalanced) == kCurveComposed);

    for (uint32_t curve = 0; curve <= 7; ++curve)
        CHECK(IsReplace(curve) || TierCurve(curve) == curve);

    for (size_t i = 0; i < kLookCount; ++i)
    {
        CHECK(!IsReplace(kLooks[i].settings.curve));
        Settings after = kLooks[i].settings;
        after.curve = TierCurve(after.curve);
        CHECK(Matches(after, kLooks[i].settings));
    }

    if (fails != 0)
    {
        printf("look presets: %d failure(s)\n", fails);
        return 1;
    }

    printf("look presets: ok\n");
    return 0;
}
