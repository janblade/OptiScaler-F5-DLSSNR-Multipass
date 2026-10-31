#pragma once

// Parses a .cube LUT file (the Adobe/DaVinci/ReShade text format) into an in-memory 3D lattice. Pure
// and free of D3D/Vulkan types, like the other small DlssNr_*.h headers, so it can be exercised on the
// host (tests/nr_lut_cube_parse_smoke.cpp) with no GPU and no game.
//
// 3D LUTs, and 1D shapers (LUT_1D_SIZE, the Resolve layout: a per-channel curve over LUT_1D_INPUT_RANGE, its rows
// before the 3D rows) alone or in front of a 3D table, as view-transform packs ship them (post-NR LUT epic Story 5,
// memory/plans/2026-10-31-nr-lut-chain.md). A file with a shaper is baked here, once, into a plain 3D lattice over the
// display 0..1 domain the LUT passes feed it, so every consumer (the Input LUT, the post-NR stack, the GPU passes)
// keeps handling one shape of data.

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace DlssNr
{
// The .cube spec allows LUT_3D_SIZE 2..256. Capped lower here: every LUT anyone actually ships (17, 33,
// 65) is far below it, and the eventual GPU texture is size^3 texels, so the cap bounds that too.
constexpr int kMaxLutSize = 128;

// LUT_1D_SIZE cap: shapers ship at 1024..65536 entries; a 1D table is small however long.
constexpr int kMaxShaperSize = 65536;

// Lattice size a 1D shaper (+ 3D) file is baked to: the shaper is a steep curve, so the finest lattice the GPU passes
// are given anywhere (DlssNr_LutChain.h kChainMaxSize).
constexpr int kShaperBakeSize = 65;

struct Shaper1D;

struct Lut3D
{
    int size = 0; // the grid is size x size x size
    float domainMin[3] = { 0.0f, 0.0f, 0.0f };
    float domainMax[3] = { 1.0f, 1.0f, 1.0f };

    // size*size*size*3 floats, one RGB triple per lattice point, in the file's own order -- red fastest,
    // then green, then blue (the .cube spec's order; lattice point i is r = i % size, g = (i / size) % size,
    // b = i / (size * size)). Trilinear sampling of this is a GPU-side concern (Story 2/3), not here.
    std::vector<float> rgb;

    // When the file had a 1D shaper: its length and the file's own 3D size (0 for a 1D-only file), for the status
    // line. `rgb` is then the bake (kShaperBakeSize, domain 0..1), not the file's table.
    int shaperSize = 0;
    int fileSize = 0;

    // The file's shaper and its own 3D table (null without a shaper), kept so a layer handed a converted signal (scene
    // light above 1, DlssNr_LutSpaces.h) reads the file itself instead of the 0..1 bake.
    std::shared_ptr<const Shaper1D> shaper;
    std::shared_ptr<const Lut3D> fileTable;

    // The header's "# Input Gamma:", "# Input Color Space:" and Output lines (as view-transform packs write them), for
    // the look input's Auto. Empty when absent.
    std::string inGamma, inGamut, outGamma, outGamut;
};

// Trilinear sample of `lut` at the display value `in`, with the shader's domain clamp (dlssnr_lut.hlsl). Lattice point
// i sits at uvw = i / (size - 1); red is the fastest axis (see Lut3D::rgb).
inline void SampleLut(const Lut3D& lut, const float in[3], float out[3])
{
    const int n = lut.size;
    const float span = (float) (n - 1);
    float f[3];
    int i0[3];
    int i1[3];

    for (int c = 0; c < 3; ++c)
    {
        const float lo = lut.domainMin[c];
        const float width = std::max(lut.domainMax[c] - lo, 1e-6f);
        const float clamped = std::clamp(in[c], lut.domainMin[c], lut.domainMax[c]);
        const float p = std::clamp((clamped - lo) / width, 0.0f, 1.0f) * span;
        i0[c] = std::min((int) std::floor(p), n - 1);
        i1[c] = std::min(i0[c] + 1, n - 1);
        f[c] = p - (float) i0[c];
    }

    const auto at = [&](int r, int g, int b, int c) { return lut.rgb[(((size_t) b * n + g) * n + r) * 3 + c]; };

    for (int c = 0; c < 3; ++c)
    {
        const float c00 = at(i0[0], i0[1], i0[2], c) * (1 - f[0]) + at(i1[0], i0[1], i0[2], c) * f[0];
        const float c10 = at(i0[0], i1[1], i0[2], c) * (1 - f[0]) + at(i1[0], i1[1], i0[2], c) * f[0];
        const float c01 = at(i0[0], i0[1], i1[2], c) * (1 - f[0]) + at(i1[0], i0[1], i1[2], c) * f[0];
        const float c11 = at(i0[0], i1[1], i1[2], c) * (1 - f[0]) + at(i1[0], i1[1], i1[2], c) * f[0];
        const float c0 = c00 * (1 - f[1]) + c10 * f[1];
        const float c1 = c01 * (1 - f[1]) + c11 * f[1];
        out[c] = c0 * (1 - f[2]) + c1 * f[2];
    }
}

// A 1D shaper: per channel, `size` points evenly spread over [min, max], linear between them, clamped at the ends
// (how Resolve and OCIO read LUT_1D_INPUT_RANGE).
struct Shaper1D
{
    int size = 0;
    float min = 0.0f;
    float max = 1.0f;
    std::vector<float> rgb; // size triples

    float Apply(float x, int c) const
    {
        const float p = std::clamp((x - min) / std::max(max - min, 1e-12f), 0.0f, 1.0f) * (float) (size - 1);
        const int i = std::min((int) p, size - 2);
        const float f = p - (float) i;
        return rgb[(size_t) i * 3 + c] * (1.0f - f) + rgb[(size_t) (i + 1) * 3 + c] * f;
    }
};

// The file itself at `in`: through its shaper and own table when it has one (any input, not only 0..1), else the
// lattice. What a layer with a converted input reads (DlssNr_LutChain.h).
inline void SampleLutFile(const Lut3D& lut, const float in[3], float out[3])
{
    if (lut.shaper == nullptr)
    {
        SampleLut(lut, in, out);
        return;
    }

    float shaped[3];
    for (int c = 0; c < 3; ++c)
        shaped[c] = lut.shaper->Apply(in[c], c);
    if (lut.fileTable != nullptr && lut.fileTable->size >= 2)
        SampleLut(*lut.fileTable, shaped, out);
    else
        std::copy(shaped, shaped + 3, out);
}

// Bakes shaper -> table into a plain lattice over display 0..1 (the LUT passes' input domain).
inline Lut3D BakeShaper(const Shaper1D& shaper, const Lut3D& table)
{
    Lut3D out;
    const int n = kShaperBakeSize;
    out.size = n;
    out.rgb.resize((size_t) n * n * n * 3);

    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r)
            {
                const int idx[3] = { r, g, b };
                float shaped[3];
                for (int c = 0; c < 3; ++c)
                    shaped[c] = shaper.Apply((float) idx[c] / (float) (n - 1), c);

                float* dst = &out.rgb[(((size_t) b * n + g) * n + r) * 3];
                if (table.size >= 2)
                    SampleLut(table, shaped, dst);
                else
                    std::copy(shaped, shaped + 3, dst);
            }

    out.shaperSize = shaper.size;
    out.fileSize = table.size;
    return out;
}

