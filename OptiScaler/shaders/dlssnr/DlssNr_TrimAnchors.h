#pragma once

// Trim anchors for the exposure-based white point sources.
//
// The white point those sources produce is PreExposure / exposure, times a Trim the user picks. A
// single Trim fits one lighting condition; a value that suits a bright exterior can be wrong in a dark
// interior. An anchor says "at this base white point, use this Trim", and Trim is interpolated in log
// space between the anchors on either side of the live value, held flat beyond the first and last.
//
// The key is the BASE white point (PreExposure / exposure), not the raw exposure. A game that changes
// its PreExposure scale changes the exposure by the same factor, so keying on exposure would move the
// curve for no reason: PreExposure 1 / exposure 0.01 and PreExposure 8 / exposure 0.08 are the same
// position, base white point 100.
//
// Stored as "baseWhitePoint:trim;" pairs in the ini, at most eight, sorted by key. Game exposure and
// automatic exposure keep separate tables. Shared by the D3D12 path, the Vulkan path and the menu so
// the three cannot drift apart. The shader has its own copy of the interpolation
// (EffectiveExposureTrim in dlssnr.hlsl) that must agree with TrimForKey below.

#include "DlssNr_Common.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

// FillConstants below writes the anchor pairs by walking floats from ExposureTrimAnchorExposure0, and
// the shader reads them by name in this order. A reordered or inserted field would silently shift the
// table, so the layout is pinned here rather than trusted.
static_assert(offsetof(DlssNrConstants, ExposureTrimAnchorTrim0) ==
              offsetof(DlssNrConstants, ExposureTrimAnchorExposure0) + sizeof(float));
static_assert(offsetof(DlssNrConstants, ExposureTrimAnchorExposure1) ==
              offsetof(DlssNrConstants, ExposureTrimAnchorExposure0) + 2 * sizeof(float));
static_assert(offsetof(DlssNrConstants, ExposureTrimAnchorExposure7) ==
              offsetof(DlssNrConstants, ExposureTrimAnchorExposure0) + 14 * sizeof(float));
static_assert(offsetof(DlssNrConstants, ExposureTrimAnchorTrim7) ==
              offsetof(DlssNrConstants, ExposureTrimAnchorExposure0) + 15 * sizeof(float));

