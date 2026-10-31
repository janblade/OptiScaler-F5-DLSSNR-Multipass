#pragma once

// A stack of LUTs applied one after another, baked into ONE lattice (post-NR LUT epic,
// memory/plans/2026-10-31-nr-lut-chain.md). The GPU pass keeps sampling a single 3D texture, so stacking costs one
// fetch per pixel at any depth and needs no shader or constant change. Pure and free of D3D/Vulkan types like
// DlssNr_LutFile.h, so tests/nr_lut_chain_smoke.cpp runs it on the host.
//
// Per layer this is exactly what dlssnr_lut.hlsl does to a pixel: clamp the display value into the layer's domain,
// trilinear-sample the lattice, then blend original -> graded by the layer's strength. The blend uses the unclamped
// input, as the shader does.

#include "DlssNr_Lut.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace DlssNr
{
constexpr int kMaxLutChainLayers = 4;

// Baked lattice size bounds for a stack of two or more layers: at least 17 so a stack of small LUTs is not resampled
// coarser than a typical one, at most 65 (the largest LUT anyone ships) so the texture stays small.
constexpr int kChainMinSize = 17;
constexpr int kChainMaxSize = 65;

// Trilinear sample of `lut` at the display value `in`, with the shader's domain clamp. Lattice point i sits at
// uvw = i / (size - 1); red is the fastest axis (see Lut3D::rgb).
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

// One layer on a display value, as the shader does it.
inline void ApplyLayer(const Lut3D& lut, float strength, const float in[3], float out[3])
{
    float graded[3];
    SampleLut(lut, in, graded);
    const float s = std::clamp(strength, 0.0f, 1.0f);

    for (int c = 0; c < 3; ++c)
        out[c] = in[c] + (graded[c] - in[c]) * s;
}

struct ChainLayer
{
    const Lut3D* lut = nullptr; // null or empty: skipped
    float strength = 1.0f;      // 0 is skipped too (it leaves the picture untouched)
};

// Composes `layers`, first to last, into *out. Returns false with *out cleared when no layer does anything.
//
// One effective layer keeps that layer's own size and domain, so one LUT at full strength is the file itself, bit for
// bit, and at a lower strength matches the single-LUT pass. Two or more are sampled on a grid of
// clamp(largest member size, kChainMinSize, kChainMaxSize) points per side over display 0..1 (the domain the shader's
// input and output both live in), each point carried through every layer in turn.
inline bool BakeChain(const std::vector<ChainLayer>& layers, Lut3D* out, std::string* error)
{
    *out = Lut3D();
    std::vector<ChainLayer> active;

    for (const ChainLayer& layer : layers)
    {
        if (layer.lut == nullptr || layer.lut->size < 2 ||
            layer.lut->rgb.size() < (size_t) layer.lut->size * layer.lut->size * layer.lut->size * 3)
            continue;
        if (!(layer.strength > 0.0f))
            continue;
        active.push_back(layer);
    }

    if (active.empty())
        return false;

    if (active.size() > (size_t) kMaxLutChainLayers)
    {
        if (error != nullptr)
            *error = "more than " + std::to_string(kMaxLutChainLayers) + " LUTs in the stack";
        return false;
    }

    int n = 0;
    for (const ChainLayer& layer : active)
        n = std::max(n, layer.lut->size);
    n = std::clamp(n, kChainMinSize, kChainMaxSize);

    if (active.size() == 1)
    {
        const Lut3D& src = *active[0].lut;
        const float s = std::clamp(active[0].strength, 0.0f, 1.0f);
        *out = src;

        if (s < 1.0f)
        {
            // Blend toward the identity ramp of the layer's own domain, which is what the shader's lerp(display,
            // graded, s) does for a display value inside the domain.
            const int m = src.size;
            for (int b = 0; b < m; ++b)
                for (int g = 0; g < m; ++g)
                    for (int r = 0; r < m; ++r)
                    {
                        const int idx[3] = { r, g, b };
                        for (int c = 0; c < 3; ++c)
                        {
                            const float id = src.domainMin[c] +
                                             (src.domainMax[c] - src.domainMin[c]) * ((float) idx[c] / (float) (m - 1));
                            float& v = out->rgb[(((size_t) b * m + g) * m + r) * 3 + c];
                            v = id + (v - id) * s;
                        }
                    }
        }
        return true;
    }

    out->size = n;
    out->rgb.resize((size_t) n * n * n * 3);

    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r)
            {
                float v[3] = { (float) r / (float) (n - 1), (float) g / (float) (n - 1), (float) b / (float) (n - 1) };

                for (const ChainLayer& layer : active)
                {
                    float next[3];
                    ApplyLayer(*layer.lut, layer.strength, v, next);
                    v[0] = next[0];
                    v[1] = next[1];
                    v[2] = next[2];
                }

                float* dst = &out->rgb[(((size_t) b * n + g) * n + r) * 3];
                dst[0] = v[0];
                dst[1] = v[1];
                dst[2] = v[2];
            }

    return true;
}

