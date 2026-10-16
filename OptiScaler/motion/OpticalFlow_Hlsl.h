#pragma once

#include "Luma_Hlsl.h"
#include "MotionArrows_Hlsl.h"
#include "GeometricDebug_Hlsl.h"

// The optical flow's compute shaders (motion/OpticalFlow_Dx12.cpp). The scene-cut passes are in motion/SceneCut_Hlsl.h,
// and the luma both use is in motion/Luma_Hlsl.h.
// Compiled ahead of time into native/F5LowShaderBytecode.h by tests/nr_shader_bytecode_smoke.cpp (run it with --write
// after changing anything here; without, it checks the embedded bytecode is current).

namespace OpticalFlowHlsl
{
inline constexpr const char* kSource = R"HLSL(
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
    uint stillFrames;       // Match, finest level: keep the still age (0 = not); where the last age is this much or more the
                            // camera candidate must beat no motion by stillMargin
    uint hasAge;            // Match: the last frame's ages (LastAge, t7) are there to read
    float stillEpsilon;     // Match: the window did not change if its largest difference to the last frame is under this
    float stillMargin;      // Match: what the camera candidate must win by over no motion where the picture is still
    uint lookWeights;       // Match, finest level, no depth: the window's samples count by how near their luma is to the pixel's
    float lookRange;        // ... within this much luma
    float lookDistance;     // ... and a little less with their distance from it, in this level's pixels
};

SamplerState Linear : register(s0);
Texture2D<float4> Color : register(t0);
Texture2D<float>  CurLuma : register(t0);
Texture2D<float>  PrevLuma : register(t1);
Texture2D<float4> Prediction : register(t2);
Texture2D<float4> History : register(t3);
Texture2D<float>  GuideLuma : register(t1);
Texture2D<float4> DebugColor : register(t1);
Texture2D<float4> FlowIn : register(t0);
Texture2D<float>  GeoCurrentLuma : register(t1);
Texture2D<float>  GeoPreviousLuma : register(t4);
Texture2D<float4> GeoModel : register(t3);
Texture2D<float4> GeoInfo : register(t5);
Texture2D<float>  SceneDepth : register(t4);
Texture2D<float4> GlobalFlow : register(t5);
Texture2D<uint>   CutFlag : register(t6);
Texture2D<uint>   LastAge : register(t7);
RWTexture2D<float>  OutLuma : register(u0);
RWTexture2D<float4> OutFlow : register(u0);
RWTexture2D<uint>   OutAge : register(u1);
RWTexture2D<float4> OutModel : register(u0);
RWTexture2D<float4> OutInfo : register(u1);

)HLSL"
                      F5LOW_LUMA_HLSL
                      R"HLSL(// Luma of the colour, averaged over 2x2 into the first level.
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

// The biggest absolute difference to the last frame at the same place (no offset, the mean not taken out): the still age
// is told from it. Only the finest level asks. The window's sixteen samples lie at odd offsets, so the pixel itself and
// its even neighbours are checked too: a dot one pixel wide that moves by an even step would otherwise slip between the
// samples and be held still.
float LargestChange(int2 p, float cur[16])
{
    float largest = 0.0;
    int2 hi = int2(size) - 1;

    [unroll] for (int j = 0; j < 4; ++j)
        [unroll] for (int i = 0; i < 4; ++i)
            largest = max(largest, abs(cur[j * 4 + i] - PrevLuma.Load(int3(clamp(p + int2(2 * i - 3, 2 * j - 3), 0, hi), 0))));

    static const int2 kEven[5] = { int2(0, 0), int2(-2, 0), int2(2, 0), int2(0, -2), int2(0, 2) };

    [unroll] for (int k = 0; k < 5; ++k)
    {
        int2 q = clamp(p + kEven[k], 0, hi);
        largest = max(largest, abs(CurLuma.Load(int3(q, 0)) - PrevLuma.Load(int3(q, 0))));
    }

    return largest;
}
)HLSL" // the compiler limits one string literal to 16 KB; the source goes on in a second one
                      R"HLSL(