namespace DlssNrTrim
{
constexpr size_t kMaxAnchors = 8;
constexpr float kMinTrim = 0.25f;
constexpr float kMaxTrim = 50.0f;

struct Anchor
{
    float key = 0.0f; // base white point
    float trim = 1.0f;
};

inline float ClampTrim(float trim) { return std::clamp(trim, kMinTrim, kMaxTrim); }

inline bool ValidKey(float key) { return std::isfinite(key) && key > 1e-8f; }

// Malformed pairs are skipped rather than rejected: an ini edited by hand should lose one bad entry,
// not the table.
inline std::vector<Anchor> Parse(const std::string& text)
{
    std::vector<Anchor> out;
    size_t pos = 0;

    while (pos < text.size() && out.size() < kMaxAnchors)
    {
        const size_t semi = text.find(';', pos);
        const std::string token = text.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        pos = semi == std::string::npos ? text.size() : semi + 1;

        const size_t colon = token.find(':');
        if (colon == std::string::npos)
            continue;

        try
        {
            const float key = std::stof(token.substr(0, colon));
            const float trim = std::stof(token.substr(colon + 1));
            if (ValidKey(key) && std::isfinite(trim) && trim > 0.0f)
                out.push_back({ key, ClampTrim(trim) });
        }
        catch (...)
        {
        }
    }

    std::sort(out.begin(), out.end(), [](const Anchor& a, const Anchor& b) { return a.key < b.key; });
    return out;
}

inline std::string Serialize(const std::vector<Anchor>& anchors)
{
    std::string out;
    char buffer[64];

    for (const auto& anchor : anchors)
    {
        snprintf(buffer, sizeof(buffer), "%.7g:%.7g;", anchor.key, anchor.trim);
        out += buffer;
    }

    return out;
}

// Adds an anchor, or replaces the one within 2% of the same key. False when the key is unusable or
// the table is already full and nothing was within reach to replace.
inline bool Upsert(std::vector<Anchor>& anchors, float key, float trim)
{
    if (!ValidKey(key))
        return false;

    trim = ClampTrim(trim);

    for (auto& anchor : anchors)
    {
        if (key > anchor.key * 0.98f && key < anchor.key * 1.02f)
        {
            anchor = { key, trim };
            std::sort(anchors.begin(), anchors.end(), [](const Anchor& a, const Anchor& b) { return a.key < b.key; });
            return true;
        }
    }

    if (anchors.size() >= kMaxAnchors)
        return false;

    anchors.push_back({ key, trim });
    std::sort(anchors.begin(), anchors.end(), [](const Anchor& a, const Anchor& b) { return a.key < b.key; });
    return true;
}

// The table lives in the ini as text, so the menu works on the text: Tune's result becomes a point (replacing one within 2%
// of the same brightness), a point can be removed. AddPoint is false when nothing changed (an unusable key, or the table
// is full and nothing was within reach to replace).
inline bool AddPoint(std::string& text, float baseWhitePoint, float trim)
{
    std::vector<Anchor> anchors = Parse(text);

    if (!Upsert(anchors, baseWhitePoint, trim))
        return false;

    text = Serialize(anchors);
    return true;
}

inline void RemovePoint(std::string& text, size_t index)
{
    std::vector<Anchor> anchors = Parse(text);

    if (index >= anchors.size())
        return;

    anchors.erase(anchors.begin() + (std::ptrdiff_t) index);
    text = Serialize(anchors);
}

// The Trim in force at a base white point: the slider when there are no anchors, the key is unusable,
// or preview is on; otherwise the anchors' Trim, log-interpolated between neighbours.
inline float TrimForKey(float key, float sliderTrim, const std::vector<Anchor>& anchors, bool preview)
{
    sliderTrim = ClampTrim(sliderTrim);

    if (preview || anchors.empty() || !ValidKey(key))
        return sliderTrim;
    if (anchors.size() == 1)
        return anchors[0].trim;
    if (key <= anchors.front().key)
        return anchors.front().trim;
    if (key >= anchors.back().key)
        return anchors.back().trim;

    for (size_t i = 0; i + 1 < anchors.size(); ++i)
    {
        const auto& a = anchors[i];
        const auto& b = anchors[i + 1];

        if (key >= a.key && key <= b.key && b.key > a.key * 1.000001f)
        {
            const float t = (std::log(key) - std::log(a.key)) / (std::log(b.key) - std::log(a.key));
            return ClampTrim(std::exp(std::log(a.trim) + t * (std::log(b.trim) - std::log(a.trim))));
        }
    }

    return anchors.back().trim;
}

// Writes the trim fields of the constants the shader reads. The shader recomputes the same white point
// from these when it is reading a live exposure texture.
inline void FillConstants(DlssNrConstants& params, float sliderTrim, const std::vector<Anchor>& anchors,
                          bool preview, float shadowProtection)
{
    params.ExposureTrim = ClampTrim(sliderTrim);
    params.ExposureTrimPreview = preview ? 1u : 0u;
    params.ExposureTrimAnchorCount = (uint32_t) std::min(anchors.size(), kMaxAnchors);

    float* pairs = &params.ExposureTrimAnchorExposure0;
    for (size_t i = 0; i < params.ExposureTrimAnchorCount; ++i)
    {
        pairs[i * 2 + 0] = anchors[i].key;
        pairs[i * 2 + 1] = anchors[i].trim;
    }

    params.AutoExposureShadowProtection = std::clamp(shadowProtection, 0.0f, 100.0f);
}
} // namespace DlssNrTrim
