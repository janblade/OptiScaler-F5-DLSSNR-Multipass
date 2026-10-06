// Not built with the precompiled header: self-contained so the host GPU test can compile it alone.
#include "OpticalFlow_Dx12.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

namespace
{

constexpr uint32_t kDescriptorsPerPass = 8; // seven SRVs and one UAV (the scene-cut passes use three of the eight)
constexpr uint32_t kPassesPerFrame = 1 + 2 + (OpticalFlowDx12::kLevels - 1) + OpticalFlowDx12::kLevels + 1 + 1 + 1 + 1;
constexpr uint32_t kFramesInFlight = 8;
constexpr DXGI_FORMAT kLumaFormat = DXGI_FORMAT_R32_FLOAT;
constexpr DXGI_FORMAT kFlowFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; // typed UAV stores of this are required of every device
constexpr DXGI_FORMAT kStateFormat = DXGI_FORMAT_R32_UINT; // typed atomics on this are required of every device
constexpr uint32_t kSceneStateRows = 19; // nine tiles' counts, nine previous histograms, one row of scratch

const char* kSource = R"HLSL(
cbuffer P : register(b0)
{
    uint2 size;      // the size of what is written
    uint2 aux;       // the size of what is read (luma, down) or unused
    int radius;
    uint hasPrediction;
    float lambda;
    float scale;
    uint hasHistory;
    float knee;
    uint coarseCells;
    uint depthMatching; // the window's samples count by how near their depth is to this pixel's (SceneDepth, t4)
    uint2 depthSize;
    uint reversed;
    uint hasGlobal; // Match: the frame-wide candidate (GlobalFlow) is there to be tried
    uint inverseRefinement; // Match: the sub-pixel steps use the current frame's gradients, found once
    uint sceneCutEnabled;   // Match: the scene-cut flag (CutFlag, t6) is there to be read
    float whiteNits;        // Luma: what an HDR picture's white is, in nits (scRGB 1.0 is 80 nits)
    float zeroMargin;       // Match, finest level: no motion wins when its cost is within this of the best (0 = off)
    uint zeroReach;         // Match: ... but only over a best offset no larger than this many pixels (0 = any)
};

SamplerState Linear : register(s0);
Texture2D<float4> Color : register(t0);
Texture2D<float>  CurLuma : register(t0);
Texture2D<float>  PrevLuma : register(t1);
Texture2D<float4> Prediction : register(t2);
Texture2D<float4> History : register(t3);
Texture2D<float>  GuideLuma : register(t1);
Texture2D<float4> FlowIn : register(t0);
Texture2D<float>  SceneDepth : register(t4);
Texture2D<float4> GlobalFlow : register(t5);
Texture2D<uint>   CutFlag : register(t6);
RWTexture2D<float>  OutLuma : register(u0);
RWTexture2D<float4> OutFlow : register(u0);

// PQ (SMPTE ST 2084) to linear light, 1.0 being 10000 nits.
float3 PqToLinear(float3 v)
{
    const float3 p = pow(saturate(v), 1.0 / 78.84375);
    return pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125);
}

// The luma of one colour for the match, in one of four ways (LUMA_MODE, a different pipeline for each). 0, the old one: Rec.601
// weights and l / (1 + l) on the values as they are. 1, a gamma-encoded SDR colour: already a perceptual one, so it is
// used as it is (the same weights, without the compression). 2 and 3, a linear scRGB or a PQ colour: made linear light
// relative to the white (its Rec.709 or Rec.2020 luminance over whiteNits) and put through the CIE lightness curve, 0..1
// from black to white and above 1 for highlights. Equal steps of it look alike, so the match weighs a dark detail like a
// bright one.
// The match's thresholds (lambda, the confidence knee) are in these units, and 1, 2 and 3 reach white at 1.0, twice the
// old one's 0.5. Much of what the newer ones gain comes from that: the old luma doubled matches as well on a dark HDR
// pan, and mode 1 halved loses nearly all of the grain tally. Mode 1's gain on thin bright lines is its own (no
// compression).
#ifndef LUMA_MODE
#define LUMA_MODE 0
#endif

float LumaOf(float3 c)
{
    c = max(c, 0.0);

#if LUMA_MODE == 0
    const float l = dot(c, float3(0.299, 0.587, 0.114));
    return l / (1.0 + l);
#elif LUMA_MODE == 1
    return dot(min(c, 4.0), float3(0.299, 0.587, 0.114));
#else
#if LUMA_MODE == 2
    const float y = dot(c, float3(0.2126, 0.7152, 0.0722)) * (80.0 / whiteNits);
#else
    const float y = dot(PqToLinear(c), float3(0.2627, 0.6780, 0.0593)) * (10000.0 / whiteNits);
#endif
    const float yc = min(y, 64.0);
    return 0.01 * (yc <= 216.0 / 24389.0 ? yc * (24389.0 / 27.0) : 116.0 * pow(yc, 1.0 / 3.0) - 16.0);
#endif
}

// Luma of the colour, averaged over 2x2 into the first level.
[numthreads(8, 8, 1)]
void Luma(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    float sum = 0.0;
    [unroll] for (int j = 0; j < 2; ++j)
        [unroll] for (int i = 0; i < 2; ++i)
        {
            int2 p = min(int2(id.xy) * 2 + int2(i, j), int2(aux) - 1);
            sum += LumaOf(Color.Load(int3(p, 0)).rgb);
        }

    OutLuma[id.xy] = sum * 0.25;
}

// The next level: a 2x2 average of the one above.
[numthreads(8, 8, 1)]
void Down(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    float sum = 0.0;
    [unroll] for (int j = 0; j < 2; ++j)
        [unroll] for (int i = 0; i < 2; ++i)
            sum += CurLuma.Load(int3(min(int2(id.xy) * 2 + int2(i, j), int2(aux) - 1), 0));

    OutLuma[id.xy] = sum * 0.25;
}

// The previous frame at a fractional pixel position, filtered.
float Sample(float2 position)
{
    return PrevLuma.SampleLevel(Linear, (position + 0.5) / float2(size), 0);
}

// How far the surface at uv (0..1 across the picture) is, larger is farther; only the ratio of two is used.
float Distance(float2 uv)
{
    int2 at = min(int2(uv * float2(depthSize)), int2(depthSize) - 1);
    float d = SceneDepth.Load(int3(at, 0));
    return 1.0 / max(reversed != 0 ? d : 1.0 - d, 1e-6);
}

