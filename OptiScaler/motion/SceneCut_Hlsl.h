#pragma once

// The scene-cut detector's compute shaders (motion/SceneCut_Dx12.cpp): brightness histograms of a luma in a 3x3 grid,
// compared with the last frame's, and a luma pass for callers without one. Compiled ahead of time into
// native/F5LowShaderBytecode.h by tests/nr_shader_bytecode_smoke.cpp (run it with --write after changing anything
// here; without, it checks the embedded bytecode is current).

#include "Luma_Hlsl.h"

namespace SceneCutHlsl
{
inline constexpr const char* kSource = R"HLSL(
cbuffer S : register(b0)
{
    uint2 size;       // the luma's size (SceneLuma: the size it writes)
    uint hasPrevious; // the last frame's smoothed histograms are in the state, to compare with
    float threshold;  // the divergence past which the frame is a cut
    uint2 colorSize;  // SceneLuma: the colour's size
    uint step;        // SceneLuma: how many colour pixels, each way, one luma pixel stands for
    float whiteNits;  // SceneLuma: what an HDR picture's white is, in nits (scRGB 1.0 is 80 nits)
};

Texture2D<float> Luma : register(t0);
Texture2D<float4> Color : register(t0);
RWTexture2D<float> OutLuma : register(u0);
RWTexture2D<uint> State : register(u0); // x is the bin; row t (0..8) holds tile t's counts, row 9 + t its last smoothed histogram
                                        // (float bits), row 18 the scratch: a sum for each shift, and at x = 255 the count of
                                        // finished groups
RWTexture2D<uint> Cut : register(u1);   // texel 0 the flag, texel 1 the divergence (float bits)
RWTexture2D<unorm float> Distrust : register(u2); // 1x1: 1 on a cut, else 0 (a history-distrust input read by uv)
)HLSL"
                      F5LOW_LUMA_HLSL
                      R"HLSL(
// The detector's own luma, for a caller that has none (DLSS-NR's game input): the colour shrunk by `step` each way, each
// pixel the mean luma of up to 4x4 samples spread over its block. A few hundred pixels across are plenty for nine
// histograms, and the cost stays the same at any resolution.
[numthreads(8, 8, 1)]
void SceneLuma(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    const uint taps = min(step, 4u);
    float sum = 0.0;

    for (uint j = 0; j < taps; ++j)
        for (uint i = 0; i < taps; ++i)
        {
            const uint2 p = min(id.xy * step + (uint2(i, j) * step + step / 2) / taps, colorSize - 1);
            sum += LumaOf(Color.Load(int3(p, 0)).rgb);
        }

    OutLuma[id.xy] = sum / (float) (taps * taps);
}

// The bins are equal steps of the luma's logarithm, nine stops of it over 256 bins, so a brightness change of the whole picture
// (exposure, a fade) moves every histogram the same number of bins sideways, whatever the picture holds.
static const float kStops = 9.0;
static const int kMaxShift = 48; // bins, either way: the sideways shifts tried are -48 .. 48 (97), about 1.7 stops of luma
static const uint kShifts = 2 * kMaxShift + 1;
static const uint kCopies = 8; // each group counts in eight copies of its histogram, so lanes that hit one bin do not queue
groupshared uint gCounts[kCopies * 256];

// The counts of one tile: eight groups per tile each take a strip of its rows and add what they counted into the state.
[numthreads(256, 1, 1)]
void SceneHist(uint3 group : SV_GroupID, uint t : SV_GroupIndex)
{
    for (uint k = t; k < kCopies * 256; k += 256)
        gCounts[k] = 0;
    GroupMemoryBarrierWithGroupSync();

    const uint2 tile = size / 3;
    const uint2 origin = uint2(group.y % 3, group.y / 3) * tile;
    const uint rows = (tile.y + 7) / 8;
    const uint y0 = group.x * rows, y1 = min(y0 + rows, tile.y);
    const uint lx = t % 32, ly = t / 32;
    const uint copy = lx % kCopies;

    for (uint y = y0 + ly; y < y1; y += 8)
        for (uint x = lx; x < tile.x; x += 32)
        {
            const float l = max(Luma.Load(int3(int2(origin + uint2(x, y)), 0)), 1.0e-6);
            const uint bin = (uint) clamp((log2(l) + kStops) * (255.0 / kStops), 0.0, 255.0);
            InterlockedAdd(gCounts[copy * 256 + bin], 1);
        }

    GroupMemoryBarrierWithGroupSync();

    uint sum = 0;
    for (uint c = 0; c < kCopies; ++c)
        sum += gCounts[c * 256 + t];

    if (sum != 0)
        InterlockedAdd(State[uint2(t, group.y)], sum);
}