// How much a sample with luma l counts beside the centre pixel's lc: about 1 where the look is alike, little across an edge
// that changes it, and a little less with the distance from the centre (offset, in pixels of this level).
float LookWeight(float l, float lc, float2 offset)
{
    float dl = (l - lc) / lookRange;
    return max(exp(-dl * dl - 0.5 * dot(offset, offset) / (lookDistance * lookDistance)), 0.02);
}
)HLSL"
                      R"HLSL(

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

        if (stillFrames != 0)
            OutAge[id.xy] = 0; // the picture is another one: nothing has been still yet

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

    // On the finest level the samples also count by how near their brightness is to this pixel's: without depth the depth
    // weights' counterpart, so the edge of a thing that looks different does not decide the match beside it, and with depth
    // on top of them, which keeps a misaligned depth from doing the same. The weights are then scaled back to the sum they
    // had: the cost and the sub-pixel step do not change with a common scale, but the confidence below is a sum over the
    // weights, and so its knee keeps the meaning it has with the depth weights alone.
    if (lookWeights != 0)
    {
        const float lc = CurLuma.Load(int3(p, 0));
        float before = 0.0, after = 0.0;

        [unroll] for (int lj = 0; lj < 4; ++lj)
            [unroll] for (int li = 0; li < 4; ++li)
            {
                before += w[lj * 4 + li];
                w[lj * 4 + li] *= LookWeight(cur[lj * 4 + li], lc, float2(2 * li - 3, 2 * lj - 3));
                after += w[lj * 4 + li];
            }

        [unroll] for (int ls = 0; ls < 16; ++ls)
            w[ls] *= before / after;
    }

    // The candidates for where to search: no motion, the coarser level's answer at the cells around this pixel (doubled, it is
    // in this level's pixels) and the last frame's flow here. The one that matches
    // best is where the search starts, so a steady pan carries over from frame to frame and an edge is not stuck with the
    // answer of a cell that lies across it.
    int2 centre = 0;
    float start = Cost(p, centre, w, cur);

    // How long the picture has been unchanged here (finest level only): the age last frame, and from this frame's
    // difference at no offset the age now, written at the end. Where it has been still long enough, the camera below has
    // to beat no motion by a margin instead of winning the tie.
    uint lastAge = 0;
    float largest = 0.0;

    if (stillFrames != 0)
    {
        largest = LargestChange(p, cur);
        lastAge = hasAge != 0 ? LastAge.Load(int3(p, 0)) : 0;
    }

    const bool pictureStill = stillFrames != 0 && lastAge >= stillFrames;

    // What the whole picture did last frame (the camera): where nothing in the window says otherwise it wins the tie with no
    // motion, so a flat wall moves with the picture. `scale` here is this level's pixels per full-resolution pixel.
    if (hasGlobal != 0)
    {
        float4 g = GlobalFlow.Load(int3(0, 0, 0));

        if (g.z > 0.5)
        {
            int2 d = int2(round(g.xy * scale));
            float c = Cost(p, d, w, cur);

            if (pictureStill ? c + stillMargin < start : c <= start)
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
        float ca = 0.0, cb = 0.0, cc = 0.0;
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
                // Confidence is a separate texture-structure diagnostic. Do not
                // fold depth or current-luma support weights into it.
                ca += gx * gx;
                cb += gx * gy;
                cc += gy * gy;
                a += wk * gx * gx;
                b += wk * gx * gy;
                c += wk * gy * gy;
                sx += wk * gx;
                sy += wk * gy;
                sw += wk;
            }

        const float det = a * c - b * b;
        const float weak = 0.5 * ((ca + cc) - sqrt((ca - cc) * (ca - cc) + 4.0 * cb * cb));
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
        float ca = 0.0, cb = 0.0, cc = 0.0;
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

                // Keep confidence independent from the depth/luma weights used
                // by the matcher and LK update.
                ca += gx * gx;
                cb += gx * gy;
                cc += gy * gy;
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
        float weak = 0.5 * ((ca + cc) - sqrt((ca - cc) * (ca - cc) + 4.0 * cb * cb));
        confidence = weak / (weak + knee);
    }

    OutFlow[id.xy] = float4(float2(bestD) + sub, confidence, 1.0);

    if (stillFrames != 0)
        OutAge[id.xy] = largest < stillEpsilon ? min(lastAge + 1, 255u) : 0u;
}

)HLSL" // ... and on in a third one
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
void VisualiseLegacy(uint3 id : SV_DispatchThreadID)
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
)HLSL"
R"HLSL(
// Selective geometric fusion. The model maps normalized current-frame coordinates to the
// corresponding previous-frame coordinates. Its eight parameters are the free entries of
// H=[h0 h1 h2; h3 h4 h5; h6 h7 1]. Flow remains in full-resolution pixel units.
float GeoPhotoCost(float2 uv, float2 motionPx)
{
    float2 halfSize = floor((float2(aux) + 1.0) * 0.5);
    float2 texel = 1.0 / halfSize;
    float2 previousUv = uv + motionPx / float2(aux);
    if (any(previousUv < 0.0) || any(previousUv > 1.0))
        return 1e6;

    float currentMean = 0.0;
    float previousMean = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * texel;
            currentMean += GeoCurrentLuma.SampleLevel(Linear, uv + offset, 0.0);
            previousMean += GeoPreviousLuma.SampleLevel(Linear, previousUv + offset, 0.0);
        }
    currentMean /= 9.0;
    previousMean /= 9.0;

    float cost = 0.0;
    [unroll] for (int yy = -1; yy <= 1; ++yy)
        [unroll] for (int xx = -1; xx <= 1; ++xx)
        {
            float2 offset = float2(xx, yy) * texel;
            float a = GeoCurrentLuma.SampleLevel(Linear, uv + offset, 0.0) - currentMean;
            float b = GeoPreviousLuma.SampleLevel(Linear, previousUv + offset, 0.0) - previousMean;
            float residual = a - b;
            cost += sqrt(residual * residual + 1e-5);
        }
    return cost / 9.0;
}