// How much a sample at distance z counts beside one at zc: about 1 on the same surface, little across a depth edge.
float SameSurface(float z, float zc)
{
    float rel = abs(z - zc) / max(min(z, zc), 1e-6);
    return max(exp(-rel * rel / 0.01), 0.02);
}

// Sum of absolute differences between the current frame around p and the previous frame around p + d: sixteen samples, two
// pixels apart, over an 8x8 window, each counted by w. Each window's mean is taken out first, so a picture that got brighter
// or darker as a whole (eye adaptation, a fade, a flash) still matches where its content went. cur is the current frame's
// sixteen samples around p, which are the same for every offset, so the caller loads them once.
float Cost(int2 p, int2 d, float w[16], float cur[16])
{
    float diff[16];
    float mean = 0.0, total = 0.0;
    int2 hi = int2(size) - 1;

    [unroll] for (int j = 0; j < 4; ++j)
        [unroll] for (int i = 0; i < 4; ++i)
        {
            int2 q = int2(2 * i - 3, 2 * j - 3);
            float c = cur[j * 4 + i];
            float r = PrevLuma.Load(int3(clamp(p + q + d, 0, hi), 0));
            diff[j * 4 + i] = c - r;
            mean += w[j * 4 + i] * (c - r);
            total += w[j * 4 + i];
        }

    mean /= total;
    float s = 0.0;

    [unroll] for (int k = 0; k < 16; ++k)
        s += w[k] * abs(diff[k] - mean);

    return s * 16.0 / total;
}