namespace
{
inline std::string UpperCopy(const std::string& s)
{
    std::string u = s;
    std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c) { return (char) std::toupper(c); });
    return u;
}

// std::stof/strtof throw or read a locale-dependent decimal point; std::from_chars does neither and
// reports a partial match (trailing garbage) as failure, which a hand-edited .cube file can easily have.
inline bool TryParseFloat(const std::string& token, float* out)
{
    const auto result = std::from_chars(token.data(), token.data() + token.size(), *out);
    return result.ec == std::errc {} && result.ptr == token.data() + token.size();
}

inline std::string Trimmed(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r");
    const size_t z = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, z - a + 1);
}

// "#  Input Gamma: Arri LogC4" -> hints->inGamma = "Arri LogC4"; the colour/color spelling and case do not matter.
inline void ReadHeaderHint(const std::string& line, Lut3D* hints)
{
    const size_t hash = line.find('#');
    const size_t colon = line.find(':', hash);
    if (hash == std::string::npos || colon == std::string::npos)
        return;

    const std::string key = UpperCopy(Trimmed(line.substr(hash + 1, colon - hash - 1)));
    const std::string value = Trimmed(line.substr(colon + 1));
    if (key == "INPUT GAMMA")
        hints->inGamma = value;
    else if (key == "INPUT COLOR SPACE" || key == "INPUT COLOUR SPACE")
        hints->inGamut = value;
    else if (key == "OUTPUT GAMMA")
        hints->outGamma = value;
    else if (key == "OUTPUT COLOR SPACE" || key == "OUTPUT COLOUR SPACE")
        hints->outGamut = value;
}