static const float kKernel[6] = { 0.0088122291, 0.027143577, 0.065114059, 0.12164907, 0.17699835, 0.20056541 };

groupshared float gCounted[256];
groupshared float gSmooth[256];
groupshared float gBefore[256];
groupshared float gSum[256];
groupshared uint gLast;

// One group per tile. The counts are smoothed (an 11-tap bell, and one added to every bin so nothing is empty) and made into
// a distribution. Its symmetric KL divergence to the last frame's distribution is taken for every sideways shift of the new one
// from -48 to 48 bins (a bin that shifts in from outside the range counts one), a thread for each shift, and the smallest
// counts: a brightness step of the whole picture only moves the histograms sideways and must not read as a cut, while a cut that
// changes what is where (the sky now below, a dark tile now bright) cannot be undone by one shift for every tile. Each tile's
// value for a shift, 1 - exp(-divergence), is added into the scratch (a ninth of it each); the last group to finish takes the
// smallest sum and sets the flag.
[numthreads(256, 1, 1)]
void SceneDiverge(uint3 group : SV_GroupID, uint i : SV_GroupIndex)
{
    const uint tile = group.x;

    gCounted[i] = (float) State[uint2(i, tile)];
    State[uint2(i, tile)] = 0; // ready for the next frame's counts
    GroupMemoryBarrierWithGroupSync();

    float v = 1.0;
    [unroll] for (int k = -5; k <= 5; ++k)
        v += kKernel[5 - abs(k)] * gCounted[clamp(int(i) + k, 0, 255)];
    gSmooth[i] = v;
    gSum[i] = v;
    GroupMemoryBarrierWithGroupSync();

    for (uint s = 128; s > 0; s >>= 1)
    {
        if (i < s)
            gSum[i] += gSum[i + s];
        GroupMemoryBarrierWithGroupSync();
    }

    const float total = gSum[0];
    const float before = hasPrevious != 0 ? asfloat(State[uint2(i, 9 + tile)]) : v / total;
    gBefore[i] = before;
    GroupMemoryBarrierWithGroupSync(); // everyone has read the old histogram and has what it needs of this one
    State[uint2(i, 9 + tile)] = asuint(v / total);

    if (hasPrevious == 0)
    {
        if (group.x == 0 && i == 0)
        {
            Cut[uint2(0, 0)] = 0;
            Cut[uint2(1, 0)] = 0;
            Distrust[uint2(0, 0)] = 0.0;
        }
        return;
    }

    if (i < kShifts)
    {
        // This thread's shift: the new histogram's bin j + shift is compared with the old one's bin j.
        const int shift = int(i) - kMaxShift;
        float sum = total;

        for (int d = 0; d < abs(shift); ++d)
            sum += 1.0 - gSmooth[shift > 0 ? d : 255 - d];

        const float inverse = 1.0 / sum;
        float divergence = 0.0;

        for (int j = 0; j < 256; ++j)
        {
            const int from = j + shift;
            const float p = (from < 0 || from > 255 ? 1.0 : gSmooth[from]) * inverse;
            const float q = gBefore[j];
            divergence += (p - q) * log(p / q); // p log(p/q) + q log(q/p)
        }

        InterlockedAdd(State[uint2(i, 18)], (uint) ((1.0 - exp(-abs(divergence))) / 9.0 * 1000000.0));
    }

    DeviceMemoryBarrier();
    GroupMemoryBarrierWithGroupSync();

    if (i == 0)
    {
        uint finished;
        InterlockedAdd(State[uint2(255, 18)], 1, finished);
        gLast = finished == 8 ? 1 : 0;
    }
    GroupMemoryBarrierWithGroupSync();

    if (gLast == 0)
        return;

    // The last group: the smallest of the shifts' sums (the scratch is cleared as it is read).
    uint sumHere = 0;
    if (i < kShifts)
        InterlockedExchange(State[uint2(i, 18)], 0, sumHere);
    gSum[i] = i < kShifts ? (float) sumHere / 1000000.0 : 2.0;
    GroupMemoryBarrierWithGroupSync();

    for (uint r = 128; r > 0; r >>= 1)
    {
        if (i < r)
            gSum[i] = min(gSum[i], gSum[i + r]);
        GroupMemoryBarrierWithGroupSync();
    }

    if (i == 0)
    {
        Cut[uint2(0, 0)] = gSum[0] > threshold ? 1 : 0;
        Cut[uint2(1, 0)] = asuint(gSum[0]);
        Distrust[uint2(0, 0)] = gSum[0] > threshold ? 1.0 : 0.0;
        State[uint2(255, 18)] = 0;
    }
}
)HLSL";
} // namespace SceneCutHlsl