void GeoAddEquation(inout float system[72], float row[8], float rhs, float weight)
{
    [loop] for (int r = 0; r < 8; ++r)
    {
        [loop] for (int c = 0; c < 8; ++c)
            system[r * 9 + c] += weight * row[r] * row[c];
        system[r * 9 + 8] += weight * row[r] * rhs;
    }
}

bool GeoSolveSystem(inout float system[72], out float h[8])
{
    [loop] for (int col = 0; col < 8; ++col)
    {
        int pivot = col;
        float pivotValue = abs(system[col * 9 + col]);
        [loop] for (int row = col + 1; row < 8; ++row)
        {
            float candidate = abs(system[row * 9 + col]);
            if (candidate > pivotValue) { pivot = row; pivotValue = candidate; }
        }
        if (!(pivotValue > 1e-7) || !(pivotValue < 1e20)) return false;
        if (pivot != col)
            [unroll] for (int k = 0; k < 9; ++k)
                if (k >= col)
                {
                    float t = system[col * 9 + k];
                    system[col * 9 + k] = system[pivot * 9 + k];
                    system[pivot * 9 + k] = t;
                }
        float invPivot = rcp(system[col * 9 + col]);
        [unroll] for (int k = 0; k < 9; ++k)
            if (k >= col) system[col * 9 + k] *= invPivot;
        [loop] for (int row = 0; row < 8; ++row)
            if (row != col)
            {
                float factor = system[row * 9 + col];
                [unroll] for (int k = 0; k < 9; ++k)
                    if (k >= col) system[row * 9 + k] -= factor * system[col * 9 + k];
            }
    }
    [unroll] for (int i = 0; i < 8; ++i) h[i] = system[i * 9 + 8];
    return h[0] == h[0] && h[1] == h[1] && h[2] == h[2] && h[3] == h[3] &&
           h[4] == h[4] && h[5] == h[5] && h[6] == h[6] && h[7] == h[7];
}

void GeoRows(float2 current, float2 previous, out float rowX[8], out float rowY[8])
{
    rowX[0] = current.x; rowX[1] = current.y; rowX[2] = 1.0;
    rowX[3] = 0.0; rowX[4] = 0.0; rowX[5] = 0.0;
    rowX[6] = -previous.x * current.x; rowX[7] = -previous.x * current.y;
    rowY[0] = 0.0; rowY[1] = 0.0; rowY[2] = 0.0;
    rowY[3] = current.x; rowY[4] = current.y; rowY[5] = 1.0;
    rowY[6] = -previous.y * current.x; rowY[7] = -previous.y * current.y;
}

float2 GeoPredict(float h[8], float2 current, out float denominator)
{
    denominator = h[6] * current.x + h[7] * current.y + 1.0;
    return float2(h[0] * current.x + h[1] * current.y + h[2],
                  h[3] * current.x + h[4] * current.y + h[5]) / denominator;
}

