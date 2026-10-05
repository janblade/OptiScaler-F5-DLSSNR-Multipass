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
// texture, s0 a static linear-clamp sampler, u0 the graded output, b0 these constants.
//
// Decode is deliberately NOT dlssnr.hlsl's DecodeGameColour/EncodeGameColour: those land in the model's own
// working domain (tone-mapped sRGB OR linear light, depending on gInputEncoding there), not in the
// gamma-encoded display-referred 0-1 a .cube LUT is defined over. This shader decodes straight to display
// 0-1 with the same low-level curves dlssnr.hlsl itself is built from (LinearToSrgb/SrgbToLinear and
// NeutwoEncode/NeutwoDecode from dlssnr_replace_curve.hlsli, DecodePQ/EncodePQ from dlssnr_pq.hlsli), so a
// gamma 2.2 (and passthrough/tone-mapped-sRGB) frame is already there with nothing to convert, a PQ frame
// is tone-mapped through the same ITU-R BT.2408 reference-white convention Finished Picture/
// PqToReferenceLinear use before the sRGB encode, and open-ended scene-linear light (colourIsLinearHdr) is
// divided by trim and compressed with Neutwo -- the same reversible proxy curve the model's own encode
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
    // What to divide linear light by before compressing it, matching DLSS-NR's own default exposure trim
    // (DlssNr::AutoTrimEffective) when nothing more specific has been measured yet for this frame.
    float trim;
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
[[vk::binding(4, 0)]] [[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> gTarget : register(u0);

static const uint kInputGamma22 = 3u;
static const uint kInputPq = 4u;
static const float kScrgbPerReferenceWhite = 203.0 / 80.0; // mirrors dlssnr.hlsl's own constant of the same name

// PQ -> this pass's reference-linear light, exactly as dlssnr.hlsl's PqToReferenceLinear reads it (ST 2084,
// BT.2020 -> BT.709, ITU-R BT.2408 reference white at 1.0), then LinearToSrgb lands it in display 0-1.
float3 DecodeToDisplay01(float3 c)
{
    if (inputEncoding == kInputPq)
        return LinearToSrgb(mul(kBt2020To709, DecodePQ(c)) / kScrgbPerReferenceWhite);

    if (colourIsLinearHdr != 0)
    {
        // Open-ended scene-linear light: divide by trim (the same default the model's own exposure uses
        // absent a better measurement) to land roughly near 1.0, then Neutwo compresses [0, inf) into
        // [0, 1) with no hard clip -- the same reversible proxy curve dlssnr_replace_curve.hlsli's own
        // Encode pass uses, so a bright highlight fades toward white instead of being clipped flat there.
        return NeutwoEncode(max(c, 0.0) / max(trim, 1e-4));
    }

    // Gamma 2.2, passthrough and tone-mapped-sRGB frames are already display-referred: a .cube LUT is
    // defined over exactly this domain, so there is nothing to convert, only to clamp before the sample.
    return saturate(c);
}

float3 EncodeFromDisplay01(float3 c)
{
    if (inputEncoding == kInputPq)
        return EncodePQ(mul(kBt709To2020, SrgbToLinear(c) * kScrgbPerReferenceWhite));

    if (colourIsLinearHdr != 0)
        return NeutwoDecode(saturate(c)) * max(trim, 1e-4);

    return c;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;

    const float4 raw = gSource.Load(int3(id.xy, 0));
    const float3 display = DecodeToDisplay01(raw.rgb);

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
    gTarget[id.xy] = float4(EncodeFromDisplay01(result), raw.a);
}
