#pragma once

// The experimental input styles as the menu's one card has them: one way to flatten the picture first (watercolour or
// smooth canvas, never both: they do the same job), then the structure hint, each with its own strength, and Faces only
// for both. No D3D, no Config: a host test can include it.
//
// The passes run in that order, each on the one before's result: flattening first, so the structure hint raises what
// is left. Style numbers are the old single [DlssNr] InputStyle's (1 watercolour, 2 structure hint, 3 smooth canvas),
// which StyleMode and StyleName in DlssNr_Common.h take. (A texture hint, 4, was tried and removed on 2026-10-17: its
// pattern did not stay on the surfaces when the camera moved.)

#include <cmath>
#include <cstdint>

namespace DlssNrInput
{
inline constexpr uint32_t kStyleWatercolour = 1;
inline constexpr uint32_t kStyleStructureHint = 2;
inline constexpr uint32_t kStyleSmoothCanvas = 3;

// [DlssNr] InputFlatten
inline constexpr uint32_t kFlattenOff = 0;
inline constexpr uint32_t kFlattenWatercolour = 1;
inline constexpr uint32_t kFlattenSmoothCanvas = 2;

// The most a style's strength does: watercolour and smooth canvas are a blend (1), the structure hint's boost goes to 3x
inline float MostStrength(uint32_t style) { return style == kStyleStructureHint ? 3.0f : 1.0f; }

struct Styles
{
    uint32_t flatten = kFlattenOff;
    float flattenStrength = 1.0f;
    bool structure = false;
    float structureStrength = 1.0f;
    bool facesOnly = false;
};

inline uint32_t FlattenStyle(uint32_t flatten)
{
    return flatten == kFlattenWatercolour ? kStyleWatercolour : flatten == kFlattenSmoothCanvas ? kStyleSmoothCanvas : 0u;
}

struct Pass
{
    uint32_t style; // 1..3, as above
    float strength; // within MostStrength(style), above 0
};

// The passes to run, in order; how many (0..2)
inline int Passes(const Styles& s, Pass out[2])
{
    int n = 0;
    const auto add = [&](uint32_t style, float strength)
    {
        strength = std::fmin(strength, MostStrength(style));

        if (style != 0u && strength > 0.0f)
            out[n++] = { style, strength };
    };

    add(FlattenStyle(s.flatten), s.flattenStrength);

    if (s.structure)
        add(kStyleStructureHint, s.structureStrength);

    return n;
}

inline bool Any(const Styles& s)
{
    Pass passes[2];
    return Passes(s, passes) > 0;
}

// The old single [DlssNr] InputStyle (0..3; 4, the removed texture hint, reads as off) and InputStyleStrength: that one
// style, at that strength
inline Styles FromSingle(uint32_t style, float strength, bool facesOnly)
{
    Styles s;
    s.facesOnly = facesOnly;

    switch (style)
    {
    case kStyleWatercolour:
        s.flatten = kFlattenWatercolour;
        s.flattenStrength = strength;
        break;
    case kStyleSmoothCanvas:
        s.flatten = kFlattenSmoothCanvas;
        s.flattenStrength = strength;
        break;
    case kStyleStructureHint:
        s.structure = true;
        s.structureStrength = strength;
        break;
    default:
        break;
    }

    return s;
}

// What the old single key says for these styles, written beside the new keys for a build from before them: the first
// style that runs and its strength (0 when none does)
inline Pass ToSingle(const Styles& s)
{
    Pass passes[2];
    return Passes(s, passes) > 0 ? passes[0] : Pass { 0u, 1.0f };
}

inline bool NearStrength(float a, float b) { return std::fabs(a - b) < 0.005f; }

// The same picture: what runs, at what strength (a strength of a style that is off does not count)
inline bool Same(const Styles& a, const Styles& b)
{
    Pass pa[2], pb[2];
    const int na = Passes(a, pa), nb = Passes(b, pb);

    if (na != nb || (na > 0 && a.facesOnly != b.facesOnly))
        return false;

    for (int i = 0; i < na; ++i)
        if (pa[i].style != pb[i].style || !NearStrength(pa[i].strength, pb[i].strength))
            return false;

    return true;
}
} // namespace DlssNrInput