inline void CopyHeaderHints(const Lut3D& from, Lut3D* to)
{
    to->inGamma = from.inGamma;
    to->inGamut = from.inGamut;
    to->outGamma = from.outGamma;
    to->outGamut = from.outGamut;
}
} // namespace

// "33^3", or for a file with a shaper "1D 4096 + 33^3" (what the file holds, not the bake).
inline std::string DescribeLutSize(const Lut3D& lut)
{
    if (lut.shaperSize == 0)
        return std::to_string(lut.size) + "^3";
    return "1D " + std::to_string(lut.shaperSize) +
           (lut.fileSize >= 2 ? " + " + std::to_string(lut.fileSize) + "^3" : std::string());
}

// Reads a .cube file into *out. False on any problem, with *error set to a specific, user-facing reason
// (shown in the menu's status line, DlssNr_Menu.cpp) rather than a generic "failed to load".
inline bool ParseCube(const std::string& path, Lut3D* out, std::string* error)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        *error = "could not open " + path;
        return false;
    }

    bool sawSize = false;
    Shaper1D shaper;
    int size = 0;
    float domainMin[3] = { 0.0f, 0.0f, 0.0f };
    float domainMax[3] = { 1.0f, 1.0f, 1.0f };
    std::vector<float> rgb;
    Lut3D hints; // only its header strings

    std::string line;
    unsigned int lineNumber = 0;

    while (std::getline(file, line))
    {
        ++lineNumber;

        std::istringstream iss(line);
        std::string first;
        if (!(iss >> first))
            continue; // blank line

        if (first[0] == '#')
        {
            ReadHeaderHint(line, &hints);
            continue;
        }

        const std::string keyword = UpperCopy(first);

        if (keyword == "TITLE")
            continue; // the rest of the line is a quoted title, nothing this feature reads

        if (keyword == "LUT_1D_SIZE")
        {
            int n = 0;
            if (shaper.size != 0 || !(iss >> n) || n < 2 || n > kMaxShaperSize)
            {
                *error = "LUT_1D_SIZE given twice, without its number or out of range (2.." +
                         std::to_string(kMaxShaperSize) + ") (line " + std::to_string(lineNumber) + ")";
                return false;
            }
            shaper.size = n;
            shaper.rgb.reserve((size_t) n * 3);
            continue;
        }

        if (keyword == "LUT_1D_INPUT_RANGE" || keyword == "LUT_3D_INPUT_RANGE")
        {
            float lo = 0.0f, hi = 0.0f;
            if (!(iss >> lo >> hi) || !(hi > lo))
            {
                *error = keyword + " needs two increasing numbers (line " + std::to_string(lineNumber) + ")";
                return false;
            }

            if (keyword == "LUT_1D_INPUT_RANGE")
            {
                shaper.min = lo;
                shaper.max = hi;
            }
            else
                for (int c = 0; c < 3; ++c)
                {
                    domainMin[c] = lo;
                    domainMax[c] = hi;
                }
            continue;
        }

        if (keyword == "LUT_3D_SIZE")
        {
            if (sawSize)
            {
                *error = "LUT_3D_SIZE given twice";
                return false;
            }

            int n = 0;
            if (!(iss >> n))
            {
                *error = "LUT_3D_SIZE is missing its number (line " + std::to_string(lineNumber) + ")";
                return false;
            }

            if (n < 2 || n > kMaxLutSize)
            {
                *error =
                    "LUT_3D_SIZE " + std::to_string(n) + " is out of range (2.." + std::to_string(kMaxLutSize) + ")";
                return false;
            }

            size = n;
            sawSize = true;
            rgb.reserve((size_t) size * size * size * 3);
            continue;
        }

        if (keyword == "DOMAIN_MIN" || keyword == "DOMAIN_MAX")
        {
            float v[3];
            if (!(iss >> v[0] >> v[1] >> v[2]))
            {
                *error = keyword + " needs three numbers (line " + std::to_string(lineNumber) + ")";
                return false;
            }

            float(&target)[3] = keyword == "DOMAIN_MIN" ? domainMin : domainMax;
            target[0] = v[0];
            target[1] = v[1];
            target[2] = v[2];
            continue;
        }

        // Anything else is expected to be a data row: three floats, the first token already read.
        float r = 0.0f;
        if (!TryParseFloat(first, &r))
        {
            *error = "could not parse line " + std::to_string(lineNumber) + ": " + line;
            return false;
        }

        float g = 0.0f, b = 0.0f;
        if (!(iss >> g >> b))
        {
            *error = "data row on line " + std::to_string(lineNumber) + " needs three numbers";
            return false;
        }

        // The shaper's rows come first (the Resolve layout), then the 3D table's.
        if (shaper.size != 0 && shaper.rgb.size() < (size_t) shaper.size * 3)
        {
            shaper.rgb.push_back(r);
            shaper.rgb.push_back(g);
            shaper.rgb.push_back(b);
            continue;
        }

        if (!sawSize)
        {
            *error = shaper.size != 0
                         ? "more data rows than LUT_1D_SIZE " + std::to_string(shaper.size) +
                               " calls for, and no LUT_3D_SIZE (line " + std::to_string(lineNumber) + ")"
                         : "a data row appeared before LUT_3D_SIZE (line " + std::to_string(lineNumber) + ")";
            return false;
        }

        const size_t expected = (size_t) size * size * size * 3;
        if (rgb.size() >= expected)
        {
            *error = "more data rows than LUT_3D_SIZE " + std::to_string(size) + " calls for";
            return false;
        }

        rgb.push_back(r);
        rgb.push_back(g);
        rgb.push_back(b);
    }

    if (shaper.size != 0 && shaper.rgb.size() != (size_t) shaper.size * 3)
    {
        *error = "expected " + std::to_string(shaper.size) + " data rows for LUT_1D_SIZE " +
                 std::to_string(shaper.size) + ", found " + std::to_string(shaper.rgb.size() / 3);
        return false;
    }

    if (!sawSize && shaper.size == 0)
    {
        *error = "no LUT_3D_SIZE or LUT_1D_SIZE found";
        return false;
    }

    const size_t expected = (size_t) size * size * size * 3;
    if (sawSize && rgb.size() != expected)
    {
        *error = "expected " + std::to_string(expected / 3) + " data rows for LUT_3D_SIZE " + std::to_string(size) +
                 ", found " + std::to_string(rgb.size() / 3);
        return false;
    }

    if (shaper.size != 0)
    {
        Lut3D table;
        if (sawSize)
        {
            table.size = size;
            std::copy(domainMin, domainMin + 3, table.domainMin);
            std::copy(domainMax, domainMax + 3, table.domainMax);
            table.rgb = std::move(rgb);
        }
        *out = BakeShaper(shaper, table);
        if (sawSize)
            out->fileTable = std::make_shared<const Lut3D>(std::move(table));
        out->shaper = std::make_shared<const Shaper1D>(std::move(shaper));
        CopyHeaderHints(hints, out);
        return true;
    }

    out->size = size;
    out->domainMin[0] = domainMin[0];
    out->domainMin[1] = domainMin[1];
    out->domainMin[2] = domainMin[2];
    out->domainMax[0] = domainMax[0];
    out->domainMax[1] = domainMax[1];
    out->domainMax[2] = domainMax[2];
    out->rgb = std::move(rgb);
    CopyHeaderHints(hints, out);
    return true;
}
} // namespace DlssNr