// Block matching at one level: look around the coarser level's answer (doubled, it is in that level's pixels) for the offset
// into the previous frame with the smallest difference, then a few gradient steps for the part of a pixel.
[numthreads(8, 8, 1)]
void Match(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    // A hard cut found on this very frame: the previous frame shows another scene, so there is nothing to match. No motion and
    // no confidence at every level (the whole group leaves here or none does: the flag is one value for the dispatch).
    if (sceneCutEnabled != 0 && CutFlag.Load(int3(0, 0, 0)) != 0)
    {
        OutFlow[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    int2 p = int2(id.xy);

    // How much each sample of the window counts: all alike, or with depth-aware matching only those on this pixel's surface,
    // so the edge of something nearer does not decide the match of what lies beside it.
    float w[16];
    float zc = depthMatching != 0 ? Distance((float2(p) + 0.5) / float2(size)) : 0.0;

    [unroll] for (int wj = 0; wj < 4; ++wj)
        [unroll] for (int wi = 0; wi < 4; ++wi)
        {
            int2 q = clamp(p + int2(2 * wi - 3, 2 * wj - 3), 0, int2(size) - 1);
            w[wj * 4 + wi] = depthMatching != 0 ? SameSurface(Distance((float2(q) + 0.5) / float2(size)), zc) : 1.0;
        }

    // The current frame's window, loaded once for every candidate and search offset below.
    float cur[16];

    [unroll] for (int cj = 0; cj < 4; ++cj)
        [unroll] for (int ci = 0; ci < 4; ++ci)
            cur[cj * 4 + ci] = CurLuma.Load(int3(clamp(p + int2(2 * ci - 3, 2 * cj - 3), 0, int2(size) - 1), 0));

    // The candidates for where to search: no motion, the coarser level's answer at the cells around this pixel (doubled, it is
    // in this level's pixels) and the last frame's flow here. The one that matches
    // best is where the search starts, so a steady pan carries over from frame to frame and an edge is not stuck with the
    // answer of a cell that lies across it.
    int2 centre = 0;
    float start = Cost(p, centre, w, cur);

    // What the whole picture did last frame (the camera): where nothing in the window says otherwise it wins the tie with no
    // motion, so a flat wall moves with the picture. `scale` here is this level's pixels per full-resolution pixel.
    if (hasGlobal != 0)
    {
        float4 g = GlobalFlow.Load(int3(0, 0, 0));

        if (g.z > 0.5)
        {
            int2 d = int2(round(g.xy * scale));
            float c = Cost(p, d, w, cur);

            if (c <= start)
            {
                start = c;
                centre = d;
            }
        }
    }

    if (hasPrediction != 0)
    {
        int2 coarseHi = int2(aux) - 1;
        int2 cp = min(p >> 1, coarseHi);
        int2 step = int2((p.x & 1) != 0 ? 1 : -1, (p.y & 1) != 0 ? 1 : -1);

        // Nearest first: the four a bilinear read would use, then the rest of the 3x3, the far side last.
        static const int2 kCells[9] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1), int2(-1, 0),
                                        int2(0, -1), int2(-1, 1), int2(1, -1), int2(-1, -1) };

        [loop] for (int k = 0; k < (int) coarseCells; ++k)
        {
            int2 cell = cp + kCells[k] * step;
            int2 d = int2(round(Prediction.Load(int3(clamp(cell, 0, coarseHi), 0)).xy * 2.0));
            float c = Cost(p, d, w, cur);

            if (c < start)
            {
                start = c;
                centre = d;
            }
        }
    }

    if (hasHistory != 0)
    {
        int2 d = int2(round(History.Load(int3(p, 0)).xy));
        float c = Cost(p, d, w, cur);

        if (c < start)
        {
            start = c;
            centre = d;
        }
    }

    float best = 1e30;
    float bestCost = 0.0;
    int2 bestD = centre;

    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx)
        {
            int2 d = centre + int2(dx, dy);
            float cost = Cost(p, d, w, cur);
            float c = cost + lambda * length(float2(dx, dy));

            if (c < best)
            {
                best = c;
                bestCost = cost;
                bestD = d;
            }
        }

    // Zero wins a near tie on the finest level: in flat or grainy ground the best match is a small offset picked on noise, and
    // the picture did not move there. Only an offset within zeroReach is overruled (a real pan is far from it).
    if (zeroMargin > 0.0 && any(bestD != 0) && (zeroReach == 0 || max(abs(bestD.x), abs(bestD.y)) <= (int) zeroReach))
        if (Cost(p, int2(0, 0), w, cur) <= bestCost + zeroMargin)
            bestD = int2(0, 0);

    // The part of a pixel: a few Lucas-Kanade steps. With the previous frame sampled at the matched offset, the remaining
    // difference is explained by the picture's gradient there; solve that for the shift. A match that is already exact has no
    // difference left and is not moved, which a fit through the costs either side cannot promise. The difference's mean over
    // the window is taken out first, so a brightness change does not read as a shift (the gradients are not centred).
    float2 sub = 0.0;
    float confidence = 0.0;

    if (inverseRefinement != 0)
    {
        // The same steps with the gradients taken from the current frame at the window's own pixels instead of from the
        // previous frame at the shifted position: they do not move with the shift, so they and everything built from them
        // are found once, and each step then needs a single filtered read per sample, not five. It stops once a step is
        // under a hundredth of a pixel.
        float a = 0.0, b = 0.0, c = 0.0, sx = 0.0, sy = 0.0, sw = 0.0;
        float gxs[16], gys[16];
        int2 hi = int2(size) - 1;

        [unroll] for (int j = 0; j < 4; ++j)
            [unroll] for (int i = 0; i < 4; ++i)
            {
                int2 q = clamp(p + int2(2 * i - 3, 2 * j - 3), 0, hi);
                float gx = 0.5 * (CurLuma.Load(int3(clamp(q + int2(1, 0), 0, hi), 0)) -
                                  CurLuma.Load(int3(clamp(q - int2(1, 0), 0, hi), 0)));
                float gy = 0.5 * (CurLuma.Load(int3(clamp(q + int2(0, 1), 0, hi), 0)) -
                                  CurLuma.Load(int3(clamp(q - int2(0, 1), 0, hi), 0)));
                float wk = w[j * 4 + i];

                gxs[j * 4 + i] = gx;
                gys[j * 4 + i] = gy;
                a += wk * gx * gx;
                b += wk * gx * gy;
                c += wk * gy * gy;
                sx += wk * gx;
                sy += wk * gy;
                sw += wk;
            }

        const float det = a * c - b * b;
        const float weak = 0.5 * ((a + c) - sqrt((a - c) * (a - c) + 4.0 * b * b));
        confidence = weak / (weak + knee);

        // A window with no structure across (flat, or one straight edge) gives no step: its reads are skipped. Written as the
        // negation so a NaN determinant runs the steps, as the break on `det <= 1e-9` it replaces did.
        [loop] for (int iteration = 0; iteration < 3 && !(det <= 1e-9); ++iteration)
        {
            float e = 0.0, f = 0.0, sr = 0.0;

            [unroll] for (int j = 0; j < 4; ++j)
                [unroll] for (int i = 0; i < 4; ++i)
                {
                    int2 q = clamp(p + int2(2 * i - 3, 2 * j - 3), 0, hi);
                    float r = cur[j * 4 + i] - Sample(float2(q + bestD) + sub);
                    float wk = w[j * 4 + i];

                    e += wk * gxs[j * 4 + i] * r;
                    f += wk * gys[j * 4 + i] * r;
                    sr += wk * r;
                }

            const float meanResidual = sr / sw;
            e -= sx * meanResidual;
            f -= sy * meanResidual;

            const float2 step = float2(c * e - b * f, a * f - b * e) / det;
            sub = clamp(sub + step, -1.5, 1.5);

            if (max(abs(step.x), abs(step.y)) < 0.01)
                break;
        }
    }
    else
    [loop] for (int iteration = 0; iteration < 3; ++iteration)
    {
        float a = 0.0, b = 0.0, c = 0.0, e = 0.0, f = 0.0;
        float sx = 0.0, sy = 0.0, sr = 0.0, sw = 0.0;

        [unroll] for (int j = -3; j <= 3; j += 2)
            [unroll] for (int i = -3; i <= 3; i += 2)
            {
                int2 q = clamp(p + int2(i, j), 0, int2(size) - 1);
                float2 at = float2(q + bestD) + sub;
                float centre = Sample(at);
                float gx = 0.5 * (Sample(at + float2(1, 0)) - Sample(at - float2(1, 0)));
                float gy = 0.5 * (Sample(at + float2(0, 1)) - Sample(at - float2(0, 1)));
                float r = CurLuma.Load(int3(q, 0)) - centre;
                float wk = w[((j + 3) / 2) * 4 + (i + 3) / 2];

                a += wk * gx * gx;
                b += wk * gx * gy;
                c += wk * gy * gy;
                e += wk * gx * r;
                f += wk * gy * r;
                sx += wk * gx;
                sy += wk * gy;
                sr += wk * r;
                sw += wk;
            }

        // Only the difference is taken about its mean: centring the gradients too fits one more unknown to sixteen samples,
        // which costs accuracy in grain.
        const float meanResidual = sr / sw;
        e -= sx * meanResidual;
        f -= sy * meanResidual;

        float det = a * c - b * b;

        if (det > 1e-9)
            sub = clamp(sub + float2(c * e - b * f, a * f - b * e) / det, -1.5, 1.5);

        // The weaker direction of the picture's structure in the window: a match along an edge, or in a flat area, is not sure
        // across it.
        float weak = 0.5 * ((a + c) - sqrt((a - c) * (a - c) + 4.0 * b * b));
        confidence = weak / (weak + knee);
    }

    OutFlow[id.xy] = float4(float2(bestD) + sub, confidence, 1.0);
}

)HLSL" // the compiler limits one string literal to 16 KB; the source goes on in a second one
                      R"HLSL(
// A 3x3 median of each component, which removes the odd wrong block, scaled to full-resolution pixels.
void Sort(inout float a, inout float b)
{
    float lo = min(a, b);
    b = max(a, b);
    a = lo;
}

float Median9(float v[9])
{
    // a sorting network for nine values, the middle one is the median
    Sort(v[0], v[1]); Sort(v[3], v[4]); Sort(v[6], v[7]);
    Sort(v[1], v[2]); Sort(v[4], v[5]); Sort(v[7], v[8]);
    Sort(v[0], v[1]); Sort(v[3], v[4]); Sort(v[6], v[7]);
    Sort(v[0], v[3]); Sort(v[3], v[6]); Sort(v[0], v[3]);
    Sort(v[1], v[4]); Sort(v[4], v[7]); Sort(v[1], v[4]);
    Sort(v[2], v[5]); Sort(v[5], v[8]); Sort(v[2], v[5]);
    Sort(v[2], v[4]); Sort(v[4], v[6]); Sort(v[2], v[4]);
    return v[4];
}


