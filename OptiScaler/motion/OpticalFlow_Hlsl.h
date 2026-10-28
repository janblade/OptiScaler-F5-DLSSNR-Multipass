#pragma once

#include "Luma_Hlsl.h"
#include "CameraModel_Hlsl.h"
#include "CameraFusion_Hlsl.h"

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
    float photoTolerance;   // Match, finest level: how much lower (in matching-cost units) a sub-pixel answer's cost must
                            // be to count as better; the rounding allowance of the acceptance test
    float evidenceNoise;    // Match: the luma noise floor the vector evidence (.w) is measured against
    uint2 padding;
};

SamplerState Linear : register(s0);
Texture2D<float4> Color : register(t0);
Texture2D<float>  CurLuma : register(t0);
Texture2D<float>  PrevLuma : register(t1);
Texture2D<float4> Prediction : register(t2);
Texture2D<float4> History : register(t3);
Texture2D<float>  GuideLuma : register(t1);
Texture2D<float> CameraPrevious : register(t2);
Texture2D<float4> FlowIn : register(t0);
Texture2D<float>  SceneDepth : register(t4);
Texture2D<float4> GlobalFlow : register(t5);
Texture2D<uint>   CutFlag : register(t6);
Texture2D<uint>   LastAge : register(t7);
RWTexture2D<float>  OutLuma : register(u0);
RWTexture2D<float4> OutFlow : register(u0);
RWTexture2D<uint>   OutAge : register(u1);

)HLSL" F5LOW_LUMA_HLSL
                                       R"HLSL(// Luma of the colour, averaged over 2x2 into the first level.
[numthreads(8, 8, 1)]
void Luma(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    precise float sum = 0.0;
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

    precise float sum = 0.0;
    [unroll] for (int j = 0; j < 2; ++j)
        [unroll] for (int i = 0; i < 2; ++i)
            sum += CurLuma.Load(int3(min(int2(id.xy) * 2 + int2(i, j), int2(aux) - 1), 0));

    OutLuma[id.xy] = sum * 0.25;
}

