// Story 2 of the LUT-apply epic (dlssnr-lut-apply): grades the NR input image
// through a loaded 3D LUT (DlssNr_LutFile.h's Lut3D, uploaded by DlssNr_Dx12's DispatchLut) before the model
// ever sees it.
//
// Its own tiny root signature and descriptor table -- not dlssnr.hlsl's shared t0..t4/u0..u1 table -- because
// it is the first pass in this family to sample a 3D texture, and keeping it separate means every other
// pass's dispatch call sites and compiled bytecode stay untouched (DlssNr_Dx12.h's DispatchLut sits beside
// DispatchExposureAdapt/DispatchDetailReuse, with its own PSO and heap ring, not the shared one).
//
// t0 the pass's input (the real NGX colour resource, before Encode ever sees it), t1 the LUT as a 3D
// texture, t2 the live 1x1 exposure the encode reads (a stand-in when there is none), s0 a static linear-clamp
// sampler, u0 the graded output, b0 these constants.
//
// Decode is deliberately NOT dlssnr.hlsl's DecodeGameColour/EncodeGameColour: those land in the model's own
// working domain (tone-mapped sRGB OR linear light, depending on gInputEncoding there), not in the
// gamma-encoded display-referred 0-1 a .cube LUT is defined over. This shader decodes straight to display
// 0-1 with the same low-level curves dlssnr.hlsl itself is built from (LinearToSrgb/SrgbToLinear and
// NeutwoEncode/NeutwoDecode from dlssnr_replace_curve.hlsli, DecodePQ/EncodePQ from dlssnr_pq.hlsli), so a
// gamma 2.2 (and passthrough/tone-mapped-sRGB) frame is already there with nothing to convert, a PQ frame
// is tone-mapped through the same ITU-R BT.2408 reference-white convention Finished Picture/
// PqToReferenceLinear use before the sRGB encode, and open-ended scene-linear light (colourIsLinearHdr) is
// divided by the encode's white point and compressed with Neutwo -- the same reversible proxy curve the model's own encode
// pass uses, not a new one invented for this pass -- before the sRGB encode.
//
// Fixed (Review Pass, 2026-10-04): this used to fall through scene-linear HDR to the gamma-2.2 branch
// (`saturate(c)`), because inputEncoding alone cannot tell "already display-referred" apart from "wide open
// linear light" -- DlssNrColourEncoding::ShaderConversion collapses both to the same value. A LinearHDR
// frame (the normal outcome for most HDR games) was being clipped straight to 0-1 with no tonemap at all.

#include "dlssnr_pq.hlsli"          // DecodePQ/EncodePQ, kBt2020To709/kBt709To2020
#include "dlssnr_replace_curve.hlsli" // LinearToSrgb/SrgbToLinear

#ifdef VK_MODE
[[vk::binding(0, 0)]]
#endif
cbuffer Params : register(b0)
{
    uint width;
    uint height;
    float strength; // 0..1 (clamped on the host): how much of the graded result reaches the output
    float domainMinR, domainMinG, domainMinB;
    float domainMaxR, domainMaxG, domainMaxB;
    uint lutSize;       // the cube's side length; coordinates are clamped to the domain before sampling
    uint inputEncoding; // DlssNrConstants::InputEncoding's values: 3 gamma 2.2, 4 PQ, else passthrough/sRGB
    // Fixed (Review Pass, 2026-10-04): inputEncoding's "else" bucket covers BOTH tone-mapped sRGB (already
    // display 0-1, nothing to do) AND open-ended scene-linear HDR (needs compressing first) -- it cannot
    // tell them apart (DlssNrColourEncoding::ShaderConversion collapses both to the same value). This
    // carries the real signal (frame.ColourIsLinearHdr) so the two are no longer treated as one.
    uint colourIsLinearHdr;
    // What to divide linear light (and PQ's decoded light) by before compressing it: the white point the encode uses
    // this frame, as the host resolved it. Named as dlssnr.hlsl names it, for dlssnr_whitepoint.hlsli. When the
    // encode recomputes its white point from a live exposure texture, so does this pass (the fields below, copied from
    // the encode's constants), so the LUT grades for exactly the white the model is shown, not a value 3-4 frames old.
    float gWhitePoint;
    float gPreExposure;
    uint  gUseGameExposure;
    uint  gUseExposureWhitePoint;
    float gExposureBaseScale;
    float gExposureTrim;
    uint  gExposureTrimPreview;
    uint  gExposureTrimAnchorCount;
    float gExposureTrimAnchorExposure0;
    float gExposureTrimAnchorTrim0;
    float gExposureTrimAnchorExposure1;
    float gExposureTrimAnchorTrim1;
    float gExposureTrimAnchorExposure2;
    float gExposureTrimAnchorTrim2;
    float gExposureTrimAnchorExposure3;
    float gExposureTrimAnchorTrim3;
    float gExposureTrimAnchorExposure4;
    float gExposureTrimAnchorTrim4;
    float gExposureTrimAnchorExposure5;
    float gExposureTrimAnchorTrim5;
    float gExposureTrimAnchorExposure6;
    float gExposureTrimAnchorTrim6;
    float gExposureTrimAnchorExposure7;
    float gExposureTrimAnchorTrim7;
    uint  sceneLinear;   // post-NR stack: the lattice reads LogC4 of scene-linear light (see SceneLinearGrade)
    float sceneExposure; // linear multiplier on that light (2^EV)
};