// What the whole frame did: the middle of the finished flow on a coarse grid, each answer counted by how sure its match was
// (each component on its own, so one moving thing does not drag it), skipping sky when there is depth. A flat picture's answers
// carry no weight, so a few things on it speak for the rest. Written to a 1x1 texture, z = 1 only when most of that weight
// agrees with the middle: then the picture moved as one (the camera); when it did not (one thing moving over a still
// scene), there is no whole-picture motion to offer. One group, a thread per grid sample: each finds its weight below its
// own value (ties by position), and the sample whose weight straddles half of the total is the middle.
groupshared float3 gSample[128]; // x, y, weight
groupshared float2 gMiddle;

[numthreads(128, 1, 1)]
void Global(uint3 id : SV_DispatchThreadID)
{
    // This thread's cell of a 16x8 grid, represented by its most structured answer of an 8x8 spread: a point sample would
    // mostly land on flat ground and miss the few things that say how the scene moved.
    uint t = id.x;
    float4 f = 0.0;
    float weight = -1.0;

    for (uint s = 0; s < 64; ++s)
    {
        float2 uv = float2((t % 16 + (s % 8 + 0.5) / 8.0) / 16.0, (t / 16 + (s / 8 + 0.5) / 8.0) / 8.0);
        float4 here = FlowIn.Load(int3(min(int2(uv * float2(aux)), int2(aux) - 1), 0));
        float w = here.z;

        if (depthMatching != 0)
        {
            float d = SceneDepth.Load(int3(min(int2(uv * float2(depthSize)), int2(depthSize) - 1), 0));

            if (reversed != 0 ? d <= 1e-6 : d >= 0.999999)
                w = 0.0;
        }

        if (w > weight)
        {
            weight = w;
            f = here;
        }
    }

    gSample[t] = float3(f.xy, weight);
    if (t == 0)
        gMiddle = 0.0; // in case rounding leaves no sample straddling the half
    GroupMemoryBarrierWithGroupSync();

    float total = 0.0;
    float2 below = 0.0;

    for (uint k = 0; k < 128; ++k)
    {
        float3 other = gSample[k];
        total += other.z;
        below.x += (other.x < f.x || (other.x == f.x && k < t)) ? other.z : 0.0;
        below.y += (other.y < f.y || (other.y == f.y && k < t)) ? other.z : 0.0;
    }

    float middle = 0.5 * total;

    if (below.x < middle && middle <= below.x + weight)
        gMiddle.x = f.x;

    if (below.y < middle && middle <= below.y + weight)
        gMiddle.y = f.y;

    GroupMemoryBarrierWithGroupSync();

    if (t == 0)
    {
        // The weight within a pixel and a half (and a tenth of the motion) of the middle.
        float agree = 0.0;
        float allowed = 1.5 + 0.1 * length(gMiddle);

        for (uint k = 0; k < 128; ++k)
            agree += length(gSample[k].xy - gMiddle) <= allowed ? gSample[k].z : 0.0;

        const bool camera = total >= 0.1 && agree >= 0.75 * total;
        OutFlow[uint2(0, 0)] = camera ? float4(gMiddle, 1.0, total) : float4(0.0, 0.0, 0.0, total);
    }
}

// Hue for the direction, brightness for the speed (scale is the speed that is full brightness).
[numthreads(8, 8, 1)]
void Visualise(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    float2 f = FlowIn.Load(int3(id.xy, 0)).xy;
    float hue = atan2(f.y, f.x) / 6.2831853 + 0.5;
    float3 rgb = saturate(abs(frac(hue + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
    float level = saturate(length(f) / scale);

    OutFlow[id.xy] = float4(rgb * level, 1.0);
}

[numthreads(8, 8, 1)]
void Median(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    float xs[9], ys[9];
    int k = 0;
    int2 hi = int2(size) - 1;

    [unroll] for (int j = -1; j <= 1; ++j)
        [unroll] for (int i = -1; i <= 1; ++i)
        {
            float2 f = FlowIn.Load(int3(clamp(int2(id.xy) + int2(i, j), 0, hi), 0)).xy;
            xs[k] = f.x;
            ys[k] = f.y;
            ++k;
        }

    OutFlow[id.xy] = float4(Median9(xs) * scale, Median9(ys) * scale, FlowIn.Load(int3(id.xy, 0)).z, 1.0);
}

// Edge-aware smoothing: the average of the neighbours' motion, each counted by how sure its match was, how close its motion
// is to this pixel's and how close its brightness is, so noise in a flat area is averaged out and the edge of something that
// moves differently is not. The group's 8x8 pixels and the radius (at most 4) around them are read once into a 16x16 tile that
// the group's threads share, with the picture's edge repeated outward.
groupshared float4 gTile[256]; // x, y and confidence of the flow, and the guide's luma

[numthreads(8, 8, 1)]
void Smooth(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID)
{
    // Off: only the centre counts, so the tile is not filled (the radius is the same for the whole group, so every thread
    // leaves here or none does).
    if (radius == 0)
    {
        if (id.x < size.x && id.y < size.y)
        {
            // Rounded as the full pass rounds it (precise keeps the compiler from folding the weight away).
            float3 only = FlowIn.Load(int3(id.xy, 0)).xyz;
            precise float weight = only.z + 0.05;
            precise float2 weighted = only.xy * weight;
            OutFlow[id.xy] = float4(weighted / weight, only.z, 1.0);
        }

        return;
    }

    int2 hi = int2(size) - 1;
    int2 origin = int2(group.xy) * 8 - 4;
    uint t = local.y * 8 + local.x;

    // Every thread loads four of the tile's entries, including those outside the picture, which must still reach the barrier.
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        uint e = t + k * 64;
        int2 tile = int2(e % 16, e / 16);
        int2 q = clamp(origin + tile, 0, hi);
        gTile[e] = float4(FlowIn.Load(int3(q, 0)).xyz, GuideLuma.Load(int3(q, 0)));
    }

    GroupMemoryBarrierWithGroupSync();

    if (id.x >= size.x || id.y >= size.y)
        return;

    int2 at = int2(local.xy) + 4;
    float4 here = gTile[at.y * 16 + at.x];
    float3 centre = here.xyz;
    float lumaCentre = here.w;

    float2 sum = centre.xy * (centre.z + 0.05);
    float total = centre.z + 0.05;
    float motionRange = 1.0 + 0.02 * dot(centre.xy, centre.xy);
    float lumaRange = 0.15 * (lumaCentre + 0.05);

    [unroll] for (int j = -4; j <= 4; ++j)
        [unroll] for (int i = -4; i <= 4; ++i)
        {
            if ((i == 0 && j == 0) || abs(i) > radius || abs(j) > radius)
                continue;

            int2 q = at + int2(i, j);
            float4 other = gTile[q.y * 16 + q.x];
            float2 df = other.xy - centre.xy;
            float dl = (other.w - lumaCentre) / lumaRange;
            float w = (other.z + 0.05) * exp(-dot(df, df) / motionRange - dl * dl);
            sum += other.xy * w;
            total += w;
        }

    OutFlow[id.xy] = float4(sum / total, centre.z, 1.0);
}
)HLSL";

// The scene-cut detector: brightness histograms of the half-resolution luma in a 3x3 grid, compared with the last
// frame's.
const char* kSceneSource = R"HLSL(
cbuffer S : register(b0)
{
    uint2 size;       // the luma's size
    uint hasPrevious; // the last frame's smoothed histograms are in the state, to compare with
    float threshold;  // the divergence past which the frame is a cut
};

Texture2D<float> Luma : register(t0);
RWTexture2D<uint> State : register(u0); // x is the bin; row t (0..8) holds tile t's counts, row 9 + t its last smoothed histogram
                                        // (float bits), row 18 the scratch: a sum for each shift, and at x = 255 the count of
                                        // finished groups
RWTexture2D<uint> Cut : register(u1);   // texel 0 the flag, texel 1 the divergence (float bits)

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
        State[uint2(255, 18)] = 0;
    }
}
)HLSL";