int GeoHypothesisPoint(int hypothesis, int quadrant)
{
    uint seed = (uint)hypothesis * 747796405u + (uint)quadrant * 2891336453u + 277803737u;
    int local = (int)((seed >> 16) & 3u);
    int x = (quadrant & 1) * 2 + (local & 1);
    int y = (quadrant >> 1) * 2 + ((local >> 1) & 1);
    return y * 4 + x;
}

// One workgroup fits the same 16 correspondences and 32 deterministic hypotheses.
// Point sampling and independent minimal solves run in parallel; lane zero reduces in
// hypothesis order (the original tie break) and performs the final weighted refit.
// No additional search, sample decimation or relaxed consensus thresholds are introduced.
groupshared float2 gGeoCurrent[16], gGeoPrevious[16];
groupshared float gGeoWeight[16], gGeoScore[32], gGeoH[256];
groupshared uint gGeoMask[32];
[numthreads(8, 8, 1)]
void FitGeometry(uint3 id : SV_DispatchThreadID)
{
    uint lane = id.y * 8u + id.x;
    const float inlierPx = 0.75;
    if (lane < 16u)
    {
        float2 uv = (float2(lane % 4u, lane / 4u) + 0.5) * 0.25;
        int2 flowSize = int2((aux + 1) / 2);
        float4 flow = FlowIn.Load(int3(min(int2(uv * float2(flowSize)), flowSize - 1), 0));
        float photo = GeoPhotoCost(uv, flow.xy);
        float2 previousUv = uv + flow.xy / float2(aux);
        gGeoCurrent[lane] = (uv - 0.5) * 2.0;
        gGeoPrevious[lane] = (previousUv - 0.5) * 2.0;
        bool valid = flow.z >= 0.015 && photo < 0.20 && all(previousUv > 0.0) && all(previousUv < 1.0) &&
                     all(abs(flow.xy) < 1e6);
        gGeoWeight[lane] = valid ? max(saturate(flow.z), 0.05) * saturate((0.20 - photo) / 0.16) : 0.0;
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane < 32u)
    {
        float system[72];
        [loop] for (int m = 0; m < 72; ++m) system[m] = 0.0;
        bool usable = true;
        [loop] for (int q = 0; q < 4; ++q)
        {
            int k = GeoHypothesisPoint((int)lane, q);
            if (gGeoWeight[k] <= 0.0) { usable = false; continue; }
            float rx[8], ry[8];
            GeoRows(gGeoCurrent[k], gGeoPrevious[k], rx, ry);
            GeoAddEquation(system, rx, gGeoPrevious[k].x, 1.0);
            GeoAddEquation(system, ry, gGeoPrevious[k].y, 1.0);
        }
        float h[8];
        float score = 0.0;
        uint mask = 0u, quadrants = 0u;
        int count = 0;
        if (usable && GeoSolveSystem(system, h))
        {
            [loop] for (int i = 0; i < 16; ++i)
            {
                if (gGeoWeight[i] <= 0.0) continue;
                float denominator;
                float2 predicted = GeoPredict(h, gGeoCurrent[i], denominator);
                float errorPx = length((predicted - gGeoPrevious[i]) * float2(aux) * 0.5);
                if (denominator > 0.1 && denominator < 10.0 && errorPx <= inlierPx)
                {
                    score += gGeoWeight[i];
                    mask |= 1u << i;
                    ++count;
                    quadrants |= 1u << ((i % 4 >= 2 ? 1 : 0) + (i / 4 >= 2 ? 2 : 0));
                }
            }
        }
        bool accepted = count >= 6 && quadrants == 15u;
        gGeoScore[lane] = accepted ? score : 0.0;
        gGeoMask[lane] = mask;
        if (accepted)
            [unroll] for (int j = 0; j < 8; ++j) gGeoH[lane * 8u + j] = h[j];
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane != 0u) return;
    float totalWeight = 0.0;
    int usableCount = 0;
    [loop] for (int i = 0; i < 16; ++i)
    {
        totalWeight += gGeoWeight[i];
        usableCount += gGeoWeight[i] > 0.0 ? 1 : 0;
    }
    float bestH[8], bestInlier[16];
    [unroll] for (int j = 0; j < 8; ++j) bestH[j] = 0.0;
    float bestScore = 0.0;
    uint bestMask = 0u;
    [loop] for (int hypothesis = 0; hypothesis < 32; ++hypothesis)
        if (gGeoScore[hypothesis] > bestScore)
        {
            bestScore = gGeoScore[hypothesis];
            bestMask = gGeoMask[hypothesis];
            [unroll] for (int j = 0; j < 8; ++j) bestH[j] = gGeoH[hypothesis * 8 + j];
        }
    [loop] for (int i = 0; i < 16; ++i) bestInlier[i] = (bestMask & (1u << i)) != 0u ? 1.0 : 0.0;
    float diagnostics[4] = { 0.0, 1e6, 0.0, (float)usableCount };
    if (bestScore > 0.0)
    {
        float refine[72];
        [loop] for (int m = 0; m < 72; ++m) refine[m] = 0.0;
        [loop] for (int i = 0; i < 16; ++i)
            if (bestInlier[i] > 0.5)
            {
                float rx[8], ry[8];
                GeoRows(gGeoCurrent[i], gGeoPrevious[i], rx, ry);
                GeoAddEquation(refine, rx, gGeoPrevious[i].x, gGeoWeight[i]);
                GeoAddEquation(refine, ry, gGeoPrevious[i].y, gGeoWeight[i]);
            }
        float fitted[8];
        if (GeoSolveSystem(refine, fitted))
        {
            float finalScore = 0.0, meanError = 0.0;
            int finalCount = 0;
            uint quadrants = 0;
            [loop] for (int i = 0; i < 16; ++i)
                if (gGeoWeight[i] > 0.0)
                {
                    float denominator;
                    float2 predicted = GeoPredict(fitted, gGeoCurrent[i], denominator);
                    float errorPx = length((predicted - gGeoPrevious[i]) * float2(aux) * 0.5);
                    if (denominator > 0.1 && denominator < 10.0 && errorPx <= inlierPx)
                    {
                        finalScore += gGeoWeight[i];
                        meanError += errorPx * gGeoWeight[i];
                        ++finalCount;
                        quadrants |= 1u << ((i % 4 >= 2 ? 1 : 0) + (i / 4 >= 2 ? 2 : 0));
                    }
                }
            bool validFit = finalCount >= 6 && quadrants == 15u && totalWeight > 0.0 && finalScore / totalWeight >= 0.30;
            [unroll] for (int q = 0; q < 5; ++q)
            {
                float2 probe = q == 0 ? float2(0, 0) : q == 1 ? float2(-0.9, -0.9) :
                               q == 2 ? float2(0.9, -0.9) : q == 3 ? float2(-0.9, 0.9) : float2(0.9, 0.9);
                float denominator = fitted[6] * probe.x + fitted[7] * probe.y + 1.0;
                validFit = validFit && denominator > 0.1 && denominator < 10.0;
            }
            if (validFit)
            {
                [unroll] for (int i = 0; i < 8; ++i) bestH[i] = fitted[i];
                diagnostics[0] = finalScore / totalWeight;
                diagnostics[1] = finalScore > 0.0 ? meanError / finalScore : 1e6;
                diagnostics[2] = 1.0;
                diagnostics[3] = (float)finalCount;
            }
        }
    }
    OutModel[uint2(0, 0)] = float4(bestH[0], bestH[1], bestH[2], bestH[3]);
    OutModel[uint2(1, 0)] = float4(bestH[4], bestH[5], bestH[6], bestH[7]);
    OutModel[uint2(2, 0)] = float4(diagnostics[0], diagnostics[1], diagnostics[2], diagnostics[3]);
}