// One entry of the stack as the settings hold it. An empty path is an absent layer.
struct ChainEntry
{
    std::string path;
    float strength = 1.0f;
};

// The post-NR stack as Config holds it (templated so this header stays free of Config.h).
template <typename ConfigT> inline std::vector<ChainEntry> PostLutEntries(const ConfigT& cfg)
{
    std::vector<ChainEntry> entries;
    for (int i = 0; i < ConfigT::kPostLutLayers; ++i)
        entries.push_back(
            { cfg.DlssNrPostLutFile[i].value_or_default(), cfg.DlssNrPostLutStrength[i].value_or_default() });
    return entries;
}

} // namespace DlssNr

// The parsed layers, the baked result and the last problem, for one stack. Same contract as DlssNr_LutState: a layer
// that fails to parse does not blank out a chain that was working, it is reported by `failedLayer` / `error`.
struct DlssNr_LutChainState
{
    DlssNr_LutState layers[DlssNr::kMaxLutChainLayers];
    std::string key; // what `baked` was made from: ordered "path|strength"
    DlssNr::Lut3D baked;
    bool wanted = false;
    int failedLayer = -1; // index into the entries given to the last Ensure, -1 when none failed
    std::string failedPath;
    std::string error;

    bool Loaded() const { return baked.size >= 2; }
};

// Parses the entries (a path already tried is not read again) and re-bakes only when the ordered paths or strengths
// changed. Returns whether a chain is usable. No entries clears everything.
inline bool DlssNr_LutChainEnsure(DlssNr_LutChainState* state, const std::vector<DlssNr::ChainEntry>& entries)
{
    using namespace DlssNr;
    if (state == nullptr)
        return false;

    std::vector<ChainEntry> present;
    for (const ChainEntry& e : entries)
        if (!e.path.empty() && present.size() < (size_t) kMaxLutChainLayers)
            present.push_back(e);

    state->wanted = !present.empty();
    state->failedLayer = -1;
    state->failedPath.clear();
    state->error.clear();

    if (present.empty())
    {
        for (DlssNr_LutState& layer : state->layers)
            DlssNr_LutEnsureParsed(&layer, "");
        state->key.clear();
        state->baked = Lut3D();
        return false;
    }

    for (size_t i = 0; i < kMaxLutChainLayers; ++i)
        if (i >= present.size())
            DlssNr_LutEnsureParsed(&state->layers[i], "");

    for (size_t i = 0; i < present.size(); ++i)
    {
        DlssNr_LutEnsureParsed(&state->layers[i], present[i].path);

        if (state->layers[i].failed && state->failedLayer < 0)
        {
            state->failedLayer = (int) i;
            state->failedPath = state->layers[i].attemptedPath;
            state->error = state->layers[i].error;
        }
    }

    if (state->failedLayer >= 0)
        return state->Loaded(); // keep the last good chain

    std::string key;
    for (const ChainEntry& e : present)
        key += e.path + "|" + std::to_string(std::clamp(e.strength, 0.0f, 1.0f)) + "\n";

    if (key != state->key)
    {
        std::vector<ChainLayer> layers;
        for (size_t i = 0; i < present.size(); ++i)
            layers.push_back({ &state->layers[i].lut, present[i].strength });

        std::string err;
        Lut3D baked;
        const bool ok = BakeChain(layers, &baked, &err);
        state->key = key;
        state->baked = ok ? std::move(baked) : Lut3D();
        state->error = err;
    }

    return state->Loaded();
}

// "A (33^3) -> B (65^3), baked at 33^3" for the menu's status line; empty when nothing is loaded.
inline std::string DlssNr_DescribeChain(const DlssNr_LutChainState& state)
{
    if (!state.Loaded())
        return std::string();

    std::string text;
    for (const DlssNr_LutState& layer : state.layers)
    {
        if (!layer.Loaded())
            continue;
        if (!text.empty())
            text += " -> ";
        text +=
            std::filesystem::path(layer.loadedPath).filename().string() + " (" + std::to_string(layer.lut.size) + "^3)";
    }
    return text + ", baked at " + std::to_string(state.baked.size) + "^3";
}