ID3DBlob* Compile(const char* entry, std::string* error, const char* source = kSource,
                  const D3D_SHADER_MACRO* macros = nullptr)
{
    ID3DBlob* code = nullptr;
    ID3DBlob* messages = nullptr;

    const HRESULT hr = D3DCompile(source, strlen(source), "OpticalFlow", macros, nullptr, entry, "cs_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages);

    if (FAILED(hr))
    {
        *error = std::string("compiling ") + entry + ": " +
                 (messages != nullptr ? (const char*) messages->GetBufferPointer() : "no message");
        if (messages != nullptr)
            messages->Release();
        return nullptr;
    }

    if (messages != nullptr)
        messages->Release();

    return code;
}

} // namespace

OpticalFlowDx12::~OpticalFlowDx12()
{
    ReleaseTextures();

    for (ID3D12PipelineState** pso : { &_luma, &_lumaSdr, &_lumaScRgb, &_lumaPq, &_down, &_match, &_median, &_smooth,
                                       &_visualise, &_global, &_sceneHist, &_sceneDiverge })
        if (*pso != nullptr)
            (*pso)->Release();

    if (_rootSignature != nullptr)
        _rootSignature->Release();
    if (_sceneRoot != nullptr)
        _sceneRoot->Release();
    if (_heap != nullptr)
        _heap->Release();
}

bool OpticalFlowDx12::Init(ID3D12Device* device)
{
    if (device == nullptr)
    {
        _error = "no device";
        return false;
    }

    _device = device;

    // One table (seven SRVs, one UAV) and the root constants.
    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 7;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 7;

    D3D12_ROOT_PARAMETER params[2] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = sizeof(Constants) / 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc {};
    rootDesc.NumParameters = 2;
    rootDesc.pParameters = params;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &sampler;

    ID3DBlob* serialized = nullptr;
    ID3DBlob* messages = nullptr;

    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &messages)))
    {
        _error = "serializing the root signature";
        if (messages != nullptr)
            messages->Release();
        return false;
    }

    const HRESULT rootResult = device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                           serialized->GetBufferSize(), IID_PPV_ARGS(&_rootSignature));
    serialized->Release();

    if (messages != nullptr)
        messages->Release();

    if (FAILED(rootResult))
    {
        _error = "creating the root signature";
        return false;
    }

    struct Entry
    {
        const char* name;
        ID3D12PipelineState** target;
    };

    for (const Entry& entry : { Entry { "Luma", &_luma }, Entry { "Down", &_down }, Entry { "Match", &_match },
                                Entry { "Median", &_median }, Entry { "Smooth", &_smooth },
                                Entry { "Visualise", &_visualise }, Entry { "Global", &_global } })
    {
        ID3DBlob* code = Compile(entry.name, &_error);

        if (code == nullptr)
            return false;

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso {};
        pso.pRootSignature = _rootSignature;
        pso.CS = { code->GetBufferPointer(), code->GetBufferSize() };

        const HRESULT hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(entry.target));
        code->Release();

        if (FAILED(hr))
        {
            _error = std::string("creating the pipeline ") + entry.name;
            return false;
        }
    }

    // The luma in the three other ways (see LumaOf): the same entry point built with another LUMA_MODE.
    {
        struct Variant
        {
            const char* mode;
            ID3D12PipelineState** target;
        };

        for (const Variant& variant :
             { Variant { "1", &_lumaSdr }, Variant { "2", &_lumaScRgb }, Variant { "3", &_lumaPq } })
        {
            const D3D_SHADER_MACRO macros[] = { { "LUMA_MODE", variant.mode }, { nullptr, nullptr } };
            ID3DBlob* code = Compile("Luma", &_error, kSource, macros);

            if (code == nullptr)
                return false;

            D3D12_COMPUTE_PIPELINE_STATE_DESC pso {};
            pso.pRootSignature = _rootSignature;
            pso.CS = { code->GetBufferPointer(), code->GetBufferSize() };

            const HRESULT hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(variant.target));
            code->Release();

            if (FAILED(hr))
            {
                _error = "creating the pipeline Luma";
                return false;
            }
        }
    }

    // The scene-cut passes: the luma (one SRV), the state and the flag (two UAVs) and their own constants.
    {
        D3D12_DESCRIPTOR_RANGE sceneRanges[2] {};
        sceneRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        sceneRanges[0].NumDescriptors = 1;
        sceneRanges[0].BaseShaderRegister = 0;
        sceneRanges[0].OffsetInDescriptorsFromTableStart = 0;
        sceneRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        sceneRanges[1].NumDescriptors = 2;
        sceneRanges[1].BaseShaderRegister = 0;
        sceneRanges[1].OffsetInDescriptorsFromTableStart = 1;

        D3D12_ROOT_PARAMETER sceneParams[2] {};
        sceneParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        sceneParams[0].DescriptorTable.NumDescriptorRanges = 2;
        sceneParams[0].DescriptorTable.pDescriptorRanges = sceneRanges;
        sceneParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        sceneParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        sceneParams[1].Constants.ShaderRegister = 0;
        sceneParams[1].Constants.Num32BitValues = sizeof(SceneConstants) / 4;
        sceneParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC sceneDesc {};
        sceneDesc.NumParameters = 2;
        sceneDesc.pParameters = sceneParams;

        ID3DBlob* sceneSerialized = nullptr;
        ID3DBlob* sceneMessages = nullptr;
        HRESULT sceneResult =
            D3D12SerializeRootSignature(&sceneDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sceneSerialized, &sceneMessages);

        if (SUCCEEDED(sceneResult))
            sceneResult = device->CreateRootSignature(0, sceneSerialized->GetBufferPointer(),
                                                      sceneSerialized->GetBufferSize(), IID_PPV_ARGS(&_sceneRoot));

        if (sceneSerialized != nullptr)
            sceneSerialized->Release();
        if (sceneMessages != nullptr)
            sceneMessages->Release();

        if (FAILED(sceneResult))
        {
            _error = "creating the scene-cut root signature";
            return false;
        }

        for (const Entry& entry : { Entry { "SceneHist", &_sceneHist }, Entry { "SceneDiverge", &_sceneDiverge } })
        {
            ID3DBlob* code = Compile(entry.name, &_error, kSceneSource);

            if (code == nullptr)
                return false;

            D3D12_COMPUTE_PIPELINE_STATE_DESC pso {};
            pso.pRootSignature = _sceneRoot;
            pso.CS = { code->GetBufferPointer(), code->GetBufferSize() };

            const HRESULT hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(entry.target));
            code->Release();

            if (FAILED(hr))
            {
                _error = std::string("creating the pipeline ") + entry.name;
                return false;
            }
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap {};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = kDescriptorsPerPass * kPassesPerFrame * kFramesInFlight;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&_heap))))
    {
        _error = "creating the descriptor heap";
        return false;
    }

    _descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

