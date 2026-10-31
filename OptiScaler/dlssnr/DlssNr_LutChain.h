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

// Composes `layers`, first to last, into *out, and gives in *passStrength the strength the GPU pass applies it with.
// Returns false with *out cleared when no layer does anything.
//
// One effective layer is the file itself, bit for bit, at its own size and domain, with its strength left to the pass:
// exactly the single-LUT pass, including display values outside the layer's domain (a lerp toward the unclamped input,
// which a lattice cannot hold). Two or more are sampled on a grid of clamp(largest member size, kChainMinSize,
// kChainMaxSize) points per side over display 0..1 (the domain the shader's input and output both live in), each point
// carried through every layer in turn with its strength, and the pass applies the result at strength 1.
inline bool BakeChain(const std::vector<ChainLayer>& layers, Lut3D* out, std::string* error, float* passStrength)
{
    *out = Lut3D();
    *passStrength = 1.0f;
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
        *out = *active[0].lut;
        *passStrength = std::clamp(active[0].strength, 0.0f, 1.0f);
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
    std::filesystem::file_time_type writeTimes[DlssNr::kMaxLutChainLayers] {}; // of each layer's file when last looked
    int ensuresUntilTimeCheck = 0; // the files' write times are looked at once every kTimeCheckEvery Ensure calls

    // What `baked` was made from: ordered "path|write time|strength", the strength left out for a single layer, whose
    // strength the pass applies (so moving its slider re-uploads nothing).
    std::string key;
    DlssNr::Lut3D baked;
    float strength = 1.0f;   // the strength the pass applies `baked` with
    std::string description; // DlssNr_DescribeChain's text, made when `baked` is
    bool wanted = false;
    int failedLayer = -1; // index into the entries given to the last Ensure, -1 when none failed
    std::string failedPath;
    std::string error;

    bool Loaded() const { return baked.size >= 2; }
};

inline std::string DlssNr_DescribeChain(const DlssNr_LutChainState& state,
                                        const std::vector<DlssNr::ChainEntry>& present);

// Parses the entries (a path already tried is not read again unless its file was written since) and re-bakes only when
// the ordered paths, their files or the strengths changed. Returns whether a chain is usable. No entries clears
// everything.
inline bool DlssNr_LutChainEnsure(DlssNr_LutChainState* state, const std::vector<DlssNr::ChainEntry>& entries)
{
    using namespace DlssNr;
    // About once a second at 60 fps: a .cube re-exported in place is picked up without a file query every frame.
    constexpr int kTimeCheckEvery = 60;

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
        state->strength = 1.0f;
        state->description.clear();
        state->ensuresUntilTimeCheck = 0;
        return false;
    }

    const bool checkTimes = --state->ensuresUntilTimeCheck <= 0;
    if (checkTimes)
        state->ensuresUntilTimeCheck = kTimeCheckEvery;

    for (size_t i = 0; i < kMaxLutChainLayers; ++i)
        if (i >= present.size())
            DlssNr_LutEnsureParsed(&state->layers[i], "");

    for (size_t i = 0; i < present.size(); ++i)
    {
        // A new path, or every kTimeCheckEvery calls: note the file's write time. A file written since it was parsed is
        // parsed again (forgetting the attempt is what makes DlssNr_LutEnsureParsed read it).
        DlssNr_LutState& layer = state->layers[i];
        if (layer.attemptedPath != present[i].path || checkTimes)
        {
            std::error_code ec;
            const auto written = std::filesystem::last_write_time(present[i].path, ec);
            const auto noted = ec ? std::filesystem::file_time_type {} : written;
            if (layer.attemptedPath == present[i].path && noted != state->writeTimes[i])
                layer.attemptedPath.clear();
            state->writeTimes[i] = noted;
        }

        DlssNr_LutEnsureParsed(&layer, present[i].path);

        if (state->layers[i].failed && state->failedLayer < 0)
        {
            state->failedLayer = (int) i;
            state->failedPath = state->layers[i].attemptedPath;
            state->error = state->layers[i].error;
        }
    }

    if (state->failedLayer >= 0)
        return state->Loaded(); // keep the last good chain

    const size_t effective =
        (size_t) std::count_if(present.begin(), present.end(), [](const ChainEntry& e) { return e.strength > 0.0f; });

    std::string key;
    for (size_t i = 0; i < present.size(); ++i)
    {
        key += present[i].path + "|" + std::to_string(state->writeTimes[i].time_since_epoch().count());
        if (effective != 1)
            key += "|" + std::to_string(std::clamp(present[i].strength, 0.0f, 1.0f));
        key += "\n";
    }

    if (key != state->key)
    {
        std::vector<ChainLayer> layers;
        for (size_t i = 0; i < present.size(); ++i)
            layers.push_back({ &state->layers[i].lut, present[i].strength });

        std::string err;
        Lut3D baked;
        float strength = 1.0f;
        const bool ok = BakeChain(layers, &baked, &err, &strength);
        state->key = key;
        state->baked = ok ? std::move(baked) : Lut3D();
        state->strength = strength;
        state->description = DlssNr_DescribeChain(*state, present);
    }
    else if (effective == 1)
    {
        // A single layer's strength changes without a re-bake: the pass applies it.
        for (const ChainEntry& e : present)
            if (e.strength > 0.0f)
                state->strength = std::clamp(e.strength, 0.0f, 1.0f);
    }

    if (effective == 0)
        state->error = "every LUT is at strength 0";

    return state->Loaded();
}

// "A (33^3) -> B (65^3), baked at 33^3" for the menu's status line: the layers of `present` (the entries the last
// Ensure kept) with a strength above 0, the ones in the bake. Empty when nothing is loaded. Made once per bake and
// kept in state.description, not per frame.
inline std::string DlssNr_DescribeChain(const DlssNr_LutChainState& state,
                                        const std::vector<DlssNr::ChainEntry>& present)
{
    if (!state.Loaded())
        return std::string();

    std::string text;
    int shown = 0;
    for (size_t i = 0; i < present.size() && i < (size_t) DlssNr::kMaxLutChainLayers; ++i)
    {
        const DlssNr_LutState& layer = state.layers[i];
        if (!layer.Loaded() || !(present[i].strength > 0.0f))
            continue;
        if (!text.empty())
            text += " -> ";
        text +=
            std::filesystem::path(layer.loadedPath).filename().string() + " (" + std::to_string(layer.lut.size) + "^3)";
        ++shown;
    }
    return shown > 1 ? text + ", baked at " + std::to_string(state.baked.size) + "^3" : text;
}
