#pragma once

// Parses a .cube LUT file (the Adobe/DaVinci/ReShade text format) into an in-memory 3D lattice. Pure
// and free of D3D/Vulkan types, like the other small DlssNr_*.h headers, so it can be exercised on the
// host (tests/nr_lut_cube_parse_smoke.cpp) with no GPU and no game.
//
// Only 3D LUTs are read. A 1D LUT (LUT_1D_SIZE) is a per-channel curve, a different shape of data this
// epic was never asked to apply -- rejected with its own message rather than silently misreading it as
// a point on the 3D grid.

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace DlssNr
{
// The .cube spec allows LUT_3D_SIZE 2..256. Capped lower here: every LUT anyone actually ships (17, 33,
// 65) is far below it, and the eventual GPU texture is size^3 texels, so the cap bounds that too.
constexpr int kMaxLutSize = 128;

struct Lut3D
{
    int size = 0; // the grid is size x size x size
    float domainMin[3] = { 0.0f, 0.0f, 0.0f };
    float domainMax[3] = { 1.0f, 1.0f, 1.0f };

    // size*size*size*3 floats, one RGB triple per lattice point, in the file's own order -- red fastest,
    // then green, then blue (the .cube spec's order; lattice point i is r = i % size, g = (i / size) % size,
    // b = i / (size * size)). Trilinear sampling of this is a GPU-side concern (Story 2/3), not here.
    std::vector<float> rgb;
};

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
} // namespace

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
    bool sawLut1D = false;
    int size = 0;
    float domainMin[3] = { 0.0f, 0.0f, 0.0f };
    float domainMax[3] = { 1.0f, 1.0f, 1.0f };
    std::vector<float> rgb;

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
            continue;

        const std::string keyword = UpperCopy(first);

        if (keyword == "TITLE")
            continue; // the rest of the line is a quoted title, nothing this feature reads

        if (keyword == "LUT_1D_SIZE")
        {
            sawLut1D = true;
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

        // A data row for an unsupported 1D table: already flagged above, nothing more to learn from its
        // content, and parsing it here would misreport the reason as "before LUT_3D_SIZE" instead of
        // the clearer 1D message the end-of-file check gives.
        if (sawLut1D && !sawSize)
            continue;

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

        if (!sawSize)
        {
            *error = "a data row appeared before LUT_3D_SIZE (line " + std::to_string(lineNumber) + ")";
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

    if (!sawSize)
    {
        *error = sawLut1D ? "this is a 1D LUT (LUT_1D_SIZE); only 3D LUTs (LUT_3D_SIZE) can be applied here"
                          : "no LUT_3D_SIZE found";
        return false;
    }

    const size_t expected = (size_t) size * size * size * 3;
    if (rgb.size() != expected)
    {
        *error = "expected " + std::to_string(expected / 3) + " data rows for LUT_3D_SIZE " + std::to_string(size) +
                 ", found " + std::to_string(rgb.size() / 3);
        return false;
    }

    out->size = size;
    out->domainMin[0] = domainMin[0];
    out->domainMin[1] = domainMin[1];
    out->domainMin[2] = domainMin[2];
    out->domainMax[0] = domainMax[0];
    out->domainMax[1] = domainMax[1];
    out->domainMax[2] = domainMax[2];
    out->rgb = std::move(rgb);
    return true;
}
} // namespace DlssNr