bool OpticalFlowDx12::CreateTexture(Tex& tex, uint32_t width, uint32_t height, DXGI_FORMAT format, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    tex.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    if (FAILED(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, tex.state, nullptr,
                                                IID_PPV_ARGS(&tex.resource))))
        return false;

    tex.resource->SetName(name);
    tex.width = width;
    tex.height = height;
    return true;
}

void OpticalFlowDx12::ReleaseTextures()
{
    auto release = [](Tex& tex)
    {
        if (tex.resource != nullptr)
            tex.resource->Release();
        tex = Tex {};
    };

    for (auto& set : _pyramid)
        for (auto& tex : set)
            release(tex);

    for (auto& set : _levelFlow)
        for (auto& tex : set)
            release(tex);

    release(_flowMedian);
    release(_globalFlow);
    release(_flow);
    release(_preview);
    release(_sceneState);
    release(_cutFlag);
}

bool OpticalFlowDx12::EnsureSize(uint32_t width, uint32_t height)
{
    if (width == _width && height == _height && _flow.resource != nullptr)
        return true;

    // The previous work may still be reading these; a size change is rare (a resolution change) and the caller is expected
    // to have waited, as it does before replacing its own targets.
    ReleaseTextures();
    _havePrevious = false;
    _globalReady = false;
    _scenePrevValid = false;
    _sceneCutRan = false;
    _flowValid = false;
    _width = width;
    _height = height;

    uint32_t w = (width + 1) / 2;
    uint32_t h = (height + 1) / 2;

    for (int level = 0; level < kLevels; ++level)
    {
        for (int set = 0; set < 2; ++set)
            if (!CreateTexture(_pyramid[set][level], w, h, kLumaFormat, L"OpticalFlow_Luma"))
                return false;

        for (int set = 0; set < 2; ++set)
            if (!CreateTexture(_levelFlow[set][level], w, h, kFlowFormat, L"OpticalFlow_LevelFlow"))
                return false;

        w = (w + 1) / 2;
        h = (h + 1) / 2;
    }

    return CreateTexture(_sceneState, 256, kSceneStateRows, kStateFormat, L"OpticalFlow_SceneState") &&
           CreateTexture(_cutFlag, 2, 1, kStateFormat, L"OpticalFlow_SceneCut") &&
           CreateTexture(_flowMedian, (width + 1) / 2, (height + 1) / 2, kFlowFormat, L"OpticalFlow_FlowMedian") &&
           CreateTexture(_globalFlow, 1, 1, kFlowFormat, L"OpticalFlow_Global") &&
           CreateTexture(_flow, (width + 1) / 2, (height + 1) / 2, kFlowFormat, L"OpticalFlow_Flow") &&
           CreateTexture(_preview, (width + 1) / 2, (height + 1) / 2, DXGI_FORMAT_R8G8B8A8_UNORM,
                         L"OpticalFlow_Preview");
}

void OpticalFlowDx12::Transition(ID3D12GraphicsCommandList* list, Tex& tex, D3D12_RESOURCE_STATES state)
{
    if (tex.state == state)
        return;

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = tex.resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = tex.state;
    barrier.Transition.StateAfter = state;
    list->ResourceBarrier(1, &barrier);
    tex.state = state;
}

