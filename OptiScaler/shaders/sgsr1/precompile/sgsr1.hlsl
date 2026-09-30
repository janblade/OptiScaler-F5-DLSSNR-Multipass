//============================================================================================================
//
//                  Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
//                              SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================
//
// Ported for OptiScaler from Qualcomm's official Snapdragon Game Super Resolution v1 shader
// (SnapdragonGameStudios/snapdragon-gsr, sgsr/v1/include/hlsl/sgsr1_mobile.h +
// sgsr1_shader_mobile.hlsl) into a DX12/Vulkan compute kernel. The algorithm itself
// (fastLanczos2/weightY/SgsrYuvH) is unchanged -- only the shader stage (pixel -> compute),
// resource bindings, and per-thread UV derivation differ from the original. Fixed at
// OperationMode 1 (RGBA) and edge direction off, matching upstream's own defaults; neither is
// runtime-configurable. EdgeThreshold/EdgeSharpness are fixed at 0.300/2.00 (set from the C++
// caller, not user-configurable) -- see the cbuffer below.
//
// VK_MODE bindings are stated explicitly (same rationale as dlssnr.hlsl's own header comment):
// D3D keeps b/t/u/s in separate register files, Vulkan has one number line per descriptor set.
// The order below (UBO 0, sampled image 1, storage image 2, sampler 3) matches SGSR1_Vk's
// descriptor set layout entry for entry, mirroring OS_Vk's own binding order.
//
// DLSS-NR's Neutwo/Hybrid reversible mapping (dlssnr.hlsl) writes its answer/proxy textures as
// LinearToSrgb(NeutwoEncode(normalized)) or LinearToSrgb(HybridEncode(normalized)), not plain
// sRGB gamma -- a second curve stacked on top of the gamma encode. EdgeThreshold defaults to
// upstream's own fixed 8/255, tuned against ordinary gamma-encoded content; reported in-game as
// blur under Neutwo/Hybrid + reduced model resolution, worst in "Replace" mode, where nothing
// downstream recomposites over this pass's output the way Composed mode's ratio/hue blend does.
// DecodeDomain/EncodeDomain strip and reapply the OUTER gamma only around the edge-directed math
// below, landing on NeutwoEncode(N)/HybridEncode(N) itself rather than gamma -- moving the
// threshold off the curve it wasn't tuned for without ever touching the unbounded scene-linear N
// behind it (an earlier version of this fix went all the way to N via NeutwoDecode/HybridDecode,
// then re-entered LinearToSrgb, whose saturate crushed every highlight above the white point flat
// -- see DecodeDomain's own comment). Identity when ReversibleMode is 0 (soft knee), 5-7 (the signal
// curves, whose stored value is already what the model reads) or the frame is passthrough, so those
// cases are untouched byte-for-byte.
//
//============================================================================================================

#ifdef VK_MODE
[[vk::binding(1, 0)]]
#endif
Texture2D<float4>   InputTexture       : register(t0);
#ifdef VK_MODE
[[vk::binding(2, 0)]]
#endif
RWTexture2D<float4>  OutputTexture      : register(u0);
#ifdef VK_MODE
[[vk::binding(3, 0)]]
#endif
SamplerState         LinearClampSampler : register(s0);

#ifdef VK_MODE
[[vk::binding(0, 0)]]
#endif
cbuffer Params : register(b0)
{
    // x = 1/srcWidth, y = 1/srcHeight, z = srcWidth, w = srcHeight -- derived directly from
    // the original shader's own imgCoord/coord math (imgCoord = uv*con1.zw - (0.5,-0.5),
    // coord = imgCoordPixel*con1.xy), not from external docs.
    float4 ViewportInfo;
    int2   DstSize;
    uint   ReversibleMode; // matches dlssnr.hlsl's gReversibleMode: 0 off, 1/2 Neutwo, 3/4 hybrid, 5-7 signal curves
    uint   Passthrough;
    // Retuned versions of upstream's own fixed kEdgeThreshold/kEdgeSharpness (8/255, 2.0) -- an
    // in-game A/B found the vote firing on noisy high-frequency content (skin, hair) upstream's
    // fixed threshold wasn't tuned for, smoothing detail plain bilinear preserved. Fixed constants
    // (0.300/2.00) passed in via the cbuffer from the C++ caller, not user-configurable.
    float  EdgeThreshold;
    float  EdgeSharpness;
};