#ifdef VK_MODE
[[vk::binding(1, 0)]]
#endif
Texture2D<float4> gSource : register(t0);
#ifdef VK_MODE
[[vk::binding(2, 0)]]
#endif
Texture3D<float4> gLut : register(t1);
#ifdef VK_MODE
[[vk::binding(3, 0)]]
#endif
SamplerState gLutSampler : register(s0);
#ifdef VK_MODE
// LUT_ANY_FORMAT_TARGET: the post-NR stack grades the game's own image in place, whatever its format, so that
// variant declares the format unknown (the device must allow storage writes without one). Left out, dxc would declare
// rgba32f from the float4, which must then match the image's own format.
#ifdef LUT_ANY_FORMAT_TARGET
[[vk::binding(4, 0)]] [[vk::image_format("unknown")]]
#else
[[vk::binding(4, 0)]] [[vk::image_format("rgba16f")]]
#endif
#endif
RWTexture2D<float4> gTarget : register(u0);
#ifdef VK_MODE
[[vk::binding(5, 0)]]
#endif
Texture2D<float4> gExposureLive : register(t2);

// The live exposure texel, for dlssnr_whitepoint.hlsli; read only when gUseGameExposure or gUseExposureWhitePoint is set.
float ExposureSample() { return gExposureLive.Load(int3(0, 0, 0)).r; }

#include "dlssnr_whitepoint.hlsli"

static const uint kInputGamma22 = 3u;
static const uint kInputPq = 4u;
static const float kScrgbPerReferenceWhite = 203.0 / 80.0; // mirrors dlssnr.hlsl's own constant of the same name

// PQ -> this pass's reference-linear light, exactly as dlssnr.hlsl's PqToReferenceLinear reads it (ST 2084,
// BT.2020 -> BT.709, ITU-R BT.2408 reference white at 1.0), put on the game's HDR paper white (the encode's white point
// for forced PQ, 1.0 at the default 203 nits), then LinearToSrgb lands it in display 0-1.
float3 DecodeToDisplay01(float3 c, float white)
{
    if (inputEncoding == kInputPq)
        return LinearToSrgb(mul(kBt2020To709, DecodePQ(c)) / kScrgbPerReferenceWhite / white);

    if (colourIsLinearHdr != 0)
    {
        // Open-ended scene-linear light: divide by the encode's white point to land near 1.0, then Neutwo
        // compresses [0, inf) into [0, 1) with no hard clip -- the same reversible proxy curve
        // dlssnr_replace_curve.hlsli's own Encode pass uses, so a bright highlight fades toward white instead of
        // being clipped flat there.
        return NeutwoEncode(max(c, 0.0) / white);
    }

    // Gamma 2.2, passthrough and tone-mapped-sRGB frames are already display-referred: a .cube LUT is
    // defined over exactly this domain, so there is nothing to convert, only to clamp before the sample.
    return saturate(c);
}

float3 EncodeFromDisplay01(float3 c, float white)
{
    if (inputEncoding == kInputPq)
        return EncodePQ(mul(kBt709To2020, SrgbToLinear(c) * white * kScrgbPerReferenceWhite));

    if (colourIsLinearHdr != 0)
        return NeutwoDecode(saturate(c)) * white;

    return c;
}

