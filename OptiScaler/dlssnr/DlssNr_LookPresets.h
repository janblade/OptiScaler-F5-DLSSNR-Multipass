#pragma once

// The menu's look presets: one click sets everything about how DLSS-NR's edit looks. Plain data and rules, no config
// and no ImGui, so tests/nr_look_presets_smoke.cpp runs them; dlssnr/DlssNr_Menu.cpp reads the config into a Settings
// and writes one back.
//
// Each preset row on the menu owns its own settings, and its green highlight checks only those, so the rows never undo
// each other:
// - a look owns the per-pass model profiles, the curve, Detail and Colour strength, the skin / environment split and
//   the input style (Settings below);
// - the Multipass presets own the pass count; the Pre-SR / Post-SR tiers own placement and model resolution.
// A look never sets exposure, colour encoding or paper white: those belong to the game and the display, not to taste
// (Native look's brightness suits NBA 2K27 and is far too dark in RDR2).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "../shaders/dlssnr/DlssNr_InputStyles.h"

namespace DlssNrLook
{
// [DlssNr] ReversibleMode, the curve and how the answer is put back (Config.h)
inline constexpr uint32_t kCurveSoftKnee = 0;        // soft knee + composed (the default)
inline constexpr uint32_t kCurveComposed = 1;        // unclipped proxy + composed: the tiers' composition
inline constexpr uint32_t kCurveReplace = 2;         // pure-inverse replace
inline constexpr uint32_t kCurveReplaceBalanced = 4; // balanced curve, replace
inline constexpr uint32_t kCurveHlg = 5;             // HLG + composed
inline constexpr uint32_t kCurveLinear = 7;          // linear + composed: what game integrations hand the model

// The old single [DlssNr] InputStyle numbers: a look still sets one style (DlssNrInput::FromSingle)
inline constexpr uint32_t kInputOff = 0;
inline constexpr uint32_t kInputWatercolour = 1;
inline constexpr uint32_t kInputStructureHint = 2;
inline constexpr uint32_t kInputSmoothCanvas = 3;

inline bool IsReplace(uint32_t curve) { return curve == kCurveReplace || curve == kCurveReplaceBalanced; }

// What a tier leaves the curve at: composed always (Replace was the lower tiers' and is no longer), and a composed
// curve a look chose stays, so the look stays in effect after a tier.
inline uint32_t TierCurve(uint32_t current) { return IsReplace(current) ? kCurveComposed : current; }

// One pass's model profile ([DlssNr] Style, Intensity, LocalStructure, LocalTone, SkinStructure, AutoMask)
struct Profile
{
    uint32_t style = 0; // 0 Standard, 1 Natural, 2 Cinematic
    float intensity = 1.0f;
    float structure = 1.0f;
    float tone = 1.0f;
    float skin = -1.0f; // -1: follow Local structure
    bool autoMask = true;
};

// Everything a look owns. Values a look does not mention are the defaults below, so a look gives the same picture
// whatever was set before it.
struct Settings
{
    Profile pass1;
    std::optional<Profile> pass2; // absent: inherits pass 1 (Local tone 0 from pass 2 on, as PassProfiles.h has it)
    std::optional<Profile> pass3;
    bool laterOverrides = false; // any pass 4+ override set (a look sets none)
    uint32_t curve = kCurveSoftKnee;
    float detail = 1.0f;    // TransferStrength, the menu's Detail strength
    float colour = 1.0f;    // ColourStrength
    bool skinSplit = false; // SkinProtection: separate skin / environment controls
    bool skinTone = true;   // SkinToneEnabled
    float skinDetail = 1.0f;
    float skinColour = 1.0f;
    float envDetail = 1.0f;
    float envColour = 1.0f;
    DlssNrInput::Styles inputs; // the input styles card, Faces only included
};

// What the second-row looks (Faces, Painterly, Soft canvas, Structure) start from: the base's pass profiles and curve.
// Native is not offered: it is Linear, which bands in the darks under the looks' boosts, and it is NVIDIA's look as it is.
enum class Base
{
    Standard,
    Natural,
    Cinematic,
};

inline constexpr size_t kBaseCount = 3;

// What a second-row look changes on its base. An empty field keeps the base's value.
struct Modifier
{
    std::optional<float> skin;         // skin structure on every pass the base defines
    std::optional<float> structureCap; // Local structure capped at this on every pass (only ever lowered)
    std::optional<uint32_t> curve;     // a curve the look needs whatever the base uses
    std::optional<float> detail;
    std::optional<bool> skinSplit;
    std::optional<float> envDetail;
    std::optional<uint32_t> inputStyle;
    std::optional<float> inputStrength;
};

enum class Look
{
    Native,
    Natural,
    Cinematic,
    DetailOnly,
    Faces,
    Painterly,
    SoftCanvas,
    Structure,
};

struct Entry
{
    Look id;
    const char* name;
    const char* tip;
    bool experimental; // an input style: no effect above 100% model resolution or on the deferred path
    Settings settings; // for a modifier look: Compose(Base::Standard, modifier)
    bool isModifier;   // Faces, Painterly, Soft canvas, Structure: built on a Base
    Modifier modifier;
};

namespace detail
{
inline Settings Make(Profile pass1, uint32_t curve)
{
    Settings s;
    s.pass1 = pass1;
    s.curve = curve;
    return s;
}

inline Settings Native() { return Make({ 0, 1.0f, 1.0f, 0.0f, 0.75f, true }, kCurveLinear); }

// The tuned multipass profiles (were the Multipass presets'), pass 1 toned down from 1.8 to 1.4 (user, 2026-10-14)
inline Settings Natural()
{
    Settings s = Make({ 1, 1.4f, 1.4f, 1.4f, -1.0f, true }, kCurveHlg);
    s.pass2 = Profile { 1, 1.0f, 1.0f, 1.0f, -1.0f, true };
    s.pass3 = Profile { 1, 0.75f, 1.5f, 0.46f, 1.0f, true };
    return s;
}

inline Settings Cinematic() { return Make({ 2, 1.2f, 1.0f, 1.0f, -1.0f, true }, kCurveSoftKnee); }

// Colour strength 0: only brightness detail reaches the picture, so no hue can move
inline Settings DetailOnly()
{
    Settings s = Make({ 0, 0.8f, 1.0f, 1.0f, -1.0f, true }, kCurveSoftKnee);
    s.colour = 0.0f;
    return s;
}

inline Settings BaseSettings(Base base)
{
    switch (base)
    {
    case Base::Natural:
        return Natural();
    case Base::Cinematic:
        return Cinematic();
    default:
        return Make({ 0, 1.0f, 1.0f, 1.0f, -1.0f, true }, kCurveSoftKnee);
    }
}

inline Modifier Faces()
{
    Modifier m;
    m.skin = 1.0f;
    m.skinSplit = true;
    m.envDetail = 0.6f;
    return m;
}

inline Modifier Painterly()
{
    Modifier m;
    m.inputStyle = kInputWatercolour;
    m.detail = 1.5f; // user, 2026-10-14
    return m;
}

inline Modifier SoftCanvas()
{
    Modifier m;
    m.curve = kCurveHlg; // keeps the gradation in the darks under the boost
    m.inputStyle = kInputSmoothCanvas;
    m.detail = 2.0f;
    return m;
}

// The structure hint shows the model the picture with its fine contrast raised, so it paints more surface structure;
// Composed hands back only what it painted beyond that picture, so the boost itself never reaches the screen. The boost
// is on brightness with one scale per pixel, capped below clipping, so no hue moves. Strength 1.25: 2 was too strong in
// NBA 2K27 (user, 2026-10-15). The model's own Local structure is capped at 1 and Detail strength stays at 1, so the
// boost is not stacked on a second one (the model already over-sharpens mids and highlights, and Natural's 1.4 and 1.5
// would add to it); skin structure 0.75, the value games with built-in NR use, keeps pores from turning harsh; HLG
// keeps the gradation in the darks, where the boost would band on Linear.
inline Modifier Structure()
{
    Modifier m;
    m.skin = 0.75f;
    m.structureCap = 1.0f;
    m.curve = kCurveHlg;
    m.inputStyle = kInputStructureHint;
    m.inputStrength = 1.25f;
    return m;
}
} // namespace detail

inline void ApplyToPass(Profile& p, const Modifier& m)
{
    if (m.skin)
        p.skin = *m.skin;

    if (m.structureCap)
        p.structure = std::fmin(p.structure, *m.structureCap);
}

// The base's pass profiles and curve with the look's changes on top. Passes 2 and 3 are changed only when the base
// defines them (Natural's tuned ones); otherwise they inherit pass 1.
inline Settings Compose(Base base, const Modifier& m)
{
    Settings s = detail::BaseSettings(base);
    ApplyToPass(s.pass1, m);

    if (s.pass2)
        ApplyToPass(*s.pass2, m);

    if (s.pass3)
        ApplyToPass(*s.pass3, m);

    if (m.curve)
        s.curve = *m.curve;
    if (m.detail)
        s.detail = *m.detail;
    if (m.skinSplit)
        s.skinSplit = *m.skinSplit;
    if (m.envDetail)
        s.envDetail = *m.envDetail;
    if (m.inputStyle)
        s.inputs = DlssNrInput::FromSingle(*m.inputStyle, m.inputStrength.value_or(1.0f), false);

    return s;
}

inline const Entry kLooks[] = {
    { Look::Native, "Native", "NVIDIA's own look, as games with built-in NR use it: Linear curve, no local tone.",
      false, detail::Native(), false, {} },
    { Look::Natural, "Natural", "Natural style on the HLG curve. Passes 2 and 3 get their own tuned profiles.", false,
      detail::Natural(), false, {} },
    { Look::Cinematic, "Cinematic", "The model's Cinematic style.", false, detail::Cinematic(), false, {} },
    { Look::DetailOnly, "Detail only", "Adds detail and keeps the game's colours: Colour strength 0.", false,
      detail::DetailOnly(), false, {} },
    { Look::Faces, "Faces", "Full effect on skin, less on scenery (environment detail 0.6).", false,
      Compose(Base::Standard, detail::Faces()), true, detail::Faces() },
    { Look::Painterly, "Painterly", "Experimental: the picture is shown to the model as a watercolour, Detail strength 1.5.",
      true, Compose(Base::Standard, detail::Painterly()), true, detail::Painterly() },
    { Look::SoftCanvas, "Soft canvas",
      "Experimental: smooth canvas input on the HLG curve, Detail strength 2: smooth surfaces, strong detail.", true,
      Compose(Base::Standard, detail::SoftCanvas()), true, detail::SoftCanvas() },
    { Look::Structure, "Structure",
      "Experimental: the model is shown the picture with its fine contrast raised (brightness only, colours stay) and "
      "paints more surface structure. HLG curve, gentler on skin.",
      true, Compose(Base::Standard, detail::Structure()), true, detail::Structure() },
};

inline constexpr size_t kLookCount = sizeof(kLooks) / sizeof(kLooks[0]);

inline const Entry& Get(Look look) { return kLooks[(size_t) look]; }

inline bool Near(float a, float b) { return std::fabs(a - b) < 0.005f; }

inline bool Same(const Profile& a, const Profile& b)
{
    return a.style == b.style && Near(a.intensity, b.intensity) && Near(a.structure, b.structure) &&
           Near(a.tone, b.tone) && Near(a.skin, b.skin) && a.autoMask == b.autoMask;
}

inline bool Same(const std::optional<Profile>& a, const std::optional<Profile>& b)
{
    return a.has_value() == b.has_value() && (!a.has_value() || Same(*a, *b));
}

// Whether `now` is `look` in effect, for the button's highlight. Derived from the settings, never remembered.
inline bool Matches(const Settings& now, const Settings& look)
{
    return Same(now.pass1, look.pass1) && Same(now.pass2, look.pass2) && Same(now.pass3, look.pass3) &&
           now.laterOverrides == look.laterOverrides && now.curve == look.curve && Near(now.detail, look.detail) &&
           Near(now.colour, look.colour) && now.skinSplit == look.skinSplit && now.skinTone == look.skinTone &&
           Near(now.skinDetail, look.skinDetail) && Near(now.skinColour, look.skinColour) &&
           Near(now.envDetail, look.envDetail) && Near(now.envColour, look.envColour) &&
           DlssNrInput::Same(now.inputs, look.inputs);
}

// A second-row look on a base
inline Settings ComposeLook(Look look, Base base) { return Compose(base, Get(look).modifier); }

struct InEffect
{
    Look look;
    Base base;
};

// The second-row look in effect and the base it is built on. Derived from the settings, never remembered. At most one
// (base, look) pair matches (tests/nr_look_presets_smoke.cpp).
inline std::optional<InEffect> SecondRowInEffect(const Settings& now)
{
    for (const auto& e : kLooks)
    {
        if (!e.isModifier)
            continue;

        for (size_t b = 0; b < kBaseCount; ++b)
            if (Matches(now, Compose((Base) b, e.modifier)))
                return InEffect { e.id, (Base) b };
    }

    return std::nullopt;
}

inline std::optional<Base> DerivedBase(const Settings& now)
{
    const auto found = SecondRowInEffect(now);
    return found ? std::optional<Base> { found->base } : std::nullopt;
}
} // namespace DlssNrLook