// -- Outer-gamma strip/reapply around the edge-directed math (see file header) ---------------

float3 LinearToSrgb(float3 v)
{
    v = saturate(v);
    return lerp(v * 12.92, 1.055 * pow(max(v, 1e-8), 1.0 / 2.4) - 0.055, step(0.0031308, v));
}

float3 SrgbToLinear(float3 v)
{
    v = saturate(v);
    return lerp(v / 12.92, pow((v + 0.055) / 1.055, 2.4), step(0.04045, v));
}

// Strips only the OUTER sRGB gamma, landing on NeutwoEncode(N)/HybridEncode(N) itself -- the
// value the reversible encode already bounded to [0,1) -- rather than continuing on through
// NeutwoDecode/HybridDecode to the unbounded scene-linear N behind it. That unbounded value is
// exactly what Neutwo/Hybrid exist to represent for highlights above the white point (N > 1),
// and re-entering it through LinearToSrgb, which saturates its input to [0,1] before gamma
// encoding, crushed every such pixel flat to 1.0 -- a first version of this fix did exactly that,
// silently destroying highlight detail before SGSR1 ever ran. Staying inside the already-bounded
// curve domain the whole time makes SrgbToLinear/LinearToSrgb genuine inverses here (no
// saturation ever triggers for a value already in [0,1)), so this is lossless, and it still moves
// the edge-vote/weight math below off the outer gamma curve and onto the reversible mapping's own
// shape, closer to what SGSR1's fixed threshold was tuned against.
// Only Neutwo and the hybrid (modes 1-4) are shaped in light under an outer sRGB gamma. The knee (0) and the signal
// curves (5 HLG, 6 PQ, 7 linear) already store the value the model reads, so SGSR1 works on it as it is.
bool StripsOuterGamma() { return Passthrough == 0 && ReversibleMode >= 1 && ReversibleMode <= 4; }

float3 DecodeDomain(float3 c)
{
    return StripsOuterGamma() ? SrgbToLinear(c) : c;
}

// Exact inverse of DecodeDomain (a plain sRGB gamma encode of an already-[0,1) value has no
// saturation risk either), applied to the finished (already sharpened) colour right before it is
// written out, so downstream (the resolve's own SrgbToLinear + NeutwoDecode/HybridDecode) sees
// the same curve it would have from an un-upscaled answer.
float3 EncodeDomain(float3 c)
{
    return StripsOuterGamma() ? LinearToSrgb(c) : c;
}

// Same gamma-only strip as DecodeDomain, for the Gather taps below: GatherGreen only ever returns
// the green channel of its four neighbours, so this is naturally single-channel and needs no
// peak-channel logic (there is none to preserve -- it isn't touching the Neutwo/Hybrid curve at
// all, just the outer gamma, same as DecodeDomain).
float DecodeGreenScalar(float g)
{
    return StripsOuterGamma() ? SrgbToLinear(float3(g, g, g)).x : g;
}

float fastLanczos2(float x)
{
    float wA = x - 4.0;
    float wB = x * wA - wA;
    wA *= wA;
    return wB * wA;
}

// Non-edge-direction variant (edge direction off by default upstream too).
float2 weightY(float dx, float dy, float c, float std)
{
    float x = ((dx * dx) + (dy * dy)) * 0.5 + saturate(abs(c) * std);
    float w = fastLanczos2(x);
    return float2(w, w * c);
}

float4 GatherGreen(float2 uv)
{
    return InputTexture.GatherGreen(LinearClampSampler, uv);
}