void OpticalFlowDx12::Pass(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, ID3D12Resource* src0,
                           DXGI_FORMAT format0, ID3D12Resource* src1, DXGI_FORMAT format1, ID3D12Resource* src2,
                           DXGI_FORMAT format2, Tex& dst, DXGI_FORMAT dstFormat, const Constants& constants,
                           ID3D12Resource* src3, DXGI_FORMAT format3, ID3D12Resource* src4, DXGI_FORMAT format4,
                           ID3D12Resource* src5, DXGI_FORMAT format5, ID3D12Resource* src6, DXGI_FORMAT format6)
{
    Transition(list, dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    StampBegin(list);

    const UINT first = _heapCursor;
    _heapCursor = (_heapCursor + kDescriptorsPerPass) % (kDescriptorsPerPass * kPassesPerFrame * kFramesInFlight);

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = _heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T) first * _descriptorSize;
    gpu.ptr += (UINT64) first * _descriptorSize;

    ID3D12Resource* sources[7] = { src0,
                                   src1 != nullptr ? src1 : src0,
                                   src2 != nullptr ? src2 : src0,
                                   src3 != nullptr ? src3 : src0,
                                   src4 != nullptr ? src4 : src0,
                                   src5 != nullptr ? src5 : src0,
                                   src6 != nullptr ? src6 : src0 };
    const DXGI_FORMAT formats[7] = { format0,
                                     src1 != nullptr ? format1 : format0,
                                     src2 != nullptr ? format2 : format0,
                                     src3 != nullptr ? format3 : format0,
                                     src4 != nullptr ? format4 : format0,
                                     src5 != nullptr ? format5 : format0,
                                     src6 != nullptr ? format6 : format0 };

    for (int i = 0; i < 7; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = formats[i];
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        _device->CreateShaderResourceView(sources[i], &srv, cpu);
        cpu.ptr += _descriptorSize;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.Format = dstFormat;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    _device->CreateUnorderedAccessView(dst.resource, nullptr, &uav, cpu);

    ID3D12DescriptorHeap* heaps[] = { _heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(_rootSignature);
    list->SetPipelineState(pso);
    list->SetComputeRootDescriptorTable(0, gpu);
    list->SetComputeRoot32BitConstants(1, sizeof(Constants) / 4, &constants, 0);
    list->Dispatch((dst.width + 7) / 8, (dst.height + 7) / 8, 1);

    // Anything that reads it next reads it as a texture.
    Transition(list, dst, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    StampEnd(list, pso);
}

// The test's timing: a timestamp before the first pass of a frame, then one after each (the barrier at the end of a
// pass is in it).
void OpticalFlowDx12::StampBegin(ID3D12GraphicsCommandList* list)
{
    if (_timeHeap != nullptr && _timeCount == 0 && _timeCapacity >= 2 && _timeCapacity <= 64)
    {
        list->EndQuery(_timeHeap, D3D12_QUERY_TYPE_TIMESTAMP, _timeCount);
        _timeNames[_timeCount++] = "start";
    }
}

void OpticalFlowDx12::StampEnd(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso)
{
    if (_timeHeap == nullptr || _timeCount == 0 || _timeCount >= _timeCapacity || _timeCount >= 64)
        return;

    list->EndQuery(_timeHeap, D3D12_QUERY_TYPE_TIMESTAMP, _timeCount);
    _timeNames[_timeCount++] = pso == _luma || pso == _lumaSdr || pso == _lumaScRgb || pso == _lumaPq ? "luma"
                               : pso == _down                                                         ? "down"
                               : pso == _match                                                        ? "match"
                               : pso == _median                                                       ? "median"
                               : pso == _smooth                                                       ? "smooth"
                               : pso == _global                                                       ? "global"
                               : pso == _sceneHist                                                    ? "scenehist"
                               : pso == _sceneDiverge                                                 ? "scenecut"
                                                                                                      : "other";
}

void OpticalFlowDx12::ScenePass(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, ID3D12Resource* luma,
                                uint32_t groupsX, uint32_t groupsY, const SceneConstants& constants)
{
    StampBegin(list);

    // The state stays a UAV for good (the two passes only write it); the flag is one while the passes run and a texture
    // after.
    Transition(list, _sceneState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list, _cutFlag, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const UINT first = _heapCursor;
    _heapCursor = (_heapCursor + kDescriptorsPerPass) % (kDescriptorsPerPass * kPassesPerFrame * kFramesInFlight);

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = _heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T) first * _descriptorSize;
    gpu.ptr += (UINT64) first * _descriptorSize;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = kLumaFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    _device->CreateShaderResourceView(luma, &srv, cpu);
    cpu.ptr += _descriptorSize;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.Format = kStateFormat;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    _device->CreateUnorderedAccessView(_sceneState.resource, nullptr, &uav, cpu);
    cpu.ptr += _descriptorSize;
    _device->CreateUnorderedAccessView(_cutFlag.resource, nullptr, &uav, cpu);

    ID3D12DescriptorHeap* heaps[] = { _heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(_sceneRoot);
    list->SetPipelineState(pso);
    list->SetComputeRootDescriptorTable(0, gpu);
    list->SetComputeRoot32BitConstants(1, sizeof(SceneConstants) / 4, &constants, 0);
    list->Dispatch(groupsX, groupsY, 1);

    // The next pass reads what this one wrote.
    D3D12_RESOURCE_BARRIER barriers[2] {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = _sceneState.resource;
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[1].UAV.pResource = _cutFlag.resource;
    list->ResourceBarrier(2, barriers);

    StampEnd(list, pso);
}

bool OpticalFlowDx12::Dispatch(ID3D12GraphicsCommandList* list, ID3D12Resource* color, DXGI_FORMAT colorFormat,
                               ID3D12Resource* depth, DXGI_FORMAT depthFormat, bool depthReversed, Encoding encoding)
{
    if (_device == nullptr || _match == nullptr || list == nullptr || color == nullptr)
        return false;

    const D3D12_RESOURCE_DESC colorDesc = color->GetDesc();

    if (!EnsureSize((uint32_t) colorDesc.Width, colorDesc.Height))
    {
        _error = "creating the textures";
        return false;
    }

    // Build the current frame's pyramid.
    auto& current = _pyramid[_current];
    auto& previous = _pyramid[1 - _current];

    Constants constants {};
    constants.sizeX = current[0].width;
    constants.sizeY = current[0].height;
    constants.auxX = (uint32_t) colorDesc.Width;
    constants.auxY = colorDesc.Height;
    constants.whiteNits = (std::max)(_settings.hdrWhiteNits, 1.0f);
    Pass(list,
         !_settings.perceptualLuma     ? _luma
         : encoding == Encoding::ScRgb ? _lumaScRgb
         : encoding == Encoding::Pq    ? _lumaPq
                                       : _lumaSdr,
         color, colorFormat, nullptr, DXGI_FORMAT_UNKNOWN, nullptr, DXGI_FORMAT_UNKNOWN, current[0], kLumaFormat,
         constants);

    // Is this frame a hard cut from the last one? Decided here, on the GPU, so the match and the trust mask can act on
    // it in this very frame.
    _sceneCutRan = false;

    if (_settings.sceneCutDetector)
    {
        SceneConstants scene {};
        scene.sizeX = current[0].width;
        scene.sizeY = current[0].height;
        scene.hasPrevious = _scenePrevValid ? 1 : 0;
        scene.threshold = _settings.sceneCutThreshold;
        ScenePass(list, _sceneHist, current[0].resource, 8, 9, scene);
        ScenePass(list, _sceneDiverge, current[0].resource, 9, 1, scene);
        Transition(list, _cutFlag, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        _sceneCutRan = true;
        _scenePrevValid = true;
    }
    else
        _scenePrevValid = false;

    for (int level = 1; level < kLevels; ++level)
    {
        constants.sizeX = current[level].width;
        constants.sizeY = current[level].height;
        constants.auxX = current[level - 1].width;
        constants.auxY = current[level - 1].height;
        Pass(list, _down, current[level - 1].resource, kLumaFormat, nullptr, DXGI_FORMAT_UNKNOWN, nullptr,
             DXGI_FORMAT_UNKNOWN, current[level], kLumaFormat, constants);
    }

    const bool history = _flowValid; // the last frame made a flow: its levels are candidates
    _flowValid = false;
    auto& levelNow = _levelFlow[_current];
    auto& levelBefore = _levelFlow[1 - _current];

    // Depth-aware matching, when there is depth.
    const bool depthMatching = _settings.depthMatching && depth != nullptr && depthFormat != DXGI_FORMAT_UNKNOWN;
    _usedDepth = depthMatching;
    const D3D12_RESOURCE_DESC depthDesc = depthMatching ? depth->GetDesc() : D3D12_RESOURCE_DESC {};

    if (_havePrevious)
    {
        for (int level = kLevels - 1; level >= 0; --level)
        {
            const bool coarsest = level == kLevels - 1;

            constants = Constants {};
            constants.sizeX = current[level].width;
            constants.sizeY = current[level].height;
            constants.radius = coarsest ? _settings.coarseRadius : _settings.radius;
            constants.hasPrediction = coarsest ? 0 : 1;
            constants.lambda = _settings.lambda;
            constants.hasHistory = (history && _settings.useHistory) ? 1 : 0;
            constants.coarseCells = (uint32_t) std::clamp(_settings.coarseCells, 1, 9);
            constants.knee = _settings.confidenceKnee;
            constants.depthMatching = depthMatching ? 1 : 0;
            constants.depthX = depthMatching ? (uint32_t) depthDesc.Width : 1;
            constants.depthY = depthMatching ? depthDesc.Height : 1;
            constants.reversed = depthReversed ? 1 : 0;
            constants.hasGlobal = _globalReady ? 1 : 0;
            constants.inverseRefinement = _settings.inverseRefinement ? 1 : 0;
            constants.sceneCutEnabled = _sceneCutRan ? 1 : 0;
            constants.zeroMargin = level == 0 ? _settings.zeroMargin : 0.0f;
            constants.zeroReach = (uint32_t) std::clamp(_settings.zeroReach, 0, 64);
            constants.scale = 1.0f / (float) (2 << level); // full-resolution pixels in this level's

            if (!coarsest)
            {
                constants.auxX = current[level + 1].width;
                constants.auxY = current[level + 1].height;
            }

            Pass(list, _match, current[level].resource, kLumaFormat, previous[level].resource, kLumaFormat,
                 coarsest ? nullptr : levelNow[level + 1].resource, kFlowFormat, levelNow[level], kFlowFormat,
                 constants, (history && _settings.useHistory) ? levelBefore[level].resource : nullptr, kFlowFormat,
                 depthMatching ? depth : nullptr, depthFormat, _globalReady ? _globalFlow.resource : nullptr,
                 kFlowFormat, _sceneCutRan ? _cutFlag.resource : nullptr, kStateFormat);
        }

        constants = Constants {};
        constants.sizeX = _flow.width;
        constants.sizeY = _flow.height;
        constants.scale = 2.0f; // the half-resolution level's pixels to full-resolution ones
        Pass(list, _median, levelNow[0].resource, kFlowFormat, nullptr, DXGI_FORMAT_UNKNOWN, nullptr,
             DXGI_FORMAT_UNKNOWN, _flowMedian, kFlowFormat, constants);

        constants = Constants {};
        constants.sizeX = _flow.width;
        constants.sizeY = _flow.height;
        constants.radius = std::clamp(_settings.smoothRadius, 0, 4); // the shader's tile holds at most four pixels around a group
        Pass(list, _smooth, _flowMedian.resource, kFlowFormat, current[0].resource, kLumaFormat, nullptr,
             DXGI_FORMAT_UNKNOWN, _flow, kFlowFormat, constants);

        if (_settings.globalCandidate)
        {
            // What the whole frame did, for the next one to try everywhere (see Global).
            constants = Constants {};
            constants.sizeX = 1;
            constants.sizeY = 1;
            constants.auxX = _flow.width;
            constants.auxY = _flow.height;
            constants.depthMatching = depthMatching ? 1 : 0;
            constants.depthX = depthMatching ? (uint32_t) depthDesc.Width : 1;
            constants.depthY = depthMatching ? depthDesc.Height : 1;
            constants.reversed = depthReversed ? 1 : 0;
            Pass(list, _global, _flow.resource, kFlowFormat, nullptr, DXGI_FORMAT_UNKNOWN, nullptr, DXGI_FORMAT_UNKNOWN,
                 _globalFlow, kFlowFormat, constants, nullptr, DXGI_FORMAT_UNKNOWN, depthMatching ? depth : nullptr,
                 depthFormat);
        }

        _globalReady = _settings.globalCandidate;
        _flowValid = true;
    }

    // The flow can be read by a pixel shader too (the menu preview, a consumer's sampling).
    Transition(list, _flow,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    _current = 1 - _current;
    _havePrevious = true;
    return true;
}

bool OpticalFlowDx12::Visualise(ID3D12GraphicsCommandList* list, float maxSpeed)
{
    if (!_flowValid || _preview.resource == nullptr || list == nullptr)
        return false;

    // The flow rests in the combined read state; the pass wants the non-pixel one it was written to.
    Transition(list, _flow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    Constants constants {};
    constants.sizeX = _preview.width;
    constants.sizeY = _preview.height;
    constants.scale = maxSpeed;
    Pass(list, _visualise, _flow.resource, kFlowFormat, nullptr, DXGI_FORMAT_UNKNOWN, nullptr, DXGI_FORMAT_UNKNOWN,
         _preview, DXGI_FORMAT_R8G8B8A8_UNORM, constants);

    Transition(list, _flow,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(list, _preview, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}