// The lattice at `v`, clamped to its domain, with the half-texel inset that puts its corners on the edge texels.
float3 SampleCube(float3 v)
{
    const float3 domainMin = float3(domainMinR, domainMinG, domainMinB);
    const float3 domainMax = float3(domainMaxR, domainMaxG, domainMaxB);
    const float3 uvw = (clamp(v, domainMin, domainMax) - domainMin) / max(domainMax - domainMin, 1e-6);
    const float lutSpan = max((float) lutSize, 2.0) - 1.0;
    const float3 texCoord = (uvw * lutSpan + 0.5) / (float) lutSize;
    return gLut.SampleLevel(gLutSampler, texCoord, 0).rgb;
}

// Scene-linear input (post-NR stack, Story 5): for view-transform LUTs, which read ARRI LogC4 in ARRI Wide Gamut 4 and
// write sRGB display 0..1. The frame's light is put on the game's paper white (1.0 = white, the same divisor the other
// branches use), times the user's exposure, turned from BT.709 into AWG4 and encoded with the published LogC4 curve.
// Checked against the LinRec709-to-ArriLogC4AWG4 LUT such packs ship (within 0.003 from mid grey up).
static const float3x3 kBt709ToAwg4 = {
    0.56583654, 0.34033100, 0.09383246,
    0.08862564, 0.80934684, 0.10202752,
    0.01775004, 0.10944762, 0.87280234,
};

float LogC4(float e)
{
    const float a = 2231.826309;
    const float b = 0.9071358749;
    const float c = 0.09286412512;
    const float s = 0.1135972086;
    const float t = -0.01805699612;
    return e >= t ? (log2(a * e + 64.0) - 6.0) / 14.0 * b + c : (e - t) / s;
}

// The frame's light on paper white. A tone-mapped SDR frame has none to give: its sRGB is decoded and a Reinhard-shaped
// tone curve undone (white lands at 63x paper white), an approximation of the light the game started from.
float3 SceneLight(float3 c, float white)
{
    if (inputEncoding == kInputPq)
        return mul(kBt2020To709, DecodePQ(c)) / kScrgbPerReferenceWhite / white;
    if (colourIsLinearHdr != 0)
        return max(c, 0.0) / white;
    const float3 y = SrgbToLinear(saturate(c));
    return y / max(1.0 - y, 1.0 / 64.0);
}

// The graded picture back in the frame's encoding, blended with the frame by `strength` in that encoding's light: the
// look's sRGB white lands on the game's paper white (an sRGB view transform makes an SDR picture), its highlights stop
// there.
float3 SceneLinearGrade(float3 c, float white)
{
    const float3 awg = mul(kBt709ToAwg4, SceneLight(c, white) * sceneExposure);
    const float3 graded = saturate(SampleCube(float3(LogC4(awg.r), LogC4(awg.g), LogC4(awg.b))));
    const float s = saturate(strength);

    if (inputEncoding == kInputPq)
    {
        const float3 frame = mul(kBt2020To709, DecodePQ(c));
        return EncodePQ(mul(kBt709To2020, lerp(frame, SrgbToLinear(graded) * white * kScrgbPerReferenceWhite, s)));
    }
    if (colourIsLinearHdr != 0)
        return lerp(max(c, 0.0), SrgbToLinear(graded) * white, s);
    return lerp(saturate(c), graded, s);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;

    // Uniform across the dispatch: the host's value, or recomputed from the live exposure as the encode does.
    // WhitePoint() floors the host value at 1e-4, as this pass's own max(trim, 1e-4) did.
    const float white = WhitePoint();

    const float4 raw = gSource.Load(int3(id.xy, 0));
    if (sceneLinear != 0u)
    {
        gTarget[id.xy] = float4(SceneLinearGrade(raw.rgb, white), raw.a);
        return;
    }

    const float3 display = DecodeToDisplay01(raw.rgb, white);

    const float3 domainMin = float3(domainMinR, domainMinG, domainMinB);
    const float3 domainMax = float3(domainMaxR, domainMaxG, domainMaxB);
    const float3 clamped = clamp(display, domainMin, domainMax);
    const float3 span = max(domainMax - domainMin, 1e-6);
    const float3 uvw = (clamped - domainMin) / span;

    // Half-texel inset: texel i's centre sits at (i + 0.5) / lutSize, so the lattice's own corners land
    // exactly on the texture's edge texels instead of bleeding into the clamp border.
    const float lutSpan = max((float) lutSize, 2.0) - 1.0;
    const float3 texCoord = (uvw * lutSpan + 0.5) / (float) lutSize;
    const float3 graded = gLut.SampleLevel(gLutSampler, texCoord, 0).rgb;

    const float3 result = lerp(display, graded, saturate(strength));
    gTarget[id.xy] = float4(EncodeFromDisplay01(result, white), raw.a);
}