// The edge-directed upsample. `uv` is the destination pixel's normalized coordinate.
float4 SgsrUpscale(float2 uv, float4 con1)
{
    float4 rawSample = InputTexture.SampleLevel(LinearClampSampler, uv, 0.0);

    // The base colour: exact, hue-preserving decode (a real RGB triple, not a Gather tap), so
    // this is what deltaY below gets added onto and what EncodeDomain re-curves at the end.
    float4 pix;
    pix.xyz = DecodeDomain(rawSample.xyz);

    // The edge-vote/weight subsystem below only ever sees green-channel Gather taps, so it works
    // in the single-channel approximation of the same curve (DecodeGreenScalar) throughout,
    // rather than mixing it with the exact vector decode above -- greenC stands in for pix.y in
    // all of it, matching upstream's own choice to vote/correct on the green channel alone.
    float greenC = DecodeGreenScalar(rawSample.y);

    float2 imgCoord = (uv * con1.zw) + float2(-0.5, 0.5);
    float2 imgCoordPixel = floor(imgCoord);
    float2 coord = imgCoordPixel * con1.xy;
    float2 pl = imgCoord - imgCoordPixel;
    float4 left = GatherGreen(coord);
    left = float4(DecodeGreenScalar(left.x), DecodeGreenScalar(left.y), DecodeGreenScalar(left.z),
                  DecodeGreenScalar(left.w));

    // OperationMode 1 (RGBA) always edge-votes/edge-corrects on the green channel (pix.y),
    // matching upstream's SGSRH(coord, mode) dispatch collapsed to its mode==1 case.
    float edgeVote = abs(left.z - left.y) + abs(greenC - left.y) + abs(greenC - left.z);
    if (edgeVote > EdgeThreshold)
    {
        coord.x += con1.x;

        float4 right = GatherGreen(coord + float2(con1.x, 0.0));
        right = float4(DecodeGreenScalar(right.x), DecodeGreenScalar(right.y), DecodeGreenScalar(right.z),
                       DecodeGreenScalar(right.w));
        float4 upDown;
        upDown.xy = GatherGreen(coord + float2(0.0, -con1.y)).wz;
        upDown.zw = GatherGreen(coord + float2(0.0, con1.y)).yx;
        upDown = float4(DecodeGreenScalar(upDown.x), DecodeGreenScalar(upDown.y), DecodeGreenScalar(upDown.z),
                        DecodeGreenScalar(upDown.w));

        float mean = (left.y + left.z + right.x + right.w) * 0.25;
        left -= mean;
        right -= mean;
        upDown -= mean;
        pix.w = greenC - mean;

        float sum = (abs(left.x) + abs(left.y) + abs(left.z) + abs(left.w)) +
                    (abs(right.x) + abs(right.y) + abs(right.z) + abs(right.w)) +
                    (abs(upDown.x) + abs(upDown.y) + abs(upDown.z) + abs(upDown.w));
        float sumMean = 10.14185 / sum;
        float std = sumMean * sumMean;

        float2 aWY = weightY(pl.x, pl.y + 1.0, upDown.x, std);
        aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, std);
        aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, std);
        aWY += weightY(pl.x, pl.y - 2.0, upDown.w, std);
        aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x, std);
        aWY += weightY(pl.x, pl.y - 1.0, left.y, std);
        aWY += weightY(pl.x, pl.y, left.z, std);
        aWY += weightY(pl.x + 1.0, pl.y, left.w, std);
        aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x, std);
        aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y, std);
        aWY += weightY(pl.x - 2.0, pl.y, right.z, std);
        aWY += weightY(pl.x - 1.0, pl.y, right.w, std);

        float finalY = aWY.y / aWY.x;

        float max4 = max(max(left.y, left.z), max(right.x, right.w));
        float min4 = min(min(left.y, left.z), min(right.x, right.w));
        finalY = clamp(EdgeSharpness * finalY, min4, max4);

        float deltaY = finalY - pix.w;

        pix.x = saturate(pix.x + deltaY);
        pix.y = saturate(pix.y + deltaY);
        pix.z = saturate(pix.z + deltaY);
    }

    // Re-curve back into the domain the resolve pass expects (identity outside Neutwo/Hybrid).
    pix.xyz = EncodeDomain(pix.xyz);

    // Matches OS_Dx12's own resize kernels (bcds_*), which also force alpha to 1 on their
    // resized output -- the resolve pass that reads this answer only ever uses .rgb.
    pix.w = 1.0;
    return pix;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if ((int) id.x >= DstSize.x || (int) id.y >= DstSize.y)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2((float) DstSize.x, (float) DstSize.y);
    OutputTexture[id.xy] = SgsrUpscale(uv, ViewportInfo);
}