[numthreads(8, 8, 1)]
void FuseGeometry(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(size);
    float4 flow = FlowIn.Load(int3(id.xy, 0));
    float4 h0 = GeoModel.Load(int3(0, 0, 0));
    flow.z = (flow.z >= 0.0 && flow.z <= 1.0) ? flow.z : 0.0;
    float4 h1 = GeoModel.Load(int3(1, 0, 0));
    float4 fit = GeoModel.Load(int3(2, 0, 0));
    float h[8] = { h0.x, h0.y, h0.z, h0.w, h1.x, h1.y, h1.z, h1.w };
    float flowResidual = GeoPhotoCost(uv, flow.xy);
    // Keep every finite local match available to fusion, including low-confidence vectors at
    // depth and motion boundaries. Their confidence still controls weighting and trust; rejecting
    // them here lets the camera model overwrite foreground motion where a photometric patch spans
    // two surfaces.
    bool flowValid = all(abs(flow.xy) < 1e6);
    float2 current = (uv - 0.5) * 2.0;
    float denominator;
    float2 previous = GeoPredict(h, current, denominator);
    float2 previousUv = previous * 0.5 + 0.5;
    float2 geometryFlow = (previousUv - uv) * float2(aux);
    bool modelValid = fit.z > 0.5 && fit.x >= 0.30 && denominator > 0.1 && denominator < 10.0 &&
                      all(previousUv > 0.0) && all(previousUv < 1.0);
    float geometryResidual = modelValid ? GeoPhotoCost(uv, geometryFlow) : 1e6;
    bool geometryValid = modelValid && geometryResidual < 0.20 && all(abs(geometryFlow) < 1e6);

    float2 selected = 0.0;
    float confidence = 0.0;
    float source = 0.0; // 0 invalid, 1 flow, 2 geometry, 3 agreement blend
    if (flowValid && geometryValid)
    {
        float disagreement = length(flow.xy - geometryFlow);
        float flowWeight = saturate(flow.z) * exp(-flowResidual * 6.0);
        float geometryWeight = saturate(fit.x) * exp(-geometryResidual * 6.0);
        // A small pixel-space difference is not evidence of shared motion: slow
        // foreground and a static HUD can differ from the camera by less than two
        // pixels. Preserve a structured local match when its photometric support
        // is clearly stronger, before considering an agreement blend.
        const bool distinctLocalMotion = disagreement > 0.5 ||
            (length(flow.xy) < 0.1 && length(geometryFlow) > 0.25);
        if (distinctLocalMotion && flow.z >= 0.12 && flowResidual + 0.005 < geometryResidual)
        {
            selected = flow.xy; confidence = flow.z; source = 1.0;
        }
        else if (disagreement <= 2.0)
        {
            selected = (flow.xy * flowWeight + geometryFlow * geometryWeight) / max(flowWeight + geometryWeight, 1e-6);
            confidence = flow.z;
            source = 3.0;
        }
        else if (flow.z >= 0.12 || flowResidual + 0.02 < geometryResidual)
        {
            selected = flow.xy; confidence = flow.z; source = 1.0;
        }
        else if (geometryResidual + 0.08 < flowResidual && fit.x > 0.50)
        {
            selected = geometryFlow; confidence = flow.z * fit.x; source = 2.0;
        }
        else
        {
            // Geometric flow fills only an ambiguous local result. When the two hypotheses
            // conflict and neither has clear photometric support, retain the measured vector
            // at its original confidence so the consumer can distrust it rather than replacing
            // independent object motion with camera motion.
            selected = flow.xy; confidence = flow.z; source = 1.0;
        }
    }
    else if (flowValid)
    {
        selected = flow.xy; confidence = flow.z; source = 1.0;
    }
    else if (geometryValid)
    {
        selected = geometryFlow; confidence = flow.z * fit.x; source = 2.0;
    }

    OutFlow[id.xy] = float4(selected, confidence, 1.0);
    OutInfo[id.xy] = float4(source, flowResidual, geometryResidual, fit.x);
}
)HLSL"
F5LOW_MOTION_ARROWS_HLSL
F5LOW_GEOMETRIC_DEBUG_HLSL
R"HLSL(
// The F5Low tab can select one of these views without changing the vectors consumed downstream:
// 0 final fused flow, 1 local structure confidence, 2 fitted camera vectors, 3 fusion source,
// 4 photometric support, 5 frame fit quality, 6 raw flow, 7 visible-motion arrows,
// 8 geometry rejection only, 9 geometric model grid only.
float3 GeoVectorColour(float2 f)
{
    float hue = atan2(f.y, f.x) / 6.2831853 + 0.5;
    float3 rgb = saturate(abs(frac(hue + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
    return rgb * saturate(length(f) / max(scale, 1.0));
}

[numthreads(8, 8, 1)]
void Visualise(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y) return;
    float4 flow = FlowIn.Load(int3(id.xy, 0));
    float4 info = GeoInfo.Load(int3(id.xy, 0));
    float4 fit = GeoModel.Load(int3(2, 0, 0));
    if (radius == 8)
    {
        OutFlow[id.xy] = float4(GeometryRejectionMask(info.x).xxx, 1.0);
        return;
    }
    if (radius == 9)
    {
        OutFlow[id.xy] = GeometryModelDebug((float2(id.xy) + 0.5) / float2(size), float2(aux),
            GeoModel.Load(int3(0, 0, 0)), GeoModel.Load(int3(1, 0, 0)), fit);
        return;
    }
    float4 output = 0.0;
    if (radius == 1)
        output = float4(saturate(flow.z).xxx, 1.0);
    else if (radius == 2)
    {
        if (!(fit.z > 0.5)) { OutFlow[id.xy] = float4(0.0, 0.0, 0.0, 1.0); return; }
        float2 uv = (float2(id.xy) + 0.5) / float2(size);
        float4 h0 = GeoModel.Load(int3(0, 0, 0));
        float4 h1 = GeoModel.Load(int3(1, 0, 0));
        float h[8] = { h0.x, h0.y, h0.z, h0.w, h1.x, h1.y, h1.z, h1.w };
        float denominator;
        float2 previous = GeoPredict(h, (uv - 0.5) * 2.0, denominator) * 0.5 + 0.5;
        if (!(denominator > 0.1 && denominator < 10.0))
        { OutFlow[id.xy] = float4(0.0, 0.0, 0.0, 1.0); return; }
        float2 cameraFlow = (previous - uv) * float2(aux);
        output = float4(GeoVectorColour(cameraFlow), 1.0);
    }
    else if (radius == 3)
        output = info.x < 0.5 ? float4(0.03,0.03,0.03,1) : info.x < 1.5 ? float4(0.1,0.35,1,1) :
                 info.x < 2.5 ? float4(1,0.2,0.08,1) : float4(0.15,1,0.2,1);
    else if (radius == 4)
        output = float4(saturate(info.y * 4.0), saturate(info.z * 4.0), saturate(info.w), 1.0);
    else if (radius == 5)
        output = float4(saturate(fit.x), 1.0 - saturate(fit.y / 3.0), fit.z, 1.0);
    else if (radius == 7)
        output = float4(0.0, 0.0, 0.0, 0.0);
    else
        output = float4(GeoVectorColour(flow.xy), 1.0);

    if (radius == 7)
    {
        // All geometry is in full-resolution pixels, the same units as the stored flow.
        // Gather adjacent roots so arrows can cross cell boundaries without being clipped.
        float2 pixel = (float2(id.xy) + 0.5) * float2(aux) / float2(size);
        float spacing = max(knee, 12.0);
        float aa = max(0.8, max(float(aux.x) / size.x, float(aux.y) / size.y) * 0.65);
        float2 nearest = round(pixel / spacing);
        float dotDistance = length(pixel - nearest * spacing);
        float gridAlpha = (1.0 - smoothstep(1.25, 1.25 + aa, dotDistance)) * lookDistance;
        float4 overlay = float4(0.58, 0.60, 0.64, gridAlpha);
        float4 best = 0.0;
        [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            float2 root = (nearest + float2(x, y)) * spacing;
            if (any(root < 0.0) || any(root >= float2(aux))) continue;
            int2 samplePixel = clamp(int2(root * float2(size) / float2(aux)), 0, int2(size) - 1);
            float4 sampled = FlowIn.Load(int3(samplePixel, 0));
            // Show the actual vector field, including finite low-confidence camera
            // predictions. Confidence has its own diagnostic view; it must not
            // silently erase arrows in otherwise valid regions.
            if (!all(abs(sampled.xy) < 1e6)) continue;
            float4 arrow = DrawMotionArrow(pixel, root, sampled.xy, spacing, lambda, lookRange, aa, zeroMargin, scale);
            if (arrow.a > best.a) best = arrow;
        }
        float alpha = best.a + overlay.a * (1.0 - best.a);
        float3 premultiplied = best.rgb * best.a + overlay.rgb * overlay.a * (1.0 - best.a);
        output = float4(alpha > 1e-6 ? premultiplied / alpha : 0.0, alpha * whiteNits);
        if (hasPrediction != 0)
        {
            float3 background = DebugColor.SampleLevel(Linear, (float2(id.xy) + 0.5) / float2(size), 0.0).rgb;
            output = float4(lerp(background, output.rgb, output.a), 1.0);
        }
    }

    OutFlow[id.xy] = output;
}
)HLSL";
} // namespace OpticalFlowHlsl