// Cooperative source-image reads only. No weights, candidates, residuals or
// confidence values are shared. Every lane reaches the barrier, including padding.
groupshared float gCurrentTile[256];
groupshared int2 gCurrentBase;
groupshared float gMatchDepth[256];
groupshared float gZeroPrevious[256];
int CurrentPitch() {
    // Inverse refinement reads current gradients at +/-4; conventional LK
    // keeps its original per-pixel previous-frame fractional coordinates.
    return inverseRefinement != 0u ? 16 : 14;
}
void PrepareCurrent(uint3 id) {
    int pitch=CurrentPitch();
    int2 base=int2(id.xy/8u)*8-(pitch/2-4);
    uint lane=(id.y&7u)*8u+(id.x&7u);
    if(lane==0u)gCurrentBase=base;
    for(uint n=lane;n<uint(pitch*pitch);n+=64u){
        // The cooperative tile has exactly two integer pitches. Keep every
        // original address, but expose constant divisors instead of a dynamic
        // integer division in each lane's source-image loads.
        uint2 tileOffset;
        tileOffset=inverseRefinement!=0u ? uint2(n&15u,n>>4u) : uint2(n%14u,n/14u);
        int2 at=clamp(base+int2(tileOffset),0,int2(size)-1);
        gCurrentTile[n]=CurLuma.Load(int3(at,0));
        if(depthMatching != 0u) {
            // Preserve Distance's floating-point UV-to-depth mapping verbatim.
            float2 uv=(float2(at)+0.5)/float2(size);
            int2 depthAt=min(int2(uv*float2(depthSize)),int2(depthSize)-1);
            gMatchDepth[n]=SceneDepth.Load(int3(depthAt,0));
        }
        gZeroPrevious[n]=PrevLuma.Load(int3(at,0));
    }
    GroupMemoryBarrierWithGroupSync();
}
float CurrentAt(int3 at){int2 q=at.xy-gCurrentBase;return gCurrentTile[q.y*CurrentPitch()+q.x];}
float PreviousZeroAt(int3 at){
    int2 q=at.xy-gCurrentBase;
    return gZeroPrevious[q.y*CurrentPitch()+q.x];
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

float MatchDistance(int2 p) {
    int2 q=p-gCurrentBase;
    float d=gMatchDepth[q.y*CurrentPitch()+q.x];
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
float Cost(int2 p, int2 d, float w[16], float cur[16], float weightTotal)
{
    float diff[16];
    float mean = 0.0; const float total = weightTotal;
    int2 hi = int2(size) - 1;

    [unroll] for (int j = 0; j < 4; ++j)
        [unroll] for (int i = 0; i < 4; ++i)
        {
            int2 q = int2(2 * i - 3, 2 * j - 3);
            float c = cur[j * 4 + i];
            float r = PrevLuma.Load(int3(clamp(p + q + d, 0, hi), 0));
            diff[j * 4 + i] = c - r;
            mean += w[j * 4 + i] * (c - r);
        }

    mean /= total;
    float s = 0.0;

    [unroll] for (int k = 0; k < 16; ++k)
        s += w[k] * abs(diff[k] - mean);

    return s * 16.0 / total;
}

float ZeroCost(int2 p, float w[16], float cur[16], float weightTotal)
{
    float diff[16];
    float mean = 0.0; const float total = weightTotal;
    int2 hi = int2(size) - 1;

    [unroll] for (int j = 0; j < 4; ++j)
        [unroll] for (int i = 0; i < 4; ++i)
        {
            int2 q = int2(2 * i - 3, 2 * j - 3);
            float c = cur[j * 4 + i];
            float r = PreviousZeroAt(int3(clamp(p + q, 0, hi), 0));
            diff[j * 4 + i] = c - r;
            mean += w[j * 4 + i] * (c - r);
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
            largest = max(largest, abs(cur[j * 4 + i] - PreviousZeroAt(int3(clamp(p + int2(2 * i - 3, 2 * j - 3), 0, hi), 0))));

    static const int2 kEven[5] = { int2(0, 0), int2(-2, 0), int2(2, 0), int2(0, -2), int2(0, 2) };

    [unroll] for (int k = 0; k < 5; ++k)
    {
        int2 q = clamp(p + kEven[k], 0, hi);
        largest = max(largest, abs(CurrentAt(int3(q, 0)) - PreviousZeroAt(int3(q, 0))));
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

// Exactly the matching objective, evaluated at a fractional displacement.
// Integer candidates retain Cost's typed loads; a refinement uses the same
// sixteen offsets, mean removal and depth/luma weights with linear sampling.
float FractionalCost(int2 p, float2 d, float w[16], float cur[16], float weightTotal)
{
    float diff[16]; float mean = 0.0; const float total = weightTotal;
    [unroll] for (int j = 0; j < 4; ++j)
        [unroll] for (int i = 0; i < 4; ++i)
        {
            int k = j * 4 + i;
            // Match conventional/inverse LK's original per-pixel coordinates:
            // clamp the current-window texel before applying its own offset.
            int2 q=clamp(p+int2(2*i-3,2*j-3),0,int2(size)-1);
            float r = Sample(float2(q) + d);
            diff[k] = cur[k] - r;
            mean += w[k] * diff[k];
        }
    mean /= total; float result = 0.0;
    [unroll] for (int k = 0; k < 16; ++k) result += w[k] * abs(diff[k] - mean);
    return result * 16.0 / total;
}

// Cost() keeps the original integer matching window. At image borders LK
// clamps that window before displacement, so its acceptance test needs the
// same fractional-sampler objective at the unrefined displacement as well.
float RefinementIntegerCost(int2 p,int2 d,float w[16],float cur[16],float weightTotal,float matchingCost) {
    if(all(p>=3) && all(p<int2(size)-3))return matchingCost;
    return FractionalCost(p,float2(d),w,cur,weightTotal);
}

// Vector evidence is distinct from both .z texture structure and reconstruction
// history trust. No history mask is derived from this score.
float VectorEvidence(int2 p, float2 motion, float structure, float cost,
                     float gap, float w[16], float cur[16])
{
    float mean = 0.0, total = 0.0;
    [unroll] for (int k = 0; k < 16; ++k) { mean += w[k] * cur[k]; total += w[k]; }
    mean /= total; float contrast = 0.0;
    [unroll] for (int k = 0; k < 16; ++k) contrast += w[k] * abs(cur[k] - mean);
    contrast /= total;
    float normalization = 16.0 * max(contrast + evidenceNoise, 1e-6);
    float relativeError = cost / normalization;
    float photo = rcp(1.0 + relativeError * relativeError);
    float distinct = saturate(gap / max(16.0 * evidenceNoise, 1e-6));
    float temporal = 1.0;
    if (hasHistory != 0)
    {
        float2 prior = History.Load(int3(p, 0)).xy;
        float diff = length(motion - prior);
        float allowance = 1.0 + 0.5 * length(motion);
        temporal = rcp(1.0 + diff / allowance);
    }
    return saturate(structure * photo * lerp(0.25, 1.0, distinct) * temporal);
}

// Block matching at one level: look around the coarser level's answer (doubled, it is in that level's pixels) for the offset
// into the previous frame with the smallest difference, then a few gradient steps for the part of a pixel.
[numthreads(8, 8, 1)]
void Match(uint3 id : SV_DispatchThreadID)
{
    PrepareCurrent(id);
    if (id.x >= size.x || id.y >= size.y)
        return;

    // A hard cut found on this very frame: the previous frame shows another scene, so there is nothing to match. No motion and
    // no confidence at every level (the whole group leaves here or none does: the flag is one value for the dispatch).
    if (sceneCutEnabled != 0 && CutFlag.Load(int3(0, 0, 0)) != 0)
    {
        OutFlow[id.xy] = float4(0.0, 0.0, 0.0, 0.0); // .w, the vector's support, is none too (it was an unused 1)

        if (stillFrames != 0)
            OutAge[id.xy] = 0; // the picture is another one: nothing has been still yet

        return;
    }

    int2 p = int2(id.xy);

    // How much each sample of the window counts: all alike, or with depth-aware matching only those on this pixel's surface,
    // so the edge of something nearer does not decide the match of what lies beside it.
    float w[16];
    float zc = depthMatching != 0 ? MatchDistance(p) : 0.0;

    [unroll] for (int wj = 0; wj < 4; ++wj)
        [unroll] for (int wi = 0; wi < 4; ++wi)
        {
            int2 q = clamp(p + int2(2 * wi - 3, 2 * wj - 3), 0, int2(size) - 1);
            w[wj * 4 + wi] = depthMatching != 0 ? SameSurface(MatchDistance(q), zc) : 1.0;
        }

    // The current frame's window, loaded once for every candidate and search offset below.
    float cur[16];

    [unroll] for (int cj = 0; cj < 4; ++cj)
        [unroll] for (int ci = 0; ci < 4; ++ci)
            cur[cj * 4 + ci] = CurrentAt(int3(clamp(p + int2(2 * ci - 3, 2 * cj - 3), 0, int2(size) - 1), 0));

    // On the finest level the samples also count by how near their brightness is to this pixel's: without depth the depth
    // weights' counterpart, so the edge of a thing that looks different does not decide the match beside it, and with depth
    // on top of them, which keeps a misaligned depth from doing the same. The weights are then scaled back to the sum they
    // had: the cost and the sub-pixel step do not change with a common scale, but the confidence below is a sum over the
    // weights, and so its knee keeps the meaning it has with the depth weights alone.
    if (lookWeights != 0)
    {
        const float lc = CurrentAt(int3(p, 0));
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
    // The weights are invariant across this pixel's candidates and LK steps.
    // Retain the original 16-add order, once; no other pixel supplies this sum.
    float weightTotal=0.0;
    [unroll] for(int weightIndex=0;weightIndex<16;++weightIndex)weightTotal+=w[weightIndex];
    float2 inheritedPrediction=0.0, historyPrediction=0.0;
    int2 centre = 0;
    const float zeroCost = ZeroCost(p, w, cur, weightTotal);
    float start = zeroCost;

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
            float c = all(d == centre) ? start : all(d == 0) ? zeroCost : Cost(p, d, w, cur, weightTotal);

            if (pictureStill ? c + stillMargin < start : c <= start)
            {
                start = c;
                centre = d;
                inheritedPrediction=g.xy*scale;
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

        // Cache offsets only within this pixel. A previously evaluated candidate
        // cannot beat a start cost that only decreases. Keep search/tie order.
        int2 tried[9];
        [unroll]for(int initialize=0;initialize<9;++initialize)tried[initialize]=0;
        [loop] for (int k = 0; k < (int) coarseCells; ++k)
        {
            int2 cell = cp + kCells[k] * step;
            float2 predictor=Prediction.Load(int3(clamp(cell, 0, coarseHi), 0)).xy*2.0;
            int2 d=int2(round(predictor));
            bool duplicate=all(d==centre) || all(d==0);
            [unroll]for(int previous=0;previous<9;++previous)
                duplicate=duplicate || (previous<k && all(d==tried[previous]));
            tried[k]=d;
            if (duplicate)
                continue;
            float c = Cost(p, d, w, cur, weightTotal);

            if (c < start)
            {
                start = c;
                centre = d;
                inheritedPrediction=predictor;
            }
        }
    }

    if (hasHistory != 0)
    {
        historyPrediction=History.Load(int3(p,0)).xy;
        int2 d=int2(round(historyPrediction));
        float c = all(d == centre) ? start : all(d == 0) ? zeroCost : Cost(p, d, w, cur, weightTotal);

        if (c < start)
        {
            start = c;
            centre = d;
            inheritedPrediction=historyPrediction;
        }
    }

    float best = 1e30;
    float second = 1e30;
    float bestCost = 0.0;
    int2 bestD = centre;

    if(radius==1) {
    [unroll] for (int dy = -1; dy <= 1; ++dy)
        [unroll] for (int dx = -1; dx <= 1; ++dx)
        {
            int2 d = centre + int2(dx, dy);
            float cost = (dx == 0 && dy == 0) ? start : all(d == 0) ? zeroCost : Cost(p, d, w, cur, weightTotal);
            float c = cost + lambda * length(float2(dx, dy));

            if (c < best)
            {
                second = best;
                best = c;
                bestCost = cost;
                bestD = d;
            }
            else second = min(second, c);
        }

    } else {
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx)
        {
            int2 d = centre + int2(dx, dy);
            float cost = (dx == 0 && dy == 0) ? start : all(d == 0) ? zeroCost : Cost(p, d, w, cur, weightTotal);
            float c = cost + lambda * length(float2(dx, dy));

            if (c < best)
            {
                second = best;
                best = c;
                bestCost = cost;
                bestD = d;
            }
            else second = min(second, c);
        }

    }

    // Zero wins a near tie on the finest level: in flat or grainy ground the best match is a small offset picked on noise, and
    // the picture did not move there. Only an offset within zeroReach is overruled (a real pan is far from it).
    if (zeroMargin > 0.0 && any(bestD != 0) && (zeroReach == 0 || max(abs(bestD.x), abs(bestD.y)) <= (int) zeroReach))
        if (zeroCost <= bestCost + zeroMargin)
            bestD = int2(0, 0);

    // The part of a pixel: a few Lucas-Kanade steps. With the previous frame sampled at the matched offset, the remaining
    // difference is explained by the picture's gradient there; solve that for the shift. A match that is already exact has no
    // difference left and is not moved, which a fit through the costs either side cannot promise. The difference's mean over
    // the window is taken out first, so a brightness change does not read as a shift (the gradients are not centred).
    bool underconstrained = false;
    float2 sub = 0.0;
    bool stepped = false;   // a sub-pixel step was solved here
    bool checkedSub = false; // ... by a weak solve or taken from a candidate on the finest level (tested below)
    float confidence = 0.0;
    float weakSupport=0.0;

    if (inverseRefinement != 0)
    {
        // The same steps with the gradients taken from the current frame at the window's own pixels instead of from the
        // previous frame at the shifted position: they do not move with the shift, so they and everything built from them
        // are found once, and each step then needs a single filtered read per sample, not five. It stops once a step is
        // under a hundredth of a pixel.
        float a = 0.0, b = 0.0, c = 0.0, sx = 0.0, sy = 0.0; const float sw=weightTotal;
        float gxs[16], gys[16];
        int2 hi = int2(size) - 1;

        [unroll] for (int j = 0; j < 4; ++j)
            [unroll] for (int i = 0; i < 4; ++i)
            {
                int2 q = clamp(p + int2(2 * i - 3, 2 * j - 3), 0, hi);
                float gx = 0.5 * (CurrentAt(int3(clamp(q + int2(1, 0), 0, hi), 0)) -
                                  CurrentAt(int3(clamp(q - int2(1, 0), 0, hi), 0)));
                float gy = 0.5 * (CurrentAt(int3(clamp(q + int2(0, 1), 0, hi), 0)) -
                                  CurrentAt(int3(clamp(q - int2(0, 1), 0, hi), 0)));
                float wk = w[j * 4 + i];

                gxs[j * 4 + i] = gx;
                gys[j * 4 + i] = gy;
                a += wk * gx * gx;
                b += wk * gx * gy;
                c += wk * gy * gy;
                sx += wk * gx;
                sy += wk * gy;

            }

        const float det = a * c - b * b;
        underconstrained=det<=1e-9;
        const float weak = max(0.5 * ((a + c) - sqrt((a - c) * (a - c) + 4.0 * b * b)), 0.0);
        confidence = weak / (weak + max(knee, 1e-7));

        // Contrast alone must not invalidate a well-conditioned 2D solve.
        // A weak solve additionally needs explained temporal residual and a
        // verified reduction of the original per-pixel photometric objective.
        const float integerWeakCost=RefinementIntegerCost(p,bestD,w,cur,weightTotal,all(bestD==0)?zeroCost:bestCost);
        const bool weakPotential=det>0.01*(a+c)*(a+c) && weak>sw*1e-8 && integerWeakCost>photoTolerance;
        // A window with no structure across (flat, or one straight edge) gives no step: its reads are skipped. Written as the
        // negation so a NaN determinant runs the steps, as the break on `det <= 1e-9` it replaces did.
        [loop] for (int iteration = 0; iteration < 3 && (!(det <= 1e-9) || weakPotential); ++iteration)
        {
            float e = 0.0, f = 0.0, sr = 0.0, rr = 0.0;

            [unroll] for (int j = 0; j < 4; ++j)
                [unroll] for (int i = 0; i < 4; ++i)
                {
                    int2 q = clamp(p + int2(2 * i - 3, 2 * j - 3), 0, hi);
                    float r = cur[j * 4 + i] - Sample(float2(q + bestD) + sub);
                    float wk = w[j * 4 + i];

                    e += wk * gxs[j * 4 + i] * r;
                    f += wk * gys[j * 4 + i] * r;
                    sr += wk * r; rr += wk * r * r;
                }

            const float meanResidual = sr / sw;
)HLSL"
                                       R"HLSL(            e -= sx * meanResidual;
            f -= sy * meanResidual;

            const float2 step = float2(c * e - b * f, a * f - b * e) / det;
            float2 proposal=clamp(sub+step,-1.5,1.5);
            if(det<=1e-9) {
                float residual=max(rr-sr*sr/sw,0.0);
                if(dot(float2(e,f),step)<=0.4*max(residual,sw*1e-10))break;
                float beforeCost=FractionalCost(p,float2(bestD)+sub,w,cur, weightTotal);
                float afterCost=FractionalCost(p,float2(bestD)+proposal,w,cur, weightTotal);
                if(!(afterCost+photoTolerance<0.98*beforeCost))break;
                float explained=saturate(dot(float2(e,f),step)/max(residual,sw*1e-10));
                weakSupport=max(weakSupport,explained*saturate((beforeCost-afterCost)/max(0.25*beforeCost,1e-12)));
                underconstrained=false;
                checkedSub = true;
            }
            sub = proposal;
            stepped = true;

            if (max(abs(step.x), abs(step.y)) < 0.01)
                break;
        }
    }
    else
    [loop] for (int iteration = 0; iteration < 3; ++iteration)
    {
        float2 beforeSub = sub;
        float a = 0.0, b = 0.0, c = 0.0, e = 0.0, f = 0.0;
        float sx = 0.0, sy = 0.0, sr = 0.0, rr = 0.0; const float sw=weightTotal;

        // Reuse only this pixel's own identical displaced LK source samples.
        // Adjacent taps in its 4x4 window share gradient endpoints. Coordinate
        // equality is checked after floating-point address construction, so
        // clamped image borders and rounding retain the original sample.
        float upper[4]; float upperY = 1e30;
        [unroll] for(int clear=0;clear<4;++clear)upper[clear]=0.0;
        [unroll] for (int j = -3; j <= 3; j += 2) {
            float leftValue=0.0, leftX=1e30;
            float nextUpperY=1e30;
            [unroll] for (int i = -3; i <= 3; i += 2)
            {
                int ix=(i+3)/2;
                int2 q = clamp(p + int2(i, j), 0, int2(size) - 1);
                float2 at = float2(q + bestD) + sub;
                float leftAt=at.x-1.0;
                float rightAt=at.x+1.0;
                float upAt=at.y-1.0;
                float downAt=at.y+1.0;
                float centre = Sample(at);
                float right=Sample(float2(rightAt,at.y));
                float left=(i!=-3 && leftX==leftAt)?leftValue:Sample(float2(leftAt,at.y));
                float down=Sample(float2(at.x,downAt));
                float up=(j!=-3 && upperY==upAt)?upper[ix]:Sample(float2(at.x,upAt));
                leftValue=right;leftX=rightAt;upper[ix]=down;nextUpperY=downAt;
                float gx=0.5*(right-left),gy=0.5*(down-up);
                float r = cur[((j + 3) / 2) * 4 + (i + 3) / 2] - centre;
                float wk = w[((j + 3) / 2) * 4 + (i + 3) / 2];

                a += wk * gx * gx;
                b += wk * gx * gy;
                c += wk * gy * gy;
                e += wk * gx * r;
                f += wk * gy * r;
                sx += wk * gx;
                sy += wk * gy;
                sr += wk * r; rr += wk * r * r;

            }
            upperY=nextUpperY;
        }

        // Only the difference is taken about its mean: centring the gradients too fits one more unknown to sixteen samples,
        // which costs accuracy in grain.
        const float meanResidual = sr / sw;
        e -= sx * meanResidual;
        f -= sy * meanResidual;

        float det = a * c - b * b;
        if(iteration==0)underconstrained=det<=1e-9;

        float weakEnergy=max(0.5*((a+c)-sqrt((a-c)*(a-c)+4.0*b*b)),0.0);
        float integerWeakCost=RefinementIntegerCost(p,bestD,w,cur,weightTotal,all(bestD==0)?zeroCost:bestCost);
        bool weakSolve=det<=1e-9 && det>0.01*(a+c)*(a+c) && weakEnergy>sw*1e-8 && integerWeakCost>photoTolerance;
        if (det > 1e-9 || weakSolve) {
            float2 step = float2(c * e - b * f, a * f - b * e) / det;
            float2 proposal=clamp(sub+step,-1.5,1.5);
            if(weakSolve) {
                float residual=max(rr-sr*sr/sw,0.0);
                weakSolve=dot(float2(e,f),step)>0.4*max(residual,sw*1e-10);
                if(weakSolve) {
                    float beforeCost=FractionalCost(p,float2(bestD)+sub,w,cur, weightTotal);
                    float afterCost=FractionalCost(p,float2(bestD)+proposal,w,cur, weightTotal);
                    weakSolve=afterCost+photoTolerance<0.98*beforeCost;
                    if(weakSolve) {
                        float explained=saturate(dot(float2(e,f),step)/max(residual,sw*1e-10));
                        weakSupport=max(weakSupport,explained*saturate((beforeCost-afterCost)/max(0.25*beforeCost,1e-12)));
                    }
                }
                if(weakSolve)underconstrained=false;
            }
            if (det > 1e-9 || weakSolve)
            {
                sub = proposal;
                stepped = true;
                checkedSub = checkedSub || weakSolve;
            }
        }

        // The weaker direction of the picture's structure in the window: a match along an edge, or in a flat area, is not sure
        // across it.
        float weak = max(0.5 * ((a + c) - sqrt((a - c) * (a - c) + 4.0 * b * b)), 0.0);
        confidence = weak / (weak + max(knee, 1e-7));
        if (det <= 1e-9 && !weakSolve) break; // Neither the original nor a validated weak solve can update it.
        // An exact floating-point fixed point repeats the identical displaced
        // samples, tensor, step and confidence: the remaining iterations are skipped.
        if (all(sub == beforeSub))
            break;
    }

    // A window with no structure across it (a flat one, or one straight edge) gives the steps nothing to go on, so they
    // leave the whole-pixel offset; there the part of a pixel the winning candidate had (when the search kept it) is kept,
    // at every level, which carries a camera's or a neighbour's fraction along a line or edge. On the finest level only
    // where the window changed since the last frame at the same place: one that did not (a still HUD panel early in a
    // pan, before the still age counts it as still) is explained by no motion, and with a pan's fraction the smoothing's
    // camera prior would pull the panel along with the camera. A flat fill gets its part of a pixel from that camera
    // prior instead (Smooth, CameraFuse). The clamp is the rounding's own range, and keeps a non-finite candidate
    // (history, the camera) out of the output.
    const bool windowChanged = scale != 0.5 || stillFrames == 0 || largest >= stillEpsilon;
    if (!stepped && all(bestD == centre) && windowChanged)
        sub = clamp(inheritedPrediction - float2(centre), -0.5, 0.5);

    // On the finest level, where no step could be solved, the candidate's or the last frame's fraction inside the same
    // whole-pixel offset is also taken where it lowers the matching cost.
    if (underconstrained && scale == 0.5)
    {
        float2 prior=inheritedPrediction;
        if(hasHistory!=0 && all(int2(round(historyPrediction))==bestD) && any(historyPrediction!=float2(bestD)))prior=historyPrediction;
        if(all(int2(round(prior))==bestD) && any(prior!=float2(bestD))) {
            float integerCost=RefinementIntegerCost(p,bestD,w,cur,weightTotal,all(bestD==0)?zeroCost:bestCost);
            float priorCost=FractionalCost(p,prior,w,cur, weightTotal);
            if (priorCost + photoTolerance < integerCost)
            {
                sub = prior - float2(bestD);
                checkedSub = true;
            }
        }
    }
    float2 motion = float2(bestD) + sub;
    float reliability = weakSupport;
    const bool inspect = scale == 0.5;
    // The weak solve's and the taken fraction must match no worse than the whole pixel. Not a solved step's or the
    // carried one: the filtered reads of a thin, sharp line at a part of a pixel always match worse than the whole pixel
    // (the filter blurs it), so that test would round a slow pan's true motion away there.
    const bool accept = scale == 0.5 && any(sub != 0.0) && checkedSub;
    if (inspect || accept)
    {
        // zeroMargin may have replaced the winning integer candidate. Its old
        // bestCost must not be used as the cost of this different displacement.
        float integerCost = RefinementIntegerCost(p,bestD,w,cur,weightTotal,all(bestD==0)?zeroCost:bestCost);
        float finalCost = any(sub != 0.0) ? FractionalCost(p, motion, w, cur, weightTotal) : integerCost;
        if (accept && !(finalCost <= integerCost + photoTolerance))
        { motion = float2(bestD); finalCost = integerCost; }
        if (inspect)
            reliability = max(weakSupport,VectorEvidence(p, motion, confidence, finalCost,
                second < 1e20 ? max(second - best, 0.0) : 0.0, w, cur));
    }
    OutFlow[id.xy] = float4(motion, confidence, reliability);

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

    OutFlow[id.xy] = float4(Median9(xs) * scale, Median9(ys) * scale, FlowIn.Load(int3(id.xy, 0)).z, FlowIn.Load(int3(id.xy, 0)).w);
}

// Edge-aware smoothing: the average of the neighbours' motion, each counted by how sure its match was, how close its motion
// is to this pixel's and how close its brightness is, so noise in a flat area is averaged out and the edge of something that
// moves differently is not. A 16x16 group cooperates over a 24x24 tile (2.25 input loads per output instead of 4), which
// the group's threads share, with the picture's edge repeated outward.
groupshared float4 gTile[576]; // original immutable flow record; no decision is shared or recomputed
groupshared float gSurface[576];
groupshared float gGuideLuma[576];
)HLSL" F5LOW_CAMERA_FUSION_HLSL R"HLSL(
float4 SmoothRegion(int2 at,float4 centre) {
    static const int2 offsets[8]={int2(-4,-4),int2(0,-4),int2(4,-4),int2(-4,0),int2(4,0),int2(-4,4),int2(0,4),int2(4,4)};
    float2 sum=0.0,directionSum=0.0;float total=0.0,energy=0.0,movingWeight=0.0;uint count=0u,movingCount=0u;
    float hereDepth=depthMatching!=0u?gSurface[at.y*24+at.x]:1.0;
    float lumaScale=max(0.08,0.3*(gGuideLuma[at.y*24+at.x]+0.05));
    [unroll]for(int k=0;k<8;++k) {
        int2 q=at+offsets[k];float4 f=gTile[q.y*24+q.x];
        float w=(min(f.z,0.25)+0.02)*saturate(1.0-abs(gGuideLuma[q.y*24+q.x]-gGuideLuma[at.y*24+at.x])/lumaScale);
        if(depthMatching!=0u) {
            float z=gSurface[q.y*24+q.x];
            if(abs(z-hereDepth)>0.05*min(z,hereDepth))w=0.0;
        }
        if(w>0.0)count++;
        float magnitude=dot(f.xy,f.xy);
        sum+=w*f.xy;total+=w;energy+=w*magnitude;
        if(w>0.0 && magnitude>0.0025) {
            directionSum+=w*f.xy*rsqrt(magnitude);movingWeight+=w;movingCount++;
        }
    }
    if(count<4u || total<=0.0)return float4(centre.xy,1.0,0.0);
    float coherence=saturate(length(directionSum)/max(movingWeight,1e-6))*saturate(float(movingCount)/4.0);
    // Coherent nonzero observations fill holes; zero observations must not
    // dilute their speed. Conflicting directions still cancel conservatively.
    float2 mean=sum/((coherence>0.7 && movingCount>=4u)?movingWeight:total);
    float magnitude2=dot(mean,mean);
    float spread=max(energy/total-dot(sum/total,sum/total),0.0);
    float conflict=smoothstep(0.0625,1.0,spread/(0.0625+magnitude2));
    float centreMagnitude=dot(centre.xy,centre.xy);
    float alignment=(centreMagnitude>0.0025 && magnitude2>0.0025)?dot(centre.xy,mean)*rsqrt(centreMagnitude*magnitude2):-1.0;
    conflict*=1.0-smoothstep(0.5,0.9,alignment);
    // A coherent moving neighbourhood can also disagree with an attenuated
    // centre in speed, even when all nonzero vectors point the same way.
    // This is only a proposal: CameraFuse retains the original per-pixel
    // photometric/depth validation before accepting any regional influence.
    float relativeChange=length(centre.xy-mean)/max(length(mean),0.10);
    float speedConflict=smoothstep(0.25,0.75,relativeChange)*smoothstep(0.75,0.95,coherence);
    conflict=max(conflict,speedConflict);
    // Cancellation is not evidence for an arbitrary nonzero direction.
    // Zero is only a proposal and still faces the same per-pixel photo test.
    if(coherence<0.15 && magnitude2<0.25)mean=0.0;
    return float4(mean,coherence,conflict);
}


[numthreads(16, 16, 1)]
void Smooth(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID)
{
    // Off: only the centre counts, so the tile is not filled (the radius is the same for the whole group, so every thread
    // leaves here or none does).
    if (radius == 0)
    {
        if (id.x < size.x && id.y < size.y)
        {
            // Rounded as the full pass rounds it (precise keeps the compiler from folding the weight away).
            float4 sample = FlowIn.Load(int3(id.xy, 0));
            float3 only = sample.xyz;
            precise float weight = only.z + 0.05;
            precise float2 weighted = only.xy * weight;
            OutFlow[id.xy] = float4(weighted / weight, only.z, max(only.z, sample.w));
        }

        return;
    }

    int2 hi = int2(size) - 1;
    int2 origin = int2(group.xy) * 16 - 4;
    uint t = local.y * 16 + local.x;

    // All 256 lanes cooperatively load the 576 entries and reach the barrier, including edge lanes.
    [loop] for (uint e = t; e < 576u; e += 256u)
    {
        int2 tile = int2(e % 24, e / 24);
        int2 q = clamp(origin + tile, 0, hi);
        float4 loaded=FlowIn.Load(int3(q,0));
        gTile[e]=loaded;
        gGuideLuma[e]=GuideLuma.Load(int3(q,0));
        if (depthMatching != 0) gSurface[e] = Distance((float2(q)+0.5)/float2(size));
    }

    GroupMemoryBarrierWithGroupSync();

    if (id.x >= size.x || id.y >= size.y)
        return;

    int2 at = int2(local.xy) + 4;
    float4 here = gTile[at.y * 24 + at.x];
    float3 centre = here.xyz;
    float lumaCentre = gGuideLuma[at.y*24+at.x];
    float surfaceCentre = depthMatching != 0 ? gSurface[at.y*24+at.x] : 1.0;

    precise float2 sum = centre.xy * (centre.z + 0.05);
    precise float total = centre.z + 0.05;
    // Each centre retains its own direct support read. Neighbour support
    // remains the same already-defined Smooth input at the same clamped index;
    // staging it changes neither the stencil nor any per-pixel weight. This
    // optimization requires independent byte-exact stage/route validation.
    float centreSupport = FlowIn.Load(int3(id.xy, 0)).w;
    float centreEvidence = centreSupport;
    sum *= max(centreEvidence, 0.05); total *= max(centreEvidence, 0.05);
    // Weak texture cannot justify a narrow motion gate around a possibly
    // arbitrary centre. Keep strong matches unchanged; pool modestly different
    // answers in ambiguous regions without temporal lag or extra passes.
    float ambiguous = 1.0 - smoothstep(0.01, 0.10, centre.z);
    float motionRange = 1.0 + 0.02 * dot(centre.xy, centre.xy) + 4.0 * ambiguous;
    float motionGuard=1.0-0.95*ambiguous;
    float lumaRange = max(0.15 * (lumaCentre + 0.05), 0.02 * ambiguous);

    [unroll] for (int j = -4; j <= 4; ++j)
        [unroll] for (int i = -4; i <= 4; ++i)
        {
            // A conditional body preserves the native unrolled arithmetic order
            // in FX too; FX's continue lowering otherwise forces a different loop.
            if ((i != 0 || j != 0) && abs(i) <= radius && abs(j) <= radius)
            {

            int2 q = at + int2(i, j);
            float4 other = gTile[q.y * 24 + q.x];
            precise float2 df = other.xy - centre.xy;
            precise float dl = (gGuideLuma[q.y*24+q.x] - lumaCentre) / lumaRange;
            float evidence = gTile[q.y * 24 + q.x].w;
            float surfacePenalty=0.0;
            // Strong centres keep the original filter. Only the expanded
            // ambiguous support needs this additional surface guard; identical
            // depth values need no ratio/exponential calculation.
            [branch] if (depthMatching != 0 && ambiguous>0.0) {
                float z=gSurface[q.y*24+q.x];
                // A branch, rather than a select, keeps the reciprocal and
                // penalty out of wave execution on constant-depth surfaces.
                [branch] if(z!=surfaceCentre) {
                    float rel=abs(z-surfaceCentre)/max(min(z,surfaceCentre),1e-6);
                    surfacePenalty=min(rel*rel/0.01,3.9120230054);
                }
            }
            precise float w = (other.z + 0.05) * max(evidence, 0.05) * exp(-(dot(df, df)*motionGuard) / motionRange - dl * dl - surfacePenalty);
            sum += other.xy * w;
            total += w;
            }
        }

    precise float2 filtered = sum / total;
    float4 regional=SmoothRegion(at,here);
    float support=max(centre.z,centreSupport);
    filtered=CameraFuse(int2(id.xy),filtered,centre.z,centreSupport,true,regional.xy,regional.w,regional.z,support);
    OutFlow[id.xy]=float4(filtered,centre.z,support);
}
)HLSL" F5LOW_CAMERA_MODEL_HLSL;
} // namespace OpticalFlowHlsl
