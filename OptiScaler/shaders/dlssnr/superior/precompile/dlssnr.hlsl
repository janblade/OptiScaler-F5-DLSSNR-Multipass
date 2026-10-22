// FROZEN PORT BASELINE -- do not edit for NR features.
//
// This is a second, frozen copy of the NR shader: mattjaas' Superior Scaling v59 (914d8d1f), kept so the Superior
// passes keep v59's exact bytecode. It does NOT receive the composition, exposure, colour-encoding or any other change
// made to the live shader (OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl); fixes there do not reach it.
//
// Compiled by tools/compile_nr_interpass.ps1 into DlssNr_Shader, dlssnr_interpass_* and dlssnr_superior_* (and,
// through superior/RawLow.hlsl, host_low_raw). Only Superior Scaling uses them. DlssNr_Shader.cso (this file with no
// defines) is the fallback PSO for every Superior pass without a specialised PSO (superior/Shader.inl,
// DispatchSuperiorPass): the unit exposure (mode 7), the clamp between NR passes (mode 8) and the guide matching for
// the private DLSS (mode 10) always use it; the Area downsample (2), final low field (18) and inter-pass passes (34, 36)
// fall back to it only if their own PSO cannot be created. The final composition is not here: it is the live shader
// with superior/HostResolve.hlsli (host_resolve_* / host_normal_* / host_cached_*).
//
// Its hash is pinned by docs/superior-v59-shaders.json (tests/test_nr_superior_scaling.py).

#if defined(VK_MODE)
[[vk::binding(0, 0)]]
cbuffer Params : register(b0, space0)
#else
cbuffer Params : register(b0)
#endif
{
    uint  gMode;
    float gWhitePoint;
    uint  gWidth;
    uint  gHeight;
    float gTransferStrength;
    float gColourStrength;
    uint  gDebugView;
    float gMaxRatio;
    uint  gPassthrough;
    float gMvScaleX;     // motion vector units -> pixels of this dispatch
    float gMvScaleY;
    uint  gGuideWidth;   // the motion texture's valid region
    uint  gGuideHeight;
    uint  gCompareMode;  // 0 off, 1 side by side, 2 wipe
    float gCompareSplit; // where the wipe cuts, 0..1
    float gCompareZoom;  // side by side: 1 fits the frame, 2 fills the half
    uint  gCompareSwap;  // put the edited frame on the other side
    uint  gTransfer;     // 0 classic, 1/2 residual, 3/4 lighting+colour, 6 P100 residual, 7 Direct fused, 8 P100-guided residual
    float gDebugScale;   // what the debug views are scaled by, held still while the meter moves
    uint  gReversibleMode; // 0 knee, 1 Neutwo+composed, 2 Neutwo+replace, 3 hybrid+composed, 4 hybrid+replace
    uint  gApplyModel;     // 0 output the clean frame (pass still runs), 1 apply the model's edit
    float gMaxDarkening;    // percent: 100 = uncapped, 0 = no darkening
    float gResidualScale;
    uint  gSkinProtection;
    uint  gShowSkinMask;
    float gSkinDetail;
    float gSkinColour;
    float gEnvironmentDetail;
    float gEnvironmentColour;
    float gResidualBlendUnused;
    uint gResidualHistoryValidUnused, gResidualMotionBaseXUnused, gResidualMotionBaseYUnused;
    float gReplaceDetailStrength, gModelWorkScale, gResidualConfidenceUnused;
    uint gExposureMode;
    float gPreExposure, gExposureTrim, gExposureProtection;
    uint gExposureAnchorCount, gExposureSourceWidth, gExposureSourceHeight, gExposurePadding;
    float4 gExposureAnchors[4];
    uint gDirectDetailMode;
    float gDirectDetailMaskStrength;
    uint gDirectResolveUpscaler;
    uint gDirectResolveFlags;
};

#if defined(DLSSNR_INTERPASS_MODE28)
// Isolated PSO selected only for Mode 28. Keep the constant-buffer layout
// unchanged, but let DXC eliminate unrelated modes and their groupshared
// reductions. The complete Mode 28 fallback remains available.
#define gMode 28u
#endif
#if defined(DLSSNR_INTERPASS_FAST)
// Isolate the approximate path; no native tile or unrelated reductions in its PSO.
#define gMode 33u
#endif
#if defined(DLSSNR_INTERPASS_LOW36)
#define gMode 34u
#endif
#if defined(DLSSNR_INTERPASS_CROSS5)
#define gMode 36u
#endif
#if defined(DLSSNR_SUPERIOR_RESOLVE)
// This PSO is selected only for final Resolve. Keep the cbuffer ABI, eliminate unrelated stages.
#define gMode 1u
#endif
#if defined(DLSSNR_SUPERIOR_ENCODE)
#define gMode 0u
#endif
#if defined(DLSSNR_SUPERIOR_METER)
#define gMode 3u
#endif
#if defined(DLSSNR_SUPERIOR_EXPOSURE)
#define gMode 11u
#endif
#if defined(DLSSNR_SUPERIOR_AREA)
#define gMode 2u
#define gTransfer 0u
#define DLSSNR_AREA_COORD precise
#define DLSSNR_AREA_ACC precise
#else
#define DLSSNR_AREA_COORD const
#define DLSSNR_AREA_ACC
#endif
#if defined(DLSSNR_SUPERIOR_LOW)
// Final decoded residual only; keep the original 64 taps, accumulation and FP16 store.
#define gMode 18u
#define gTransfer 1u
#endif
#if defined(DLSSNR_SUPERIOR_NORMAL)
// Selected only for the reduced signed-residual gameplay path. Inspector uses the general PSO.
#define gTransfer 9u
#define gDebugView 0u
#define gCompareMode 0u
#define gApplyModel 1u
#define gDirectResolveFlags 65536u
#if defined(DLSSNR_SUPERIOR_SDR)
#define gPassthrough 1u
#elif defined(DLSSNR_SUPERIOR_HDR)
#define gPassthrough 0u
#define gReversibleMode DLSSNR_SUPERIOR_HDR
#endif
#endif

float3 InterPassGuideBlend(float3 bilinear, float3 guided, float strength)
{
    return lerp(bilinear, guided, strength);
}


// Hue-preserving gamut compression toward the D65 neutral axis.
// Adapted from clshortfuse/RenoDX (https://github.com/clshortfuse/renodx).
// See Licenses/RenoDX_ATTRIBUTION.txt.

float SanitizeFinite(float v, float fallback) { return isfinite(v) ? v : fallback; }

float2 TemporalCarrierRange()
{
    const float kFp16Max = 65504.0;
    float low = clamp(SanitizeFinite(gWhitePoint, 0.0), -kFp16Max, kFp16Max);
    float high = clamp(SanitizeFinite(gDebugScale, 1.0), -kFp16Max, kFp16Max);
    if (!(high > low))
        return float2(0.0, 1.0);
    return float2(low, high);
}

bool TemporalCarrierDefaultRange(float2 range)
{
    return abs(range.x) <= 1.0e-6 && abs(range.y - 1.0) <= 1.0e-6;
}

// Approximate skin-colour selection, not a face/skin segmentation network. Warm
// materials may be selected and coloured lighting can hide skin. The preview is
// deliberately exposed so users can check this before relying on protection.
float SkinColourWeight(float3 rgb)
{
    rgb = saturate(rgb);
    float y = dot(rgb, float3(0.299, 0.587, 0.114));
    float cb = (rgb.b - y) * 0.564 + 0.5;
    float cr = (rgb.r - y) * 0.713 + 0.5;
    float2 distance = (float2(cb, cr) - float2(0.405, 0.600)) / float2(0.090, 0.110);
    float chroma = max(rgb.r, max(rgb.g, rgb.b)) - min(rgb.r, min(rgb.g, rgb.b));
    return (1.0 - smoothstep(0.55, 1.35, length(distance))) * smoothstep(0.02, 0.10, chroma);
}

float3 SanitizeFinite3(float3 v, float3 fallback)
{
    return float3(SanitizeFinite(v.x, fallback.x), SanitizeFinite(v.y, fallback.y),
                  SanitizeFinite(v.z, fallback.z));
}

float SafeDivide(float numerator, float denominator, float fallback)
{
    return abs(denominator) > 1e-8 ? numerator / denominator : fallback;
}

// Hunt-Pointer-Estevez LMS over linear BT.709, carrying the fixed D65 adaptation state the
// compression is defined against. The signal itself never leaves BT.709.
float3 LMSToBT709(float3 color)
{
    const float3x3 m = { 5.62059812, -4.57145756, 0.15577924,
                         -1.15555585, 2.25800438, -0.15415806,
                         0.03059913, -0.19018011, 1.06820532 };
    return mul(m, color);
}

float3 BT709ToLMS(float3 color)
{
    const float3x3 m = { 0.30569589, 0.62271286, 0.04528636,
                         0.15776262, 0.76968599, 0.08807030,
                         0.01933082, 0.11919478, 0.95053215 };
    return mul(m, color);
}

// The neutral colour of the same luminance as what is being compressed -- the point everything is
// pulled toward, so that pulling changes saturation and not hue.
float3 D65NeutralBT709(float3 adaptiveStateLms, float luminance)
{
    float3 d65 = LMSToBT709(max(adaptiveStateLms, 1e-8));
    float d65Y = max(dot(d65, float3(0.2126, 0.7152, 0.0722)), 1e-8);
    return d65 * (luminance / d65Y);
}

// The largest scale toward the neutral axis that leaves no channel negative. One for a colour that
// was already representable, which is why this is safe to run on every pixel.
float GamutCompressionScale(float3 color, float3 adaptiveStateLms)
{
    color = SanitizeFinite3(color, float3(0.0, 0.0, 0.0));

    const float y = dot(color, float3(0.2126, 0.7152, 0.0722));

    if (!(y > 1e-8))
        return 1.0;

    const float3 neutral = D65NeutralBT709(adaptiveStateLms, y);
    float scale = 1.0;

    if (color.r < 0.0 && neutral.r > color.r)
        scale = min(scale, SafeDivide(neutral.r, neutral.r - color.r, 1.0));

    if (color.g < 0.0 && neutral.g > color.g)
        scale = min(scale, SafeDivide(neutral.g, neutral.g - color.g, 1.0));

    if (color.b < 0.0 && neutral.b > color.b)
        scale = min(scale, SafeDivide(neutral.b, neutral.b - color.b, 1.0));

    return saturate(SanitizeFinite(scale, 1.0));
}

float3 ClampAp1(float3 color)
{
    const float3 adaptiveStateLms = BT709ToLMS(float3(0.18, 0.18, 0.18));
    const float scale = GamutCompressionScale(color, adaptiveStateLms);

    // Nothing was out of gamut. Leave the colour exactly as it arrived.
    if (scale >= 1.0)
        return color;

    const float y = dot(color, float3(0.2126, 0.7152, 0.0722));
    const float3 neutral = D65NeutralBT709(adaptiveStateLms, y);

    return SanitizeFinite3(neutral + (color - neutral) * scale, max(neutral, 0.0));
}

// ---------------------------------------------------------------------------------------------
// The composition below (UpgradeToneMap's two-branch ratio, the OkLab hue correction, and the blend
// between a luminance-only result and the model's own colour) is taken from RenoDX's DLSS 5 addon by
// clshortfuse -- https://github.com/clshortfuse/renodx. It is their design, not ours; see
// Licenses/RenoDX_LICENSE.txt. The OkLab matrices are Bjorn Ottosson's published constants and the
// AP1, sRGB and PQ transforms are standard colour science.
// ---------------------------------------------------------------------------------------------

// Use OkLab to retain the model's hue while adjusting chroma magnitude.
float3 CbrtSigned(float3 v) { return sign(v) * pow(abs(v), 1.0 / 3.0); }

float3 ToOkLab(float3 color)
{
    const float3x3 rgb_to_lms = { 0.4122214708, 0.5363325363, 0.0514459929,
                                  0.2119034982, 0.6806995451, 0.1073969566,
                                  0.0883024619, 0.2817188376, 0.6299787005 };
    const float3x3 lms_to_lab = { 0.2104542553, 0.7936177850, -0.0040720468,
                                  1.9779984951, -2.4285922050, 0.4505937099,
                                  0.0259040371, 0.7827717662, -0.8086757660 };
    return mul(lms_to_lab, CbrtSigned(mul(rgb_to_lms, color)));
}

float3 FromOkLab(float3 lab)
{
    const float3x3 lab_to_lms = { 1.0, 0.3963377774, 0.2158037573,
                                  1.0, -0.1055613458, -0.0638541728,
                                  1.0, -0.0894841775, -1.2914855480 };
    const float3x3 lms_to_rgb = { 4.0767416621, -3.3077115913, 0.2309699292,
                                  -1.2684380046, 2.6097574011, -0.3413193965,
                                  -0.0041960863, -0.7034186147, 1.7076147010 };
    float3 lms = mul(lab_to_lms, lab);
    return mul(lms_to_rgb, lms * lms * lms);
}

// Take hue direction from correct and chroma magnitude from incorrect.
float3 HueOkLab(float3 incorrect, float3 correct)
{
    float3 incorrectLab = ToOkLab(incorrect);
    const float3 correctLab = ToOkLab(correct);
    const float incorrectChroma = length(incorrectLab.yz);
    const float correctChroma = length(correctLab.yz);

    // Normalize hue direction before scaling; near-grey chroma must not amplify numerical noise.
    const float2 hueDirection = correctChroma > 1e-5 ? correctLab.yz / correctChroma : float2(0.0, 0.0);

    incorrectLab.yz = hueDirection * incorrectChroma;

    return ClampAp1(FromOkLab(incorrectLab));
}

// Bindings are stated for SPIR-V rather than inferred. D3D keeps b, t, u and s in separate register
// files, so b0 and t0 do not collide; Vulkan has one number line per descriptor set, and dxc's default
// mapping would put both at binding 0. The numbers below are the order the pass binds them in, and
// DlssNr_Vk's descriptor set layout has to agree with them entry for entry.
#if defined(VK_MODE)
[[vk::binding(1, 0)]]
#endif
Texture2D<float4>   gSource   : register(t0);  // encode: the frame. resolve: the proxy.
#if defined(VK_MODE)
[[vk::binding(2, 0)]]
#endif
Texture2D<float4>   gModel    : register(t1);  // resolve: what the model returned.
#if defined(VK_MODE)
[[vk::binding(3, 0)]]
#endif
Texture2D<float4>   gOriginal : register(t2);  // resolve: the untouched frame.
#if defined(VK_MODE)
[[vk::binding(4, 0)]]
#endif
Texture2D<float4>   gMotion   : register(t3);  // resolve, accumulating: the game's motion vectors.
#if !defined(VK_MODE)
Texture2D<float4>   gAux      : register(t4);  // DX12 Direct NR: packed P50.rgb + NR retention gate.
Texture2D<float4>   gAux2     : register(t5);  // DX12 Direct NR: selected full-resolution P50 reconstruction.
#endif

#if defined(VK_MODE)
[[vk::binding(5, 0)]]
#endif
RWTexture2D<float4> gTarget   : register(u0);  // encode: the proxy. resolve: the frame.
#if defined(VK_MODE)
[[vk::binding(6, 0)]]
#endif
RWTexture2D<float4> gKeep     : register(u1);  // encode: the untouched copy. unused by the resolve.
#if defined(VK_MODE)
[[vk::binding(7, 0)]]
#endif
SamplerState        gLinear   : register(s0);  // so the edit can be read at a different size

float4 DownsampleLoadClamped(int2 p, uint srcW, uint srcH)
{
    p = clamp(p, int2(0, 0), int2((int) srcW - 1, (int) srcH - 1));
    return gSource.Load(int3(p, 0));
}

float CatmullRomWeight(float x)
{
    x = abs(x);
    if (x < 1.0)
        return ((1.5 * x - 2.5) * x) * x + 1.0;
    if (x < 2.0)
        return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}

float SincPi(float x)
{
    const float ax = abs(x);
    if (ax < 1e-5)
        return 1.0;
    const float p = 3.14159265358979323846 * x;
    return sin(p) / p;
}

float Lanczos2Weight(float x)
{
    x = abs(x);
    return x < 2.0 ? SincPi(x) * SincPi(0.5 * x) : 0.0;
}

float4 DownsampleKernel4x4(float2 uv, uint srcW, uint srcH, bool lanczos)
{
    const float2 p = uv * float2(srcW, srcH) - 0.5;
    const int2 base = int2(floor(p));
    float4 sum = 0.0;
    float weightSum = 0.0;

    [unroll] for (int y = -1; y <= 2; ++y)
    {
        const float wy = lanczos ? Lanczos2Weight((float) (base.y + y) - p.y)
                                 : CatmullRomWeight((float) (base.y + y) - p.y);
        [unroll] for (int x = -1; x <= 2; ++x)
        {
            const float wx = lanczos ? Lanczos2Weight((float) (base.x + x) - p.x)
                                     : CatmullRomWeight((float) (base.x + x) - p.x);
            const float w = wx * wy;
            sum += DownsampleLoadClamped(base + int2(x, y), srcW, srcH) * w;
            weightSum += w;
        }
    }

    float4 filtered = sum / (abs(weightSum) > 1e-6 ? weightSum : 1.0);
    // Cubic/Lanczos negative lobes can overshoot an encoded proxy. Keep RGB in the model's legal range;
    // alpha follows the nearest source sample, matching the legacy area path's non-filtered alpha.
    const int2 center = int2(clamp(floor(uv * float2(srcW, srcH)), 0.0, float2(srcW - 1, srcH - 1)));
    filtered.rgb = saturate(filtered.rgb);
    filtered.a = DownsampleLoadClamped(center, srcW, srcH).a;
    return filtered;
}


float4 DownsampleSsimSharp(float2 uv, uint srcW, uint srcH, uint dstW, uint dstH)
{
    const float2 invDst = 1.0 / float2(dstW, dstH);
    const float2 srcPos = uv * float2(srcW, srcH) - 0.5;
    const int2 srcCenter = int2(floor(srcPos + 0.5));

    float3 lowMean = 0.0, lowSq = 0.0;
    float lowW = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const float w = pow(0.25, abs((float) x) + abs((float) y));
            const float2 nuv = saturate(uv + float2(x, y) * invDst);
            const float3 v = gSource.SampleLevel(gLinear, nuv, 0).rgb;
            lowMean += v * w;
            lowSq += v * v * w;
            lowW += w;
        }
    }
    lowMean /= lowW;
    lowSq /= lowW;

    float3 srcMean = 0.0, srcSq = 0.0;
    float srcWeight = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const float w = pow(0.25, abs((float) x) + abs((float) y));
            const float3 v = DownsampleLoadClamped(srcCenter + int2(x, y), srcW, srcH).rgb;
            srcMean += v * w;
            srcSq += v * v * w;
            srcWeight += w;
        }
    }
    srcMean /= srcWeight;
    srcSq /= srcWeight;

    const float3 lowVar = max(lowSq - lowMean * lowMean, 0.0);
    const float3 srcVar = max(srcSq - srcMean * srcMean, 0.0);
    float4 base = gSource.SampleLevel(gLinear, uv, 0);
    const float3 contrastGain = clamp(sqrt((srcVar + 1e-5) / (lowVar + 1e-5)), 0.85, 1.55);

    float3 result = lowMean + (base.rgb - lowMean) * contrastGain;
    result += 0.20 * (base.rgb - lowMean);

    base.rgb = saturate(result);
    base.a = DownsampleLoadClamped(srcCenter, srcW, srcH).a;
    return base;
}

float2 ExposureAnchor(uint i)
{
    return (i & 1u) ? gExposureAnchors[min(i / 2u, 3u)].zw : gExposureAnchors[min(i / 2u, 3u)].xy;
}
float WhitePoint()
{
    float base = max(gWhitePoint, 1e-4);
    if (gExposureMode == 1 || gExposureMode == 3)
    {
        float measured = gMotion.Load(int3(0, 0, 0)).r;
        if (isfinite(measured) && measured > 1e-8)
        {
        base = gExposureMode == 1 ? gPreExposure / measured : measured;
        float trim = gExposureTrim;
        uint count = min(gExposureAnchorCount, 8u);
        if (count > 0)
        {
            trim = ExposureAnchor(0).y;
            [unroll] for (uint i = 1; i < 8; ++i)
            {
                float2 previous = ExposureAnchor(i - 1);
                float2 next = ExposureAnchor(i);
                if (i < count && base > previous.x)
                {
                    float t = saturate(log2(max(base, 1e-8) / previous.x) / log2(next.x / previous.x));
                    trim = exp2(lerp(log2(previous.y), log2(next.y), t));
                }
            }
        }
        base *= trim;
        }
    }
    return max(SanitizeFinite(base, gWhitePoint), 1e-4);
}

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

// sRGB rather than a plain 2.2 power: it is what an SDR game buffer actually carries, and the model was
// trained on those.
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

// The edit at an arbitrary position, exactly as the resolve computes its own.
float3 EditAt(float2 uvq)
{
    float3 p = gSource.SampleLevel(gLinear, uvq, 0).rgb;
    float3 m = gModel.SampleLevel(gLinear, uvq, 0).rgb;

    if (gPassthrough == 0)
    {
        p = SrgbToLinear(p);
        m = SrgbToLinear(m);
    }

    return m - p;
}


// Soft-knee proxy shared by encoding and matched-residual reconstruction.
float3 SoftKnee(float3 display)
{
    if (gPassthrough != 0)
        return display;

    float displayLuma = dot(display, kLuma);

    if (displayLuma > 0.75)
    {
        float rolled = 0.75 + 0.25 * (1.0 - exp(-(displayLuma - 0.75) / 0.25));
        display *= rolled / displayLuma;
    }

    // Per-channel headroom, with the hue kept.
    //
    // The roll-off above is on luminance, and luminance is a weighted sum in which blue counts for
    // seven percent. A saturated blue can therefore sit at B = 2 with a luminance of 0.14, pass the
    // knee untouched, and be clipped per channel by the saturate in LinearToSrgb -- and clipping one
    // channel of a triple is a hue rotation, so blue arrives as cyan. That was the green cast over
    // every blue thing in GTA V at colour strength 1: the sky, the denim, the minimap. The model was
    // shown a cyan proxy, answered in cyan, and at colour strength 1 its hue is the frame's hue.
    //
    // One scalar on the whole triple cannot move hue, so the peak channel is brought to 1 that way.
    // Only pixels that were already being clipped are touched, so everything else is bit-identical
    // to before, and the resolve's reconstruction of this proxy stays exact because it goes through
    // this same function.
    float peak = max(display.r, max(display.g, display.b));

    if (peak > 1.0)
        display /= peak;

    return display;
}

// Unclipped, hue-preserving Neutwo proxy adapted from clshortfuse/RenoDX.
// One scalar maps the peak channel to [0,1), preserving RGB ratios.
// Negative input channels are clamped before encoding. See Licenses/RenoDX_LICENSE.txt.
float Neutwo(float x) { return x * rsqrt(x * x + 1.0); } // [0, inf) -> [0, 1), no clip point

float3 NeutwoEncode(float3 v)
{
    v = max(v, 0.0);
    float m = max(v.r, max(v.g, v.b));

    if (m <= 1e-6)
        return v;

    // One scalar taken from the peak channel keeps the hue; the peak lands at Neutwo(m) < 1, so no
    // channel clips and LinearToSrgb's saturate never fires -- the proxy is fully invertible.
    return v * (Neutwo(m) / m);
}

// Replace-mode inverse of NeutwoEncode. Clamp below its pole at 1 to keep highlights finite.
float3 NeutwoDecode(float3 y)
{
    y = max(y, 0.0);
    float m = max(y.r, max(y.g, y.b));
    m = min(m, 0.999999);

    if (m <= 1e-6)
        return y;

    float x = m * rsqrt(max(1.0 - m * m, 1e-8)); // Neutwo^-1 of the peak
    return y * (x / m);
}

// Hybrid proxy: identity below the knee, a C1-continuous Neutwo rolloff above it.
float HybridCurve(float m)
{
    const float k = 0.75; // knee point: identity below, gentle unclipped roll above

    if (m <= k)
        return m;

    const float e = (m - k) / (1.0 - k);         // excess above the knee, [0, inf)
    return k + (1.0 - k) * (e * rsqrt(e * e + 1.0)); // Neutwo(e) scaled into [k, 1); -> 1, never clips
}

float3 HybridEncode(float3 v)
{
    v = max(v, 0.0);
    float m = max(v.r, max(v.g, v.b));

    if (m <= 1e-6)
        return v;

    // One scalar on the peak channel, hue preserved. Below the knee the scalar is 1 (identity); above
    // it the peak lands at HybridCurve(m) < 1, so no channel clips.
    return v * (HybridCurve(m) / m);
}

// The exact inverse of the hybrid curve, for the hybrid REPLACE decode (mode 4). Because it is IDENTITY
// below the knee, the steep expansion is confined to genuine highlights: midtone model wobble is not
// amplified, so hybrid-replace flashes far less than Neutwo-replace while keeping the raw model detail.
float HybridCurveInv(float y)
{
    const float k = 0.75;

    if (y <= k)
        return y;

    float u = (y - k) / (1.0 - k);                  // Neutwo(e), in [0,1)
    u = min(u, 0.999999);                           // the inverse diverges at 1
    const float e = u * rsqrt(max(1.0 - u * u, 1e-8)); // Neutwo^-1 of the excess
    return k + (1.0 - k) * e;
}

float3 HybridDecode(float3 y)
{
    y = max(y, 0.0);
    float m = max(y.r, max(y.g, y.b));

    if (m <= 1e-6)
        return y;

    return y * (HybridCurveInv(m) / m);
}

// Scale a residual so the result cannot leave the unit cube, without changing its direction.
//
// The model's edit is carried up from a smaller raster and laid on the frame's own proxy, so nothing
// guarantees the sum is still a colour. Clamping per channel would bend the hue -- the channel that
// hits the wall first decides the colour of the rest -- so the whole residual is scaled by the
// largest factor that keeps every channel inside, and the direction survives.
//
// hhkbble's, from the multi-pass PR against this fork.
float3 CubeScaleResidual(float3 P, float3 T)
{
    if (gPassthrough != 0)
        return T;

    float3 d = T - P;
    float alpha = 1.0;

    [unroll] for (int c = 0; c < 3; ++c)
    {
        if (d[c] > 1e-6)
            alpha = min(alpha, (1.0 - P[c]) / d[c]);
        else if (d[c] < -1e-6)
            alpha = min(alpha, (0.0 - P[c]) / d[c]);
    }

    return P + saturate(alpha) * d;
}

#if !defined(VK_MODE)
float DirectDetailGateAt(int2 p)
{
    const int2 lo = int2(0, 0);
    const int2 hi = int2((int) gWidth - 1, (int) gHeight - 1);
    p = clamp(p, lo, hi);
    const int2 px0 = clamp(p + int2(-1, 0), lo, hi);
    const int2 px1 = clamp(p + int2( 1, 0), lo, hi);
    const int2 py0 = clamp(p + int2(0, -1), lo, hi);
    const int2 py1 = clamp(p + int2(0,  1), lo, hi);

    float3 pc = gSource.Load(int3(p, 0)).rgb;
    float3 nc = gModel.Load(int3(p, 0)).rgb;
    float3 pl = gSource.Load(int3(px0, 0)).rgb;
    float3 pr = gSource.Load(int3(px1, 0)).rgb;
    float3 pu = gSource.Load(int3(py0, 0)).rgb;
    float3 pd = gSource.Load(int3(py1, 0)).rgb;
    float3 nl = gModel.Load(int3(px0, 0)).rgb;
    float3 nr = gModel.Load(int3(px1, 0)).rgb;
    float3 nu = gModel.Load(int3(py0, 0)).rgb;
    float3 nd = gModel.Load(int3(py1, 0)).rgb;

    if (gPassthrough == 0)
    {
        pc = SrgbToLinear(pc); nc = SrgbToLinear(nc);
        pl = SrgbToLinear(pl); pr = SrgbToLinear(pr);
        pu = SrgbToLinear(pu); pd = SrgbToLinear(pd);
        nl = SrgbToLinear(nl); nr = SrgbToLinear(nr);
        nu = SrgbToLinear(nu); nd = SrgbToLinear(nd);
    }

    const float floorY = 1.0 / 512.0;
    const float pY = dot(pc, kLuma);
    const float nY = dot(nc, kLuma);
    const float pBlur = dot(pc + pl + pr + pu + pd, kLuma) / 5.0;
    const float nBlur = dot(nc + nl + nr + nu + nd, kLuma) / 5.0;
    const float pDetail = (pY - pBlur) / (max(pY, pBlur) + floorY);
    const float nDetail = (nY - nBlur) / (max(nY, nBlur) + floorY);

    float gate = 1.0;
    const float pMag = abs(pDetail);
    const float nMag = abs(nDetail);
    if (pMag > 0.010)
    {
        const float retained = nMag / max(pMag, 1e-5);
        gate = smoothstep(0.20, 0.70, retained);
        if (pDetail * nDetail < 0.0 && nMag > 0.20 * pMag)
            gate = 0.0;
    }
    return saturate(gate);
}

float DirectMagicKernel(float x)
{
    const float ax = abs(x);
    if (ax >= 1.5)
        return 0.0;
    if (ax <= 0.5)
        return 0.75 - ax * ax;
    const float t = ax - 1.5;
    return 0.5 * t * t;
}

float3 DirectMagicPixel(int2 outP)
{
    uint srcW, srcH;
    gModel.GetDimensions(srcW, srcH);
    const float2 k = float2(gWidth, gHeight) / float2(max(srcW, 1u), max(srcH, 1u));
    const float2 o = float2(outP) + 0.5;
    const float2 lower = (o - 1.5) / k - 0.5;
    const float2 upper = (o + 1.5) / k - 0.5;

    int2 p0 = int2(ceil(lower));
    int2 p1 = int2(floor(upper));
    p0 = clamp(p0, int2(0, 0), int2((int) srcW - 1, (int) srcH - 1));
    p1 = clamp(p1, int2(0, 0), int2((int) srcW - 1, (int) srcH - 1));

    const int nx = clamp(p1.x - p0.x + 1, 1, 12);
    const int ny = clamp(p1.y - p0.y + 1, 1, 12);
    const float uBase = k.x * ((float) p0.x + 0.5) - o.x;
    const float vBase = k.y * ((float) p0.y + 0.5) - o.y;

    float wx[12], wy[12];
    float sumWx = 0.0, sumWy = 0.0;
    [loop] for (int i = 0; i < nx; ++i)
    {
        wx[i] = DirectMagicKernel(uBase + k.x * (float) i);
        sumWx += wx[i];
    }
    [loop] for (int j = 0; j < ny; ++j)
    {
        wy[j] = DirectMagicKernel(vBase + k.y * (float) j);
        sumWy += wy[j];
    }

    float3 acc = 0.0;
    [loop] for (int j2 = 0; j2 < ny; ++j2)
    {
        [loop] for (int i2 = 0; i2 < nx; ++i2)
            acc += gModel.Load(int3(p0 + int2(i2, j2), 0)).rgb * (wx[i2] * wy[j2]);
    }
    return acc / max(sumWx * sumWy, 1e-12);
}

float3 DirectAreaPixel(int2 outP)
{
    uint srcW, srcH;
    gModel.GetDimensions(srcW, srcH);
    const float x0 = ((float) outP.x * (float) srcW) / (float) gWidth;
    const float x1 = ((float) (outP.x + 1) * (float) srcW) / (float) gWidth;
    const float y0 = ((float) outP.y * (float) srcH) / (float) gHeight;
    const float y1 = ((float) (outP.y + 1) * (float) srcH) / (float) gHeight;
    const float area = (x1 - x0) * (y1 - y0);

    const int i0 = (int) floor(x0);
    const int i1 = (int) ceil(x1) - 1;
    const int j0 = (int) floor(y0);
    const int j1 = (int) ceil(y1) - 1;
    float3 acc = 0.0;

    [loop] for (int j = j0; j <= j1; ++j)
    {
        const int jj = clamp(j, 0, (int) srcH - 1);
        const float aY = max(y0, (float) j);
        const float bY = min(y1, (float) j + 1.0);
        const float wy = max(bY - aY, 0.0);
        [loop] for (int i = i0; i <= i1; ++i)
        {
            const int ii = clamp(i, 0, (int) srcW - 1);
            const float aX = max(x0, (float) i);
            const float bX = min(x1, (float) i + 1.0);
            acc += gModel.Load(int3(ii, jj, 0)).rgb * (max(bX - aX, 0.0) * wy);
        }
    }
    return acc / max(area, 1e-12);
}

float3 DirectSpatialPixel(int2 outP)
{
    outP = clamp(outP, int2(0, 0), int2((int) gWidth - 1, (int) gHeight - 1));
    if (gDirectResolveUpscaler == 8u)
        return DirectMagicPixel(outP);
    if (gDirectResolveUpscaler == 7u)
        return DirectAreaPixel(outP);

    // Selectors 0 and 9 are exactly bilinear on this branch. FSR1's projCentre/squaredRadius
    // constants are never initialized, so its EASU shader takes its bilinear branch everywhere.
    const float2 sampleUv = (float2(outP) + 0.5) / float2(gWidth, gHeight);
    return gModel.SampleLevel(gLinear, sampleUv, 0).rgb;
}

float4 DirectSpatialSample(float2 outUv)
{
    if (gCompareMode != 1u)
    {
        const int2 p = int2(clamp(floor(outUv * float2(gWidth, gHeight)), 0.0,
                                  float2(gWidth - 1u, gHeight - 1u)));
        return float4(DirectSpatialPixel(p), 1.0);
    }

    // Side-by-side normally samples a materialized NR100 linearly. Reproduce that second sampling
    // stage by evaluating the four surrounding output pixels and blending them here.
    const float2 pos = outUv * float2(gWidth, gHeight) - 0.5;
    const int2 base = int2(floor(pos));
    const float2 f = frac(pos);
    const float3 c00 = DirectSpatialPixel(base);
    const float3 c10 = DirectSpatialPixel(base + int2(1, 0));
    const float3 c01 = DirectSpatialPixel(base + int2(0, 1));
    const float3 c11 = DirectSpatialPixel(base + int2(1, 1));
    return float4(lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y), 1.0);
}

// Final P100-guided detail experiments. They intentionally live in the existing resolve dispatch.
// Direct NR identifies itself with DirectResolveFlags bit 0 because its internal transfer can be 0 or 7;
// Upscaled NR residual is transfer 6.
float3 ExperimentDecodeProxySample(float3 raw)
{
    return gPassthrough != 0 ? raw : SrgbToLinear(raw);
}

float3 DecodeGuidedResidualCarrier(float3 carrier)
{
    const float3 d = clamp(2.0 * SanitizeFinite3(carrier, 0.5) - 1.0, -0.999, 0.999);
    return d / (1.0 - abs(d));
}

// Joint-bilateral P50 residual upsampling guided by the untouched native P100 proxy.
// All weights are positive. This deliberately avoids negative-lobe reconstruction kernels:
// the operation can redistribute/attenuate NR50-P50 across a native edge, but cannot invent ringing
// from a signed resampling kernel.
float3 GuidedResidualBilinear(float2 uvq)
{
    // IMPORTANT: capture-derived tuning was measured in the raw FP16 proxy/model texture domain
    // (the sRGB-coded model input/output for linear-HDR mode), so keep the guided estimator there.
    const float3 p = gSource.SampleLevel(gLinear, saturate(uvq), 0).rgb;
    const float3 n = gModel.SampleLevel(gLinear, saturate(uvq), 0).rgb;
    // Internal Transfer 9 receives an already-decoded signed E50 texture from the temporal carrier path.
    return gTransfer == 9u ? n : n - p;
}

float3 P100GuidedResidualAt(float2 uvq, float3 nativeGuide)
{
    uint srcW, srcH;
    gSource.GetDimensions(srcW, srcH);
    if (srcW == 0u || srcH == 0u)
        return 0.0;

    const float3 bilinearResidual = GuidedResidualBilinear(uvq);
#if defined(DLSSNR_SUPERIOR_RESOLVE)
    const int radius = 1;
#else
    const int radius = (int) clamp(gResidualHistoryValidUnused, 1u, 3u);
#endif
    const float rangeSigma = max(abs(gResidualBlendUnused), 1e-5);
    const float spatialSigma = max(abs(gResidualScale), 1e-4);
    const float invRange2 = 1.0 / (rangeSigma * rangeSigma);
    const float invSpatial2 = 1.0 / (spatialSigma * spatialSigma);
    const float gaussianExp2 = 0.7213475204444817; // 0.5 / ln(2)

    // Source texel centres are integer coordinates in this space.
    const float2 sourcePos = uvq * float2(srcW, srcH) - 0.5;
    const int2 base = int2(floor(sourcePos + 0.5));

    float3 weighted = 0.0;
    float weightSum = 0.0;
#if defined(DLSSNR_SUPERIOR_RESOLVE)
    // The normal HDR PSO accepts FP reassociation; retain all nine samples and exact weight formula.
#if defined(DLSSNR_SUPERIOR_NORMAL) && !defined(DLSSNR_SUPERIOR_SDR)
    [unroll] for (int oy = -1; oy <= 1; ++oy)
#else
    [loop] for (int oy = -1; oy <= 1; ++oy)
#endif
#else
    [loop] for (int oy = -3; oy <= 3; ++oy)
#endif
    {
        if (abs(oy) > radius)
            continue;
#if defined(DLSSNR_SUPERIOR_RESOLVE)
#if defined(DLSSNR_SUPERIOR_NORMAL) && !defined(DLSSNR_SUPERIOR_SDR)
        [unroll] for (int ox = -1; ox <= 1; ++ox)
#else
        [loop] for (int ox = -1; ox <= 1; ++ox)
#endif
#else
        [loop] for (int ox = -3; ox <= 3; ++ox)
#endif
        {
            if (abs(ox) > radius)
                continue;

            const int2 p = clamp(base + int2(ox, oy), int2(0, 0), int2((int) srcW - 1, (int) srcH - 1));
            const float3 proxyCandidate = gSource.Load(int3(p, 0)).rgb;
            const float3 modelCandidate = gModel.Load(int3(p, 0)).rgb;
            const float3 residual =
                gTransfer == 9u ? modelCandidate : modelCandidate - proxyCandidate;

            // Match the offline capture fit exactly: mean squared RGB distance in raw proxy space.
            const float3 colourDelta = proxyCandidate - nativeGuide;
            const float rangeDistance2 = dot(colourDelta, colourDelta) * (1.0 / 3.0);

            const float2 spatialDelta = float2(p) - sourcePos;
            const float spatialDistance2 = dot(spatialDelta, spatialDelta);

            const float wRange = exp2(-gaussianExp2 * rangeDistance2 * invRange2);
            const float wSpatial = exp2(-gaussianExp2 * spatialDistance2 * invSpatial2);
            const float w = wRange * wSpatial;
            weighted += residual * w;
            weightSum += w;
        }
    }

    const float3 guided = weightSum > 1e-8 ? weighted / weightSum : bilinearResidual;
    return InterPassGuideBlend(bilinearResidual, guided, saturate(gResidualConfidenceUnused));
}


#if !defined(VK_MODE)
// Faster formulation of the same positive-weight guided reconstruction. The reference
// function above is deliberately kept unchanged for the OFF / A-B test path.
// One exp2 of the sum replaces two exp2 and a multiply; only FP rounding differs.
float3 P100GuidedResidualAtOptimized(float2 uvq, float3 nativeGuide)
{
    uint srcW, srcH;
    gSource.GetDimensions(srcW, srcH);
    if (srcW == 0u || srcH == 0u)
        return 0.0;

    const float3 bilinearResidual = GuidedResidualBilinear(uvq);
    const float guideStrength = saturate(gResidualConfidenceUnused);
    if (guideStrength <= 0.0)
        return bilinearResidual;

    const int radius = (int) clamp(gResidualHistoryValidUnused, 1u, 3u);
    const float rangeSigma = max(abs(gResidualBlendUnused), 1e-5);
    const float spatialSigma = max(abs(gResidualScale), 1e-4);
    const float invRange2 = 1.0 / (rangeSigma * rangeSigma);
    const float invSpatial2 = 1.0 / (spatialSigma * spatialSigma);
    const float gaussianExp2 = 0.7213475204444817;
    const float2 sourcePos = uvq * float2(srcW, srcH) - 0.5;
    const int2 base = int2(floor(sourcePos + 0.5));

    float3 weighted = 0.0;
    float weightSum = 0.0;
    [loop] for (int oy = -radius; oy <= radius; ++oy)
    {
        [loop] for (int ox = -radius; ox <= radius; ++ox)
        {
            const int2 p = clamp(base + int2(ox, oy), int2(0, 0), int2((int) srcW - 1, (int) srcH - 1));
            const float3 proxyCandidate = gSource.Load(int3(p, 0)).rgb;
            const float3 modelCandidate = gModel.Load(int3(p, 0)).rgb;
            const float3 residual = gTransfer == 9u ? modelCandidate : modelCandidate - proxyCandidate;
            const float3 colourDelta = proxyCandidate - nativeGuide;
            const float rangeDistance2 = dot(colourDelta, colourDelta) * (1.0 / 3.0);
            const float2 spatialDelta = float2(p) - sourcePos;
            const float spatialDistance2 = dot(spatialDelta, spatialDelta);
            const float w = exp2(-gaussianExp2 *
                                 (rangeDistance2 * invRange2 + spatialDistance2 * invSpatial2));
            weighted += residual * w;
            weightSum += w;
        }
    }
    const float3 guided = weightSum > 1e-8 ? weighted / weightSum : bilinearResidual;
    return InterPassGuideBlend(bilinearResidual, guided, guideStrength);
}
#endif

#if !defined(VK_MODE)
// Apply identical frequency shaping and shadow confidence to a supplied guided edit.
// Bit 5 skips the low field ONLY when low gain equals the high/mid gain exactly.
float3 InterPassShapeEditAt(float3 guidedEditRaw, float2 uvq, float3 nativeGuide)
{
    if (gGuideWidth != 0u)
    {
        const float3 lowEditRaw = gAux2.SampleLevel(gLinear, saturate(uvq), 0).rgb;
        guidedEditRaw = gMvScaleX * guidedEditRaw + (gMvScaleY - gMvScaleX) * lowEditRaw;
    }
    else if ((gDirectResolveFlags & 32u) != 0u)
    {
        guidedEditRaw *= gMvScaleX;
    }

#if !defined(DLSSNR_SUPERIOR_SCALING)
    if (gGuideHeight != 0u)
    {
        const float shadowLowRaw = asfloat(gExposureSourceWidth);
        const float shadowHighRaw = asfloat(gExposureSourceHeight);
        const float shadowFloorRaw = asfloat(gExposurePadding);
        const float shadowLow = isfinite(shadowLowRaw) ? shadowLowRaw : 0.02;
        const float shadowHigh = isfinite(shadowHighRaw) ? shadowHighRaw : 0.08;
        const float shadowFloor = isfinite(shadowFloorRaw) ? shadowFloorRaw : 0.15;
        const float lo = min(shadowLow, shadowHigh);
        const float hi = max(max(shadowLow, shadowHigh), lo + 1e-6);
        const float y = max(dot(nativeGuide, kLuma), 0.0);
        guidedEditRaw *= lerp(saturate(shadowFloor), 1.0, smoothstep(lo, hi, y));
    }
#endif
    return SanitizeFinite3(guidedEditRaw, 0.0);
}

float3 InterPassGuidedEditAt(float2 uvq, float3 nativeGuide)
{
    const float3 guidedEditRaw = (gDirectResolveFlags & 16u) != 0u
        ? P100GuidedResidualAtOptimized(uvq, nativeGuide)
        : P100GuidedResidualAt(uvq, nativeGuide);
    return InterPassShapeEditAt(guidedEditRaw, uvq, nativeGuide);
}

// Keep the v21 implementation separate: folding it into the experimental loop changed
// DXC's FP32 rounding in the standard-PSO fallback. Its isolated bytecode stays identical.
float4 InterPassFastGuided(int2 p)
{
    // Reuse the immutable original Area proxy B. Approximate
    // Area(P100 + shaped Guided(N-B, P100)) by B + shaped Guided(N-B, P100 at centre).
    // Do not feed N-previousCorrected back as the residual; that changes the multi-pass contract.
    const float4 base = gSource.Load(int3(p, 0));
    const float3 centreResidual = gModel.Load(int3(p, 0)).rgb - base.rgb;
    const float2 uvq = (float2(p) + 0.5) / float2(gWidth, gHeight);
    const float3 nativeGuide = gOriginal.SampleLevel(gLinear, uvq, 0).rgb;
    const float rangeSigma = max(abs(gResidualBlendUnused), 1e-5);
    const float spatialSigma = max(abs(gResidualScale), 1e-4);
    const float rangeFactor = (0.7213475204444817 / 3.0) / (rangeSigma * rangeSigma);
    const float spatialFactor = 0.7213475204444817 / (spatialSigma * spatialSigma);
    float3 weighted = 0.0;
    float weightSum = 0.0;
    [unroll] for (int oy = -1; oy <= 1; ++oy)
    {
        [unroll] for (int ox = -1; ox <= 1; ++ox)
        {
            const int2 q = clamp(p + int2(ox, oy), int2(0, 0), int2(gWidth - 1, gHeight - 1));
            const float3 candidate = gSource.Load(int3(q, 0)).rgb;
            const float3 residual = gModel.Load(int3(q, 0)).rgb - candidate;
            const float3 delta = candidate - nativeGuide;
            // Use clamped coordinates just like the exact path at frame edges.
            const float2 spatialDelta = float2(q - p);
            const float w = exp2(-(dot(delta, delta) * rangeFactor +
                                   dot(spatialDelta, spatialDelta) * spatialFactor));
            weighted += residual * w;
            weightSum += w;
        }
    }
    const float3 guided = weightSum > 1e-8 ? weighted / weightSum : centreResidual;
    const float3 edit = InterPassShapeEditAt(
        InterPassGuideBlend(centreResidual, guided, saturate(gResidualConfidenceUnused)), uvq, nativeGuide);
    uint nativeW, nativeH;
    gOriginal.GetDimensions(nativeW, nativeH);
    const int2 nativeP = min(int2(uvq * float2(nativeW, nativeH)), int2(nativeW - 1, nativeH - 1));
    const float alpha = gOriginal.Load(int3(nativeP, 0)).a;
    return float4(saturate(SanitizeFinite3(base.rgb + edit, 0.5)), alpha);
}

float4 InterPassFastCross5(int2 p)
{
    // Reuse the immutable original Area proxy B. Approximate
    // Area(P100 + shaped Guided(N-B, P100)) by B + shaped Guided(N-B, P100 at centre).
    // Do not feed N-previousCorrected back as the residual; that changes the multi-pass contract.
    const float4 base = gSource.Load(int3(p, 0));
    const float3 centreResidual = gModel.Load(int3(p, 0)).rgb - base.rgb;
    const float2 uvq = (float2(p) + 0.5) / float2(gWidth, gHeight);
    const float3 nativeGuide = gOriginal.SampleLevel(gLinear, uvq, 0).rgb;
    const float rangeSigma = max(abs(gResidualBlendUnused), 1e-5);
    const float spatialSigma = max(abs(gResidualScale), 1e-4);
    const float rangeFactor = (0.7213475204444817 / 3.0) / (rangeSigma * rangeSigma);
    const float spatialFactor = 0.7213475204444817 / (spatialSigma * spatialSigma);
    float3 weighted = 0.0;
    float weightSum = 0.0;
    [unroll] for (int oy = -1; oy <= 1; ++oy)
    {
        [unroll] for (int ox = -1; ox <= 1; ++ox)
        {
            // Specialised Cross5 PSO eliminates corner loads and their weights.
            if (ox != 0 && oy != 0)
                continue;
            const int2 q = clamp(p + int2(ox, oy), int2(0, 0), int2(gWidth - 1, gHeight - 1));
            const float3 candidate = gSource.Load(int3(q, 0)).rgb;
            const float3 residual = gModel.Load(int3(q, 0)).rgb - candidate;
            const float3 delta = candidate - nativeGuide;
            // Use clamped coordinates just like the exact path at frame edges.
            const float2 spatialDelta = float2(q - p);
            const float exponent = dot(delta, delta) * rangeFactor +
                                   dot(spatialDelta, spatialDelta) * spatialFactor;
            const float w = exp2(-exponent);
            weighted += residual * w;
            weightSum += w;
        }
    }
    const float3 guided = weightSum > 1e-8 ? weighted / weightSum : centreResidual;
    const float3 edit = InterPassShapeEditAt(
        InterPassGuideBlend(centreResidual, guided, saturate(gResidualConfidenceUnused)), uvq, nativeGuide);
    uint nativeW, nativeH;
    gOriginal.GetDimensions(nativeW, nativeH);
    const int2 nativeP = min(int2(uvq * float2(nativeW, nativeH)), int2(nativeW - 1, nativeH - 1));
    const float alpha = gOriginal.Load(int3(nativeP, 0)).a;
    return float4(saturate(SanitizeFinite3(base.rgb + edit, 0.5)), alpha);
}

float4 InterPassCorrectedP100Load(int2 p)
{
    uint nativeW, nativeH;
    gOriginal.GetDimensions(nativeW, nativeH);
    if (nativeW == 0u || nativeH == 0u)
        return float4(0.5, 0.5, 0.5, 1.0);

    p = clamp(p, int2(0, 0), int2((int) nativeW - 1, (int) nativeH - 1));
    const float4 native = gOriginal.Load(int3(p, 0));
    const float2 uvq = (float2(p) + 0.5) / float2(nativeW, nativeH);
    const float3 corrected =
        SanitizeFinite3(native.rgb + InterPassGuidedEditAt(uvq, native.rgb), native.rgb);
    return float4(corrected, native.a);
}

float4 InterPassCorrectedP100Bilinear(float2 uvq)
{
    uint nativeW, nativeH;
    gOriginal.GetDimensions(nativeW, nativeH);
    const float2 pos = saturate(uvq) * float2(nativeW, nativeH) - 0.5;
    const int2 p0 = int2(floor(pos));
    const float2 f = frac(pos);
    const float4 a = lerp(InterPassCorrectedP100Load(p0),
                          InterPassCorrectedP100Load(p0 + int2(1, 0)), f.x);
    const float4 b = lerp(InterPassCorrectedP100Load(p0 + int2(0, 1)),
                          InterPassCorrectedP100Load(p0 + int2(1, 1)), f.x);
    return lerp(a, b, f.y);
}


// Scale-independent inter-pass guided reconstruction for Area downsampling.
//
// The geometry ratio (working/native) is computed and cached on the CPU when
// the ACTUAL texture dimensions change. No shader reload, GPU lookup table,
// resource upload, or per-frame geometry preparation is required.
// Pixel positions depend on the current output coordinate and still must be
// evaluated by the GPU; all colour-dependent bilateral weights remain per frame.
//
// For radius=1 the bilinear 2x2 residual footprint is ALWAYS contained in
// the 3x3 guided footprint around round(sourcePos), at arbitrary fractional
// scale and along frame edges. One set of source/model Loads therefore serves
// both the guided and bilinear estimators without changing their sample set.
// Off uses InterPassCorrectedP100Load as the original A/B reference.
// v10 specialization is compile-time: disabled options have zero LDS and
// no conditionals/register lifetime. v9 variants remain available as A/B control.
float4 InterPassCorrectedP100LoadDynamic(int2 p)
{
    uint nativeW, nativeH;
    gOriginal.GetDimensions(nativeW, nativeH);
    if (nativeW == 0u || nativeH == 0u)
        return float4(0.5, 0.5, 0.5, 1.0);

    p = clamp(p, int2(0, 0), int2((int) nativeW - 1, (int) nativeH - 1));
    const float4 native = gOriginal.Load(int3(p, 0));
    const float2 uvq = (float2(p) + 0.5) / float2(nativeW, nativeH);
    const float2 geometryScale = float2(asfloat(gResidualMotionBaseXUnused),
                                        asfloat(gResidualMotionBaseYUnused));
    const float2 sourcePos = (float2(p) + 0.5) * geometryScale - 0.5;
    const int2 roundedBase = int2(floor(sourcePos + 0.5));
    const int2 bilinearBase = int2(floor(sourcePos));
    const float2 fracPos = frac(sourcePos);

    uint srcW, srcH;
    gSource.GetDimensions(srcW, srcH);
    const float rangeSigma = max(abs(gResidualBlendUnused), 1e-5);
    const float spatialSigma = max(abs(gResidualScale), 1e-4);
    const float rangeFactor = (0.7213475204444817 / 3.0) / (rangeSigma * rangeSigma);
    const float spatialFactor = 0.7213475204444817 / (spatialSigma * spatialSigma);
    float3 weighted = 0.0;
    float weightSum = 0.0;
    float3 bilinear = 0.0;
#if defined(DLSSNR_INTERPASS_WEIGHTS)
    const bool movedX = roundedBase.x != bilinearBase.x;
    const bool movedY = roundedBase.y != bilinearBase.y;
    const float3 weightsX = movedX ? float3(1.0 - fracPos.x, fracPos.x, 0.0)
                                   : float3(0.0, 1.0 - fracPos.x, fracPos.x);
    const float3 weightsY = movedY ? float3(1.0 - fracPos.y, fracPos.y, 0.0)
                                   : float3(0.0, 1.0 - fracPos.y, fracPos.y);
#endif
    [unroll] for (int oy = -1; oy <= 1; ++oy)
    {
        [unroll] for (int ox = -1; ox <= 1; ++ox)
        {
            const int2 logical = roundedBase + int2(ox, oy);
            const int2 coord = clamp(logical, int2(0, 0),
                                     int2((int) srcW - 1, (int) srcH - 1));
            const float3 proxyCandidate = gSource.Load(int3(coord, 0)).rgb;
            const float3 residual = gModel.Load(int3(coord, 0)).rgb - proxyCandidate;

            // Use UNCLAMPED logical texel positions for bilinear membership;
            // otherwise a duplicated clamped edge texel would be added twice.
            // Samples themselves are clamped in exactly the same way as the
            // texture's linear CLAMP sampler.
#if !defined(DLSSNR_INTERPASS_WEIGHTS)
            const float wx = logical.x == bilinearBase.x ? 1.0 - fracPos.x :
                             (logical.x == bilinearBase.x + 1 ? fracPos.x : 0.0);
            const float wy = logical.y == bilinearBase.y ? 1.0 - fracPos.y :
                             (logical.y == bilinearBase.y + 1 ? fracPos.y : 0.0);
#endif
#if defined(DLSSNR_INTERPASS_WEIGHTS)
            bilinear += residual * (weightsX[ox + 1] * weightsY[oy + 1]);
#else
            bilinear += residual * (wx * wy);
#endif

            const float3 delta = proxyCandidate - native.rgb;
            const float2 deltaSpatial = float2(coord) - sourcePos;
            const float w = exp2(-(dot(delta, delta) * rangeFactor +
                                   dot(deltaSpatial, deltaSpatial) * spatialFactor));
            weighted += residual * w;
            weightSum += w;
        }
    }
    const float3 guided = weightSum > 1e-8 ? weighted / weightSum : bilinear;
    const float3 editRaw = InterPassGuideBlend(bilinear, guided, saturate(gResidualConfidenceUnused));
    const float3 edit = InterPassShapeEditAt(editRaw, uvq, native.rgb);
    return float4(SanitizeFinite3(native.rgb + edit, native.rgb), native.a);
}


float4 InterPassCorrectedClassicSharedStencil(int2 nativeP)
{
    const float4 native = gOriginal.Load(int3(nativeP, 0));
    const float2 uvq = (float2(nativeP) + 0.5) / float2(gWidth, gHeight);
    const uint srcW = gDirectDetailMode;           // set only for mode 27 + v6
    const uint srcH = gDirectResolveUpscaler;      // set only for mode 27 + v6
    const float2 sourcePos = uvq * float2(srcW, srcH) - 0.5;
    const int2 sourceBase = int2(floor(sourcePos + 0.5));
    const int2 bilinearBase = int2(floor(sourcePos));
    const float2 fracPos = frac(sourcePos);

    // Bilinear candidates are inside the same 3x3 stencil for every
    // fractional source position. Replacing per-tap coordinate comparisons
    // with two separable three-element weight sets reduces shader ALU.
    // A >= 0.5 fractional coordinate shifts the rounded stencil right/down.
    const bool movedX = sourceBase.x != bilinearBase.x;
    const bool movedY = sourceBase.y != bilinearBase.y;
    const float3 weightsX = movedX
        ? float3(1.0 - fracPos.x, fracPos.x, 0.0)
        : float3(0.0, 1.0 - fracPos.x, fracPos.x);
    const float3 weightsY = movedY
        ? float3(1.0 - fracPos.y, fracPos.y, 0.0)
        : float3(0.0, 1.0 - fracPos.y, fracPos.y);

    const float rangeSigma = max(abs(gResidualBlendUnused), 1e-5);
    const float spatialSigma = max(abs(gResidualScale), 1e-4);
    const float rangeFactor = (0.7213475204444817 / 3.0) / (rangeSigma * rangeSigma);
    const float spatialFactor = 0.7213475204444817 / (spatialSigma * spatialSigma);

    float3 weighted = 0.0, bilinear = 0.0;
    float weightSum = 0.0;
    [unroll] for (int oy = -1; oy <= 1; ++oy)
    {
        const int sampleY = clamp(sourceBase.y + oy, 0, (int)srcH - 1);
        const float dy = (float)sampleY - sourcePos.y;
        const float dy2 = dy * dy;
        const float wy = weightsY[oy + 1];
        [unroll] for (int ox = -1; ox <= 1; ++ox)
        {
            const int sampleX = clamp(sourceBase.x + ox, 0, (int)srcW - 1);
            const int2 sampleP = int2(sampleX, sampleY);
            const float3 proxyCandidate = gSource.Load(int3(sampleP, 0)).rgb;
            const float3 residual = gModel.Load(int3(sampleP, 0)).rgb - proxyCandidate;
            bilinear += residual * (weightsX[ox + 1] * wy);

            const float3 colourDelta = proxyCandidate - native.rgb;
            const float dx = (float)sampleX - sourcePos.x;
            const float w = exp2(-(dot(colourDelta, colourDelta) * rangeFactor +
                                   (dx * dx + dy2) * spatialFactor));
            weighted += residual * w;
            weightSum += w;
        }
    }

    const float3 guided = weightSum > 1e-8 ? weighted / weightSum : bilinear;
    const float3 editRaw = InterPassGuideBlend(bilinear, guided, saturate(gResidualConfidenceUnused));
    const float3 shapedEdit = InterPassShapeEditAt(editRaw, uvq, native.rgb);
    const float3 corrected = SanitizeFinite3(native.rgb + shapedEdit, native.rgb);
    return float4(corrected, native.a);
}


 // Exact 2:1 P100->P50 Area case. All four P100 centres share the same
 // nearest P50 source texel and thus exactly the same (2*r+1)^2 source
 // candidates. Load that neighbourhood ONCE, then evaluate the four
 // independent P100 guides using the reference weights/normalization.
 // No P100 intermediate and no extra dispatch or GPU scratch allocation.
float4 InterPassCorrectedAreaP50Optimized(int2 outP, uint2 nativeSize)
{
    const float2 nativeSizeF = float2(nativeSize);
    const int2 srcBase = outP;
    float4 originals[4];
    float3 bilinear[4];
    float3 weighted[4];
    float weightSum[4];
    float2 uvList[4];
    const float guideStrength = saturate(gResidualConfidenceUnused);

    [unroll] for (uint k = 0u; k < 4u; ++k)
    {
        const int2 nativeP = outP * 2 + int2(k & 1u, k >> 1u);
        const float2 uvq = (float2(nativeP) + 0.5) / nativeSizeF;
        uvList[k] = uvq;
        originals[k] = gOriginal.Load(int3(nativeP, 0));
        // Identical bilinear fallback to GuidedResidualBilinear for Transfer=0.
        bilinear[k] = gModel.SampleLevel(gLinear, uvq, 0).rgb -
                      gSource.SampleLevel(gLinear, uvq, 0).rgb;
        weighted[k] = 0.0;
        weightSum[k] = 0.0;
    }

    if (guideStrength > 0.0)
    {
        const int radius = (int) clamp(gResidualHistoryValidUnused, 1u, 3u);
        const float rangeSigma = max(abs(gResidualBlendUnused), 1e-5);
        const float spatialSigma = max(abs(gResidualScale), 1e-4);
        const float invRange2 = 1.0 / (rangeSigma * rangeSigma);
        const float invSpatial2 = 1.0 / (spatialSigma * spatialSigma);
        const float gaussianExp2 = 0.7213475204444817;

        [loop] for (int oy = -radius; oy <= radius; ++oy)
        {
            [loop] for (int ox = -radius; ox <= radius; ++ox)
            {
                const int2 sampleP = clamp(srcBase + int2(ox, oy), int2(0, 0),
                                           int2((int) gWidth - 1, (int) gHeight - 1));
                const float3 proxyCandidate = gSource.Load(int3(sampleP, 0)).rgb;
                const float3 residual = gModel.Load(int3(sampleP, 0)).rgb - proxyCandidate;
                [unroll] for (uint k = 0u; k < 4u; ++k)
                {
                    // At native=2*working, P100 centres project exactly to
                    // sourcePos = outP + { -0.25, +0.25 } on each axis.
                    const float2 sourcePos = float2(outP) +
                        float2((k & 1u) != 0u ? 0.25 : -0.25,
                               (k & 2u) != 0u ? 0.25 : -0.25);
                    const float3 colourDelta = proxyCandidate - originals[k].rgb;
                    const float rangeDistance2 = dot(colourDelta, colourDelta) * (1.0 / 3.0);
                    const float2 spatialDelta = float2(sampleP) - sourcePos;
                    const float spatialDistance2 = dot(spatialDelta, spatialDelta);
                    const float w = exp2(-gaussianExp2 *
                                         (rangeDistance2 * invRange2 + spatialDistance2 * invSpatial2));
                    weighted[k] += residual * w;
                    weightSum[k] += w;
                }
            }
        }
    }

    float3 correctedSum = 0.0;
    [unroll] for (uint k = 0u; k < 4u; ++k)
    {
        const float3 guided = weightSum[k] > 1e-8 ? weighted[k] / weightSum[k] : bilinear[k];
        const float3 edit = InterPassShapeEditAt(InterPassGuideBlend(bilinear[k], guided, guideStrength),
                                                uvList[k], originals[k].rgb);
        correctedSum += SanitizeFinite3(originals[k].rgb + edit, originals[k].rgb);
    }
    // The reference Area branch explicitly restores centre-sample alpha.
    return float4(correctedSum * 0.25, originals[3].a);
}


 // P50 + Area + radius=1 specialization: the guided stencil is a 3x3 P50
 // neighbourhood, which ALSO contains every texel needed to bilinearly
 // reconstruct the residual at the four P100 positions. Accumulate the
 // bilinear fallbacks from these already-fetched signed residuals rather
 // than issuing eight redundant bilinear texture samples.
 //
 // This path is selectable for A/B with v1. Its weights remain the same
 // per-P100 positive bilateral weights, including at clamped image edges.
float4 InterPassCorrectedAreaP50SharedBilinear(int2 outP, uint2 nativeSize)
{
    const float2 nativeSizeF = float2(nativeSize);
    float4 originals[4];
    float2 uvList[4];
    float3 bilinear[4];
    float3 weighted[4];
    float weightSum[4];

    [unroll] for (uint k = 0u; k < 4u; ++k)
    {
        const int2 nativeP = outP * 2 + int2(k & 1u, k >> 1u);
        uvList[k] = (float2(nativeP) + 0.5) / nativeSizeF;
        originals[k] = gOriginal.Load(int3(nativeP, 0));
        bilinear[k] = 0.0;
        weighted[k] = 0.0;
        weightSum[k] = 0.0;
    }

    const float rangeSigma = max(abs(gResidualBlendUnused), 1e-5);
    const float spatialSigma = max(abs(gResidualScale), 1e-4);
    const float rangeFactor = (0.7213475204444817 / 3.0) / (rangeSigma * rangeSigma);
    const float spatialFactor = 0.7213475204444817 / (spatialSigma * spatialSigma);
    const float2 baseP = float2(outP);
    const float2 p00 = baseP + float2(-0.25, -0.25);
    const float2 p11 = baseP + float2(0.25, 0.25);

    // The 3x3 static bound lets DXC unroll all nine stencil steps. Even
    // when clamping duplicates a sample at the frame edge, the position
    // used for the spatial Gaussian is its CLAMPED source texel centre.
    [unroll] for (int oy = -1; oy <= 1; ++oy)
    {
        const float wy0 = oy == -1 ? 0.25 : (oy == 0 ? 0.75 : 0.0);
        const float wy1 = oy == 1 ? 0.25 : (oy == 0 ? 0.75 : 0.0);
        [unroll] for (int ox = -1; ox <= 1; ++ox)
        {
            const float wx0 = ox == -1 ? 0.25 : (ox == 0 ? 0.75 : 0.0);
            const float wx1 = ox == 1 ? 0.25 : (ox == 0 ? 0.75 : 0.0);
            const int2 sampleP = clamp(outP + int2(ox, oy), int2(0, 0),
                                       int2((int) gWidth - 1, (int) gHeight - 1));
            const float3 proxyCandidate = gSource.Load(int3(sampleP, 0)).rgb;
            const float3 residual = gModel.Load(int3(sampleP, 0)).rgb - proxyCandidate;

            // Hardware bilinear samples at +/-0.25 P50 texel offsets are
            // exactly the positive 0.25/0.75 separable weights accumulated here.
            bilinear[0] += residual * (wx0 * wy0);
            bilinear[1] += residual * (wx1 * wy0);
            bilinear[2] += residual * (wx0 * wy1);
            bilinear[3] += residual * (wx1 * wy1);

            const float dx0 = (float) sampleP.x - p00.x;
            const float dx1 = (float) sampleP.x - p11.x;
            const float dy0 = (float) sampleP.y - p00.y;
            const float dy1 = (float) sampleP.y - p11.y;
            const float dx0sq = dx0 * dx0;
            const float dx1sq = dx1 * dx1;
            const float dy0sq = dy0 * dy0;
            const float dy1sq = dy1 * dy1;
            const float4 spatialPenalties = float4(dx0sq + dy0sq, dx1sq + dy0sq,
                                                   dx0sq + dy1sq, dx1sq + dy1sq) * spatialFactor;
            const float3 delta0 = proxyCandidate - originals[0].rgb;
            const float3 delta1 = proxyCandidate - originals[1].rgb;
            const float3 delta2 = proxyCandidate - originals[2].rgb;
            const float3 delta3 = proxyCandidate - originals[3].rgb;
            const float4 rangePenalties = float4(dot(delta0, delta0), dot(delta1, delta1),
                                                 dot(delta2, delta2), dot(delta3, delta3)) * rangeFactor;
            const float4 w = exp2(-(rangePenalties + spatialPenalties));

            weighted[0] += residual * w.x;
            weighted[1] += residual * w.y;
            weighted[2] += residual * w.z;
            weighted[3] += residual * w.w;
            weightSum[0] += w.x;
            weightSum[1] += w.y;
            weightSum[2] += w.z;
            weightSum[3] += w.w;
        }
    }

    const float guideStrength = saturate(gResidualConfidenceUnused);
    float3 correctedSum = 0.0;
    [unroll] for (uint k = 0u; k < 4u; ++k)
    {
        const float3 guided = weightSum[k] > 1e-8 ? weighted[k] / weightSum[k] : bilinear[k];
        const float3 edit = InterPassShapeEditAt(InterPassGuideBlend(bilinear[k], guided, guideStrength),
                                                 uvList[k], originals[k].rgb);
        correctedSum += SanitizeFinite3(originals[k].rgb + edit, originals[k].rgb);
    }
    return float4(correctedSum * 0.25, originals[3].a);
}

#endif

float3 ExperimentFinalModelAt(float2 uvq)
{
    uvq = saturate(uvq);
    const float3 raw = gTransfer == 7u ? DirectSpatialSample(uvq).rgb
                                        : gModel.SampleLevel(gLinear, uvq, 0).rgb;
    return ExperimentDecodeProxySample(raw);
}

float3 ExperimentBaselineReferenceAt(float2 uvq)
{
    uvq = saturate(uvq);
    // Source is the ordinary composition baseline: native P100 proxy for Direct NR and reconstructed
    // P50->P100 for Upscaled NR residual. Using it here makes strength zero algebraically identical
    // to each mode's normal edit.
    return ExperimentDecodeProxySample(gSource.SampleLevel(gLinear, uvq, 0).rgb);
}

float3 ExperimentGainReferenceAt(float2 uvq)
{
    uvq = saturate(uvq);
    // Only Direct structure transfer needs a second reference: Aux2 is the selected reconstruction
    // of the reduced image shown to NR. Upscaled residual already has that reconstruction in Source.
    const bool directStructure =
        (gDirectResolveFlags & 1u) != 0u && gResidualMotionBaseXUnused != 0u;
    const float3 raw = directStructure ? gAux2.SampleLevel(gLinear, uvq, 0).rgb
                                       : gSource.SampleLevel(gLinear, uvq, 0).rgb;
    return ExperimentDecodeProxySample(raw);
}

float3 ExperimentNativeProxyAt(float2 uvq, float normScale)
{
    uvq = saturate(uvq);

    // Direct NR already carries untouched native-resolution P100 in Source.
    if ((gDirectResolveFlags & 1u) != 0u)
        return ExperimentBaselineReferenceAt(uvq);

    // Upscaled NR residual has reconstructed P50 in Source, so derive true P100 geometry from Original
    // using the same model-domain mapping as the ordinary fullProxy path.
    const float3 native = gOriginal.SampleLevel(gLinear, uvq, 0).rgb / max(normScale, 1e-6);
    if (gPassthrough != 0)
        return saturate(native);
    if (gReversibleMode == 0u)
        return saturate(SoftKnee(native));
    return gReversibleMode >= 3u ? HybridEncode(native) : NeutwoEncode(native);
}

void ExperimentCrossBlur(float2 uvq, float radius, float normScale,
                         float3 modelCenter, float3 baselineCenter, float3 gainCenter, float3 nativeCenter,
                         out float3 modelBlur, out float3 baselineBlur,
                         out float3 gainBlur, out float3 nativeBlur,
                         out float3 nativeMin, out float3 nativeMax)
{
    const float2 texel = 1.0 / float2(max(gWidth, 1u), max(gHeight, 1u));
    const float2 dx = float2(texel.x * radius, 0.0);
    const float2 dy = float2(0.0, texel.y * radius);

    // Four neighbours plus the already-known centre. The same native P100 taps also provide the
    // anti-ringing envelope, so the envelope safeguard adds no texture reads.
    const float3 mL = ExperimentFinalModelAt(uvq - dx);
    const float3 mR = ExperimentFinalModelAt(uvq + dx);
    const float3 mU = ExperimentFinalModelAt(uvq - dy);
    const float3 mD = ExperimentFinalModelAt(uvq + dy);
    modelBlur = (4.0 * modelCenter + mL + mR + mU + mD) * 0.125;

    const float3 bL = ExperimentBaselineReferenceAt(uvq - dx);
    const float3 bR = ExperimentBaselineReferenceAt(uvq + dx);
    const float3 bU = ExperimentBaselineReferenceAt(uvq - dy);
    const float3 bD = ExperimentBaselineReferenceAt(uvq + dy);
    baselineBlur = (4.0 * baselineCenter + bL + bR + bU + bD) * 0.125;

    const bool direct = (gDirectResolveFlags & 1u) != 0u;
    const bool separateGainReference = direct && gResidualMotionBaseXUnused != 0u;
    if (separateGainReference)
    {
        gainBlur = (4.0 * gainCenter +
                    ExperimentGainReferenceAt(uvq - dx) + ExperimentGainReferenceAt(uvq + dx) +
                    ExperimentGainReferenceAt(uvq - dy) + ExperimentGainReferenceAt(uvq + dy)) * 0.125;
    }
    else
    {
        gainBlur = baselineBlur;
    }

    float3 nL, nR, nU, nD;
    if (direct)
    {
        // Direct Source is already the untouched P100 proxy, so baseline and native geometry are identical.
        nL = bL; nR = bR; nU = bU; nD = bD;
    }
    else
    {
        nL = ExperimentNativeProxyAt(uvq - dx, normScale);
        nR = ExperimentNativeProxyAt(uvq + dx, normScale);
        nU = ExperimentNativeProxyAt(uvq - dy, normScale);
        nD = ExperimentNativeProxyAt(uvq + dy, normScale);
    }

    nativeBlur = (4.0 * nativeCenter + nL + nR + nU + nD) * 0.125;
    nativeMin = min(nativeCenter, min(min(nL, nR), min(nU, nD)));
    nativeMax = max(nativeCenter, max(max(nL, nR), max(nU, nD)));
}

float ExperimentStructureGain(float3 referenceBand, float3 modelBand, float3 nativeBand,
                              bool polarityGuard, float maxGain, float confidenceThreshold,
                              out float confidence)
{
    const float r = dot(referenceBand, kLuma);
    const float m = dot(modelBand, kLuma);
    const float n = dot(nativeBand, kLuma);

    // Preserve the old 0.002..0.020 confidence shape at the default threshold while making the
    // upper threshold tunable. Crucially, confidence now controls the TRANSFER BLEND itself; low
    // confidence therefore falls back to ordinary NR rather than replacing the band with zero.
    const float high = max(abs(confidenceThreshold), 1e-6);
    const float low = max(high * 0.10, 1e-7);
    confidence = smoothstep(low, high, abs(r));

    if (!(r * m > 0.0))
    {
        confidence = 0.0;
        return 1.0;
    }

    // The native P100 band may be the opposite lobe of a thin-line kernel. Do not apply the P50/NR
    // gain to that lobe: doing so is a direct source of bright/dark ringing around narrow geometry.
    if (polarityGuard && !(r * n > 0.0))
    {
        confidence = 0.0;
        return 1.0;
    }

    const float ratio = abs(m) / max(abs(r), 1e-4);
    const float gainLimit = max(abs(maxGain), 1e-4);
    return min(max(ratio, 0.25), gainLimit);
}

float ExperimentBandEnergy(float3 band)
{
    // Luma catches the dominant drawn/sharpened-edge failure; a small RGB-energy term also catches
    // isoluminant coloured structure without letting chroma noise dominate the limiter.
    return max(abs(dot(band, kLuma)), 0.25 * length(band));
}

float3 ExperimentLimitedBand(float3 conventionalEdit, float3 nativeBand, float limiterStrength,
                             float maxEdgeGain, out float limiterActivity)
{
    limiterActivity = 0.0;

    // Cap the FINAL band, not the edit. This is the key difference from the old limiter:
    // finalBand = native P100 geometry + the ordinary NR edit.
    const float3 finalBand = nativeBand + conventionalEdit;
    const float nativeEnergy = ExperimentBandEnergy(nativeBand);
    const float finalEnergy = ExperimentBandEnergy(finalBand);
    const float gainLimit = abs(maxEdgeGain);
    const float allowedEnergy = nativeEnergy * gainLimit;

    // At gain >= 1, any NR reduction is untouched. Near a flat native region, the floor avoids
    // unstable divide-by-zero while still suppressing newly invented edge energy strongly.
    if (!(finalEnergy > allowedEnergy))
        return conventionalEdit;

    const float edgeFloor = 0.002;
    const float capScale = min(1.0, (allowedEnergy + edgeFloor) / (finalEnergy + edgeFloor));
    const float3 cappedEdit = finalBand * capScale - nativeBand;

    limiterActivity = saturate(abs(limiterStrength) * (1.0 - capScale));
    return lerp(conventionalEdit, cappedEdit, limiterStrength);
}

float3 ExperimentResolveBand(float3 conventionalEdit, float3 gainReferenceBand, float3 modelBand,
                             float3 nativeBand, bool limitBand, bool transferBand,
                             float limiterStrength, float maxEdgeGain, float structureStrength,
                             bool polarityGuard, float structureMaxGain, float confidenceThreshold,
                             float shadowWeight, out float limiterActivity)
{
    limiterActivity = 0.0;
    float3 resolved = conventionalEdit;

    if (transferBand)
    {
        float transferConfidence = 0.0;
        const float gain =
            ExperimentStructureGain(gainReferenceBand, modelBand, nativeBand, polarityGuard,
                                    structureMaxGain, confidenceThreshold, transferConfidence);
        const float3 nativeGainEdit = nativeBand * (gain - 1.0);

        // A rejected/uncertain transfer now preserves the ordinary NR band exactly. Shadow protection
        // modulates only the Structure-Transfer contribution, never the baseline NR edit.
        const float transferWeight = structureStrength * transferConfidence * shadowWeight;
        resolved = lerp(resolved, nativeGainEdit, transferWeight);
    }

    // Structure transfer changes the candidate edit first. The edge limiter remains the final
    // per-band cap when both experiments are enabled.
    if (limitBand)
        resolved = ExperimentLimitedBand(resolved, nativeBand, limiterStrength, maxEdgeGain, limiterActivity);

    return resolved;
}

float ExperimentSoftEnvelopeScalar(float value, float lo, float hi, float softness)
{
    const float s = max(abs(softness), 1e-6);
    if (value > hi)
    {
        const float excess = value - hi;
        return hi + excess / (1.0 + excess / s);
    }
    if (value < lo)
    {
        const float excess = lo - value;
        return lo - excess / (1.0 + excess / s);
    }
    return value;
}

float3 ExperimentApplyStructureEnvelope(float3 ordinaryModel, float3 transferredModel,
                                        float3 nativeMin, float3 nativeMax,
                                        uint envelopeMode, float marginPercent)
{
    if (envelopeMode == 0u)
        return transferredModel;

    const float3 span = max(nativeMax - nativeMin, 0.0);
    const float3 margin = span * (marginPercent * 0.01);
    const float3 rawLo = nativeMin - margin;
    const float3 rawHi = nativeMax + margin;
    const float3 envelopeLo = min(rawLo, rawHi);
    const float3 envelopeHi = max(rawLo, rawHi);

    // Guard ONLY the extra change introduced by Structure Transfer. If ordinary NR is already
    // outside the native envelope, do not pull it back here; merely prevent Structure Transfer from
    // pushing it farther in the same direction.
    const float3 structureDelta = transferredModel - ordinaryModel;
    const float3 loDelta = min(float3(0.0, 0.0, 0.0), envelopeLo - ordinaryModel);
    const float3 hiDelta = max(float3(0.0, 0.0, 0.0), envelopeHi - ordinaryModel);

    float3 guardedDelta;
    if (envelopeMode == 1u)
    {
        guardedDelta = clamp(structureDelta, loDelta, hiDelta);
    }
    else
    {
        const float3 softness = max(span * 0.25, float3(0.002, 0.002, 0.002));
        guardedDelta.x = ExperimentSoftEnvelopeScalar(structureDelta.x, loDelta.x, hiDelta.x, softness.x);
        guardedDelta.y = ExperimentSoftEnvelopeScalar(structureDelta.y, loDelta.y, hiDelta.y, softness.y);
        guardedDelta.z = ExperimentSoftEnvelopeScalar(structureDelta.z, loDelta.z, hiDelta.z, softness.z);
    }

    return ordinaryModel + guardedDelta;
}

float3 ExperimentP100GuidedEdit(float2 uvq, float3 modelCenter, float3 baselineCenter,
                                float3 gainCenter, float3 nativeCenter, float normScale,
                                out float limiterActivity)
{
    const uint limiterMode = min(gResidualHistoryValidUnused, 2u);
    const uint structureMode = min(gResidualMotionBaseXUnused, 2u);
    const float structureStrength = gResidualConfidenceUnused;
    const float limiterStrength = gResidualBlendUnused;
    float maxEdgeGain = asfloat(gResidualMotionBaseYUnused);
    if (!isfinite(maxEdgeGain))
        maxEdgeGain = 1.0;

    // Structure-transfer safeguard settings are packed into resolve-only fields that are unused by
    // DlssNrMode_Resolve for Direct/Upscaled residual.
    const bool polarityGuard = (gGuideHeight & 1u) != 0u;
    const bool shadowProtection = (gGuideHeight & 2u) != 0u;
    const uint envelopeMode = min(gGuideWidth, 2u);
    const float envelopeMargin = gMvScaleY;
    const float structureMaxGain = isfinite(gResidualScale) ? gResidualScale : 4.0;
    const float confidenceThreshold = isfinite(gMvScaleX) ? gMvScaleX : 0.020;
    float shadowThreshold = asfloat(gExposureSourceWidth);
    float shadowStrength = asfloat(gExposureSourceHeight);
    if (!isfinite(shadowThreshold))
        shadowThreshold = 0.08;
    if (!isfinite(shadowStrength))
        shadowStrength = 1.0;

    float shadowWeight = 1.0;
    if (shadowProtection)
    {
        const float nativeY = max(dot(nativeCenter, kLuma), 0.0);
        const float visible = smoothstep(0.0, max(abs(shadowThreshold), 1e-6), nativeY);
        shadowWeight = lerp(1.0, visible, shadowStrength);
    }

    limiterActivity = 0.0;

    float3 modelBlur1, baselineBlur1, gainBlur1, nativeBlur1, nativeMin1, nativeMax1;
    ExperimentCrossBlur(uvq, 1.0, normScale, modelCenter, baselineCenter, gainCenter, nativeCenter,
                        modelBlur1, baselineBlur1, gainBlur1, nativeBlur1, nativeMin1, nativeMax1);

    const bool useTwoBands = limiterMode >= 2u || structureMode >= 2u;
    if (!useTwoBands)
    {
        // Fine band only (roughly 0..1 px). Structure transfer and limiter are independently enabled.
        const float3 lowEdit = modelBlur1 - baselineBlur1;
        const float3 modelFine = modelCenter - modelBlur1;
        const float3 baselineFine = baselineCenter - baselineBlur1;
        const float3 gainFine = gainCenter - gainBlur1;
        const float3 nativeFine = nativeCenter - nativeBlur1;
        float fineActivity = 0.0;
        const float3 fineEdit =
            ExperimentResolveBand(modelFine - baselineFine, gainFine, modelFine, nativeFine,
                                  limiterMode >= 1u, structureMode >= 1u,
                                  limiterStrength, maxEdgeGain, structureStrength,
                                  polarityGuard, structureMaxGain, confidenceThreshold, shadowWeight,
                                  fineActivity);
        limiterActivity = fineActivity;

        float3 guidedEdit = lowEdit + fineEdit;
        if (structureMode != 0u && envelopeMode != 0u)
        {
            const float3 transferredModel = baselineCenter + guidedEdit;
            const float3 guardedModel =
                ExperimentApplyStructureEnvelope(modelCenter, transferredModel, nativeMin1, nativeMax1,
                                                 envelopeMode, envelopeMargin);
            guidedEdit = guardedModel - baselineCenter;
        }
        return guidedEdit;
    }

    // Fine + mid decomposition. This path is selected by EITHER limiter mode 2 or structure mode 2,
    // so the two experiments remain genuinely independent.
    float3 modelBlur2, baselineBlur2, gainBlur2, nativeBlur2, nativeMin2, nativeMax2;
    ExperimentCrossBlur(uvq, 2.0, normScale, modelCenter, baselineCenter, gainCenter, nativeCenter,
                        modelBlur2, baselineBlur2, gainBlur2, nativeBlur2, nativeMin2, nativeMax2);

    const float3 lowEdit = modelBlur2 - baselineBlur2;

    const float3 modelFine = modelCenter - modelBlur1;
    const float3 baselineFine = baselineCenter - baselineBlur1;
    const float3 gainFine = gainCenter - gainBlur1;
    const float3 nativeFine = nativeCenter - nativeBlur1;
    float fineActivity = 0.0;
    const float3 fineEdit =
        ExperimentResolveBand(modelFine - baselineFine, gainFine, modelFine, nativeFine,
                              limiterMode >= 1u, structureMode >= 1u,
                              limiterStrength, maxEdgeGain, structureStrength,
                              polarityGuard, structureMaxGain, confidenceThreshold, shadowWeight,
                              fineActivity);

    const float3 modelMid = modelBlur1 - modelBlur2;
    const float3 baselineMid = baselineBlur1 - baselineBlur2;
    const float3 gainMid = gainBlur1 - gainBlur2;
    const float3 nativeMid = nativeBlur1 - nativeBlur2;
    float midActivity = 0.0;
    const float3 midEdit =
        ExperimentResolveBand(modelMid - baselineMid, gainMid, modelMid, nativeMid,
                              limiterMode >= 2u, structureMode >= 2u,
                              limiterStrength, maxEdgeGain, structureStrength,
                              polarityGuard, structureMaxGain, confidenceThreshold, shadowWeight,
                              midActivity);

    limiterActivity = max(fineActivity, midActivity);
    float3 guidedEdit = lowEdit + fineEdit + midEdit;

    // Combined envelope after BOTH structure bands have been recombined. This catches overshoot that
    // is harmless in each band separately but becomes ringing/sparkles when Fine and Mid add together.
    if (structureMode != 0u && envelopeMode != 0u)
    {
        const float3 nativeMin = min(nativeMin1, nativeMin2);
        const float3 nativeMax = max(nativeMax1, nativeMax2);
        const float3 transferredModel = baselineCenter + guidedEdit;
        const float3 guardedModel =
            ExperimentApplyStructureEnvelope(modelCenter, transferredModel, nativeMin, nativeMax,
                                             envelopeMode, envelopeMargin);
        guidedEdit = guardedModel - baselineCenter;
    }

    return guidedEdit;
}
#endif

// Detail-quality lab low-resolution filters. These run only at the NR working raster and only when
// explicitly enabled. Slider ranges are UI-only: numeric settings are never clamped to those ranges here.
// Only finite-value sanitisation, texture-coordinate bounds and tiny epsilons needed for valid arithmetic remain.
//
// Mode 16 mapping:
//   Transfer = input preparation selector; TransferStrength = radius/core width; ColourStrength = strength;
//   DebugScale = edge/core detection threshold; CompareMode = core mode (Off/Luma/RGB); MaxRatio = halo protection.
//
// Mode 17 mapping:
//   Transfer/TransferStrength/ColourStrength/DebugScale = NR edge mode/radius/strength/threshold;
//   CompareMode/DebugView/CompareSwap = Ghost Guard mode / band mode / cancel preparation footprint;
//   MaxRatio = Ghost Guard strength;
//   SkinDetail/SkinColour/EnvironmentDetail/EnvironmentColour = ghost detection threshold / sharp-edge threshold /
//       ghost edge-band radius / max suppression;
//   ReplaceDetailStrength/ModelWorkScale/ResidualConfidenceUnused = band strength / low-band radius / mid-band radius.
float ExperimentSmoothThreshold(float threshold, float value)
{
    const float t = max(abs(threshold), 1e-6);
    return smoothstep(t, t * 2.0, max(value, 0.0));
}

float ExperimentThinCoreScore(float yC, float yA, float yB)
{
    const float ySide = 0.5 * (yA + yB);
    // A thin centred structure differs from both sides while the two sides resemble each other.
    // A one-sided step edge has large side asymmetry and is therefore rejected.
    return max(abs(yC - ySide) - 0.5 * abs(yA - yB), 0.0);
}

float ExperimentRgbDistance(float3 a, float3 b)
{
    // Mean absolute channel distance keeps the threshold in roughly the same 0..1 scale as luma.
    return dot(abs(a - b), float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0));
}

float ExperimentThinCoreScoreRgb(float3 c, float3 a, float3 b)
{
    const float3 side = 0.5 * (a + b);
    return max(ExperimentRgbDistance(c, side) - 0.5 * ExperimentRgbDistance(a, b), 0.0);
}

float4 ExperimentEdgeCoreAttenuation(float2 uv)
{
    uint srcW, srcH;
    gSource.GetDimensions(srcW, srcH);
    const float2 texel = 1.0 / float2(max(srcW, 1u), max(srcH, 1u));
    const float4 center = gSource.SampleLevel(gLinear, uv, 0);
    const uint mode = min(gCompareMode, 2u);
    if (mode == 0u || gColourStrength == 0.0)
        return center;

    // Four cheap axis taps estimate a continuous normal. A strong first derivative is already a good
    // edge normal. Near the centre of a symmetric thin line the gradient vanishes, so second-derivative
    // magnitudes provide the candidate direction; the orthogonal candidate is tested only in that
    // ambiguous case. This handles diagonal wires without a large fixed kernel.
    const float4 left1  = gSource.SampleLevel(gLinear, saturate(uv - float2(texel.x, 0.0)), 0);
    const float4 right1 = gSource.SampleLevel(gLinear, saturate(uv + float2(texel.x, 0.0)), 0);
    const float4 up1    = gSource.SampleLevel(gLinear, saturate(uv - float2(0.0, texel.y)), 0);
    const float4 down1  = gSource.SampleLevel(gLinear, saturate(uv + float2(0.0, texel.y)), 0);
    const float yC = dot(center.rgb, kLuma);
    const float yL = dot(left1.rgb, kLuma);
    const float yR = dot(right1.rgb, kLuma);
    const float yU = dot(up1.rgb, kLuma);
    const float yD = dot(down1.rgb, kLuma);

    const float2 gradient = float2(yR - yL, yD - yU);
    const float gradLen = length(gradient);
    const float curveXLuma = abs(2.0 * yC - yL - yR);
    const float curveYLuma = abs(2.0 * yC - yU - yD);
    // RGB mode also sees isoluminant coloured wires. The same four L/R/U/D taps provide chromatic
    // second-derivative magnitudes, so this adds arithmetic but no fixed texture samples.
    const float curveXRgb = ExperimentRgbDistance(2.0 * center.rgb, left1.rgb + right1.rgb);
    const float curveYRgb = ExperimentRgbDistance(2.0 * center.rgb, up1.rgb + down1.rgb);
    const float curveX = mode == 2u ? max(curveXLuma, curveXRgb) : curveXLuma;
    const float curveY = mode == 2u ? max(curveYLuma, curveYRgb) : curveYLuma;
    const float curvatureLen = sqrt(curveX * curveX + curveY * curveY);

    float2 normal;
    if (gradLen > 1e-6)
    {
        normal = gradient / gradLen;
    }
    else if (curvatureLen > 1e-6)
    {
        // Axis-only second derivatives cannot distinguish the two diagonal signs at an exactly symmetric
        // centre. Pick one continuous candidate here; the orthogonal candidate below resolves that ambiguity.
        normal = normalize(float2(sqrt(curveX + 1e-8), sqrt(curveY + 1e-8)));
    }
    else
    {
        normal = float2(1.0, 0.0);
    }

    const float width = max(abs(gTransferStrength), 1e-4);
    float2 offset = normal * texel * width;
    float3 a = gSource.SampleLevel(gLinear, saturate(uv - offset), 0).rgb;
    float3 b = gSource.SampleLevel(gLinear, saturate(uv + offset), 0).rgb;
    float yA = dot(a, kLuma);
    float yB = dot(b, kLuma);
    float coreScore = mode == 2u
                          ? max(ExperimentThinCoreScore(yC, yA, yB),
                                ExperimentThinCoreScoreRgb(center.rgb, a, b))
                          : ExperimentThinCoreScore(yC, yA, yB);

    // For a perfectly centred diagonal line gradLen can be nearly zero. Axis second derivatives recover
    // |nx| and |ny| but not the sign of nx*ny, so there are exactly two mirrored normal orientations.
    // Two extra taps, only on that ambiguous path, choose the mirror with the stronger thin-core signature.
    if (gradLen <= max(abs(gDebugScale), 1e-6))
    {
        const float2 alternate = float2(normal.x, -normal.y);
        const float2 altOffset = alternate * texel * width;
        const float3 altA = gSource.SampleLevel(gLinear, saturate(uv - altOffset), 0).rgb;
        const float3 altB = gSource.SampleLevel(gLinear, saturate(uv + altOffset), 0).rgb;
        const float altYA = dot(altA, kLuma);
        const float altYB = dot(altB, kLuma);
        const float altScore = mode == 2u
                                   ? max(ExperimentThinCoreScore(yC, altYA, altYB),
                                         ExperimentThinCoreScoreRgb(center.rgb, altA, altB))
                                   : ExperimentThinCoreScore(yC, altYA, altYB);
        if (altScore > coreScore)
        {
            normal = alternate;
            a = altA;
            b = altB;
            yA = altYA;
            yB = altYB;
            coreScore = altScore;
        }
    }

    const float ySide = 0.5 * (yA + yB);
    const float detected = ExperimentSmoothThreshold(gDebugScale, coreScore);
    const float centralContrast =
        mode == 2u
            ? max(ExperimentRgbDistance(center.rgb, 0.5 * (a + b)), max(abs(gDebugScale), 1e-6))
            : max(abs(yC - ySide), max(abs(gDebugScale), 1e-6));
    const float sideAsymmetry =
        mode == 2u ? ExperimentRgbDistance(a, b) : abs(yA - yB);
    const float sideSimilarity = 1.0 - saturate(sideAsymmetry / centralContrast);
    // 0 = no halo rejection, 1 = linear side-similarity rejection, >1 = increasingly strict.
    // No slider-range clamp is applied: manually entered values (2, 5, 10, ...) retain their meaning.
    const float haloWeight = pow(max(sideSimilarity, 1e-6), gMaxRatio);
    const float amount = gColourStrength * detected * haloWeight;

    float3 result;
    if (mode == 1u)
    {
        // Luma-only changes the current/core pixel along the neutral axis. It never writes a blur footprint
        // into neighbours; neighbouring pixels are modified only if they independently satisfy thin-core detection.
        result = center.rgb + (ySide - yC) * amount;
    }
    else
    {
        // RGB mode also changes only the current/core pixel, moving it toward the two matching sides.
        result = lerp(center.rgb, 0.5 * (a + b), amount);
    }
    return float4(SanitizeFinite3(result, center.rgb), center.a);
}

float4 ExperimentLegacyLowResolutionFilter(float2 uv, bool postNr)
{
    if (!postNr && gTransfer == 4u)
        return ExperimentEdgeCoreAttenuation(uv);

    uint srcW, srcH;
    gSource.GetDimensions(srcW, srcH);
    const float2 texel = 1.0 / float2(max(srcW, 1u), max(srcH, 1u));
    const float radius = gTransferStrength;
    const float strength = gColourStrength;
    const float threshold = gDebugScale;

    const float4 center = gSource.SampleLevel(gLinear, uv, 0);
    if (postNr && (gTransfer == 0u || strength == 0.0))
        return center;

    const float2 dx = float2(texel.x * radius, 0.0);
    const float2 dy = float2(0.0, texel.y * radius);
    const float4 left  = gSource.SampleLevel(gLinear, saturate(uv - dx), 0);
    const float4 right = gSource.SampleLevel(gLinear, saturate(uv + dx), 0);
    const float4 up    = gSource.SampleLevel(gLinear, saturate(uv - dy), 0);
    const float4 down  = gSource.SampleLevel(gLinear, saturate(uv + dy), 0);

    const float yC = dot(center.rgb, kLuma);
    const float yL = dot(left.rgb, kLuma);
    const float yR = dot(right.rgb, kLuma);
    const float yU = dot(up.rgb, kLuma);
    const float yD = dot(down.rgb, kLuma);
    const float localMin = min(yC, min(min(yL, yR), min(yU, yD)));
    const float localMax = max(yC, max(max(yL, yR), max(yU, yD)));
    const float edgeRange = localMax - localMin;
    float edge = gTransfer == 1u ? 1.0 : ExperimentSmoothThreshold(threshold, edgeRange);

    float3 softened;
    if (gTransfer == 1u || gTransfer == 2u)
    {
        softened = (4.0 * center.rgb + left.rgb + right.rgb + up.rgb + down.rgb) * 0.125;
    }
    else
    {
        const float2 gradient = float2(yR - yL, yD - yU);
        const float gradLen = length(gradient);
        const float2 normal = gradLen > 1e-6 ? gradient / gradLen : float2(1.0, 0.0);
        const float2 offset = normal * texel * radius;
        const float3 a = gSource.SampleLevel(gLinear, saturate(uv - offset), 0).rgb;
        const float3 b = gSource.SampleLevel(gLinear, saturate(uv + offset), 0).rgb;
        softened = (a + 2.0 * center.rgb + b) * 0.25;

        if (postNr && gTransfer == 4u)
        {
            const float4 baseL = gModel.SampleLevel(gLinear, saturate(uv - dx), 0);
            const float4 baseR = gModel.SampleLevel(gLinear, saturate(uv + dx), 0);
            const float4 baseU = gModel.SampleLevel(gLinear, saturate(uv - dy), 0);
            const float4 baseD = gModel.SampleLevel(gLinear, saturate(uv + dy), 0);
            const float baseGrad =
                length(float2(dot(baseR.rgb - baseL.rgb, kLuma), dot(baseD.rgb - baseU.rgb, kLuma)));
            const float nrGrad = length(float2(yR - yL, yD - yU));
            // This saturate limits a derived confidence mask, not a user control.
            const float excess = saturate((nrGrad - baseGrad) / max(nrGrad + abs(threshold), 1e-5));
            edge *= excess;
        }
    }

    return float4(lerp(center.rgb, softened, strength * edge), center.a);
}

float4 ExperimentProcessedNrAt(float2 uv)
{
    // This is the first post-NR stage. Every later ghost/band helper samples THIS processed field,
    // never raw N, so NR50 edge treatment composes sequentially with band suppression.
    return gTransfer != 0u ? ExperimentLegacyLowResolutionFilter(uv, true)
                           : gSource.SampleLevel(gLinear, saturate(uv), 0);
}

float3 ExperimentProcessedResponseAt(float2 uv)
{
    const float3 processedNr = ExperimentProcessedNrAt(uv).rgb;
    const float3 prepared = gModel.SampleLevel(gLinear, saturate(uv), 0).rgb;
    return processedNr - prepared; // current E after optional NR50 edge treatment
}

float3 ExperimentCrossProcessedResponseBlur(float2 uv, float2 texel, float radius)
{
    const float r = max(abs(radius), 1e-4);
    const float2 dx = float2(texel.x * r, 0.0);
    const float2 dy = float2(0.0, texel.y * r);
    const float3 c = ExperimentProcessedResponseAt(uv);
    return (4.0 * c +
            ExperimentProcessedResponseAt(uv - dx) + ExperimentProcessedResponseAt(uv + dx) +
            ExperimentProcessedResponseAt(uv - dy) + ExperimentProcessedResponseAt(uv + dy)) * 0.125;
}

float ExperimentGhostEdgeMask(float2 uv, float2 texel)
{
    const float r = max(abs(gEnvironmentDetail), 1e-4);
    const float2 dx = float2(texel.x * r, 0.0);
    const float2 dy = float2(0.0, texel.y * r);
    const float yC = dot(gOriginal.SampleLevel(gLinear, uv, 0).rgb, kLuma);
    const float yL = dot(gOriginal.SampleLevel(gLinear, saturate(uv - dx), 0).rgb, kLuma);
    const float yR = dot(gOriginal.SampleLevel(gLinear, saturate(uv + dx), 0).rgb, kLuma);
    const float yU = dot(gOriginal.SampleLevel(gLinear, saturate(uv - dy), 0).rgb, kLuma);
    const float yD = dot(gOriginal.SampleLevel(gLinear, saturate(uv + dy), 0).rgb, kLuma);
    const float localMin = min(yC, min(min(yL, yR), min(yU, yD)));
    const float localMax = max(yC, max(max(yL, yR), max(yU, yD)));
    return ExperimentSmoothThreshold(gSkinColour, localMax - localMin);
}

float ExperimentSameDirectionMask(float3 preparationDelta, float3 response)
{
    const float prepY = dot(preparationDelta, kLuma);
    const float responseY = dot(response, kLuma);
    // Same sign means the model continues the preprocessing change: ghost candidate.
    // Opposite sign means the model is undoing the preparation (for example restoring the dark wire core).
    if (prepY * responseY <= 0.0)
        return 0.0;
    return ExperimentSmoothThreshold(gSkinDetail, abs(prepY));
}

float4 ExperimentNrArtifactControl(float2 uv)
{
    uint srcW, srcH;
    gSource.GetDimensions(srcW, srcH);
    const float2 texel = 1.0 / float2(max(srcW, 1u), max(srcH, 1u));

    const float4 processedNr = ExperimentProcessedNrAt(uv);
    const float3 prepared = gModel.SampleLevel(gLinear, uv, 0).rgb;   // B
    const float3 sharp = gOriginal.SampleLevel(gLinear, uv, 0).rgb;  // S
    const float3 preparationDelta = prepared - sharp;                 // D = B-S
    float3 response = processedNr.rgb - prepared;                     // E after optional NR edge treatment

    const uint bandMode = min(gDebugView, 2u);
    const uint guardMode = min(gCompareMode, 3u);
    float edgeMask = 1.0;
    if (bandMode != 0u || guardMode >= 2u)
        edgeMask = ExperimentGhostEdgeMask(uv, texel);

    // Second stage: suppress low/mid-frequency ghost components from the already edge-treated response.
    // With bandMode=0 there are no neighbourhood response taps at all.
    if (bandMode != 0u && gReplaceDetailStrength != 0.0)
    {
        const float3 lowBand = ExperimentCrossProcessedResponseBlur(uv, texel, gModelWorkScale);
        const float lowMask = ExperimentSameDirectionMask(preparationDelta, lowBand) * edgeMask;
        response -= lowBand * (gReplaceDetailStrength * lowMask);

        if (bandMode >= 2u)
        {
            const float3 midBlur = ExperimentCrossProcessedResponseBlur(uv, texel, gResidualConfidenceUnused);
            const float3 midBand = midBlur - lowBand;
            const float midMask = ExperimentSameDirectionMask(preparationDelta, midBand) * edgeMask;
            response -= midBand * (gReplaceDetailStrength * midMask);
        }
    }

    // Third stage: Ghost Guard evaluates the response left by all previous stages.
    // Basic mode has no low/mid taps and no sharp-edge-neighbourhood taps.
    if (guardMode != 0u && gMaxRatio != 0.0 && gEnvironmentColour != 0.0)
    {
        float guardMask = ExperimentSameDirectionMask(preparationDelta, response);
        if (guardMode >= 2u)
            guardMask *= edgeMask;

        // Max suppression is semantically a cap, but neither it nor Guard strength is clamped to 0..1.
        const float amount = min(gMaxRatio * guardMask, gEnvironmentColour);
        if (guardMode < 3u)
        {
            response *= 1.0 - amount;
        }
        else
        {
            // Directional mode suppresses only RGB components whose E continues D in the same direction.
            const float channelThreshold = max(abs(gSkinDetail) * 0.25, 1e-6);
            const float3 threshold3 = float3(channelThreshold, channelThreshold, channelThreshold);
            const float3 zero3 = float3(0.0, 0.0, 0.0);
            const float3 sameChannel =
                step(threshold3, abs(preparationDelta)) *
                step(zero3, preparationDelta * response);
            response *= 1.0 - amount * sameChannel;
        }
    }

    // Final stage. Off preserves the previous B + processed(E) behaviour exactly.
    // On removes the preparation footprint algebraically: S + processed(E) = N_processed - (B-S).
    const float3 base = gCompareSwap != 0u ? sharp : prepared;
    return float4(SanitizeFinite3(base + response, processedNr.rgb), processedNr.a);
}

#include "dlssnr_resize.hlsli"


// v5 classical inter-pass: downsample + ClampProxy in the SAME dispatch.
// Mode 32 applies the exact Mode 8 operation on the downsampled RGB.
// The reference first stores the downsample to the scratch UAV, then reloads
// it for ClampProxy. If the scratch is RGBA16_FLOAT, round-trip RGB through
// binary16 here before sanitization so the two-stage path and the fused path
// agree even for out-of-range finite values that overflow to FP16 infinity.
// Alpha is preserved; final UAV storage performs its normal format rounding.
float4 DownsampleMaybeClampProxy(float4 raw, bool fused)
{
    if (!fused)
        return raw;
    float3 rgb = raw.rgb;
    if ((gDirectResolveFlags & 512u) != 0u)
        rgb = f16tof32(f32tof16(rgb));
    return float4(saturate(SanitizeFinite3(rgb, 0.5)), raw.a);
}


// Only the two separately compiled RGB variants own the cooperative tile.
// The standard shader has no inter-pass LDS allocation.
// All variants retain identical root-signature bindings and constants.
#if defined(DLSSNR_TILED_FUSED)
// Share the completed native-P100 guided reconstructions
// between all 8x8 working-resolution pixels in this compute thread group.
//
// The source footprint for 8 adjacent output texels at P50-P90 is at most
// 17x17 native texels. The 20x20 tile leaves margin for integer rounding,
// but the dynamic extent is computed with the SAME Area edges as Mode 28.
// The shader only enters this branch if the entire group fits the tile.
// Crucially, every lane reaches the group barrier, including idle lanes at
// partial output groups along the bottom/right screen edges.
//
// Work per tile is (native guided samples in union) instead of
// sum(native guided samples for every reduced output pixel).
// Shared RGB storage remains FP32; original alpha is loaded after integration.
// Compile-time 16x16 or 20x20 pitch, selected only on provably fitting scales.
#if !defined(DLSSNR_TILED_PITCH)
#define DLSSNR_TILED_PITCH 20
#endif
static const uint kInterPassTilePitch = DLSSNR_TILED_PITCH;
#define DLSSNR_TILE_VALUE float3
float3 InterPassTileValue(float4 value) { return value.rgb; }
groupshared DLSSNR_TILE_VALUE gInterPassCorrectedTile[DLSSNR_TILED_PITCH * DLSSNR_TILED_PITCH];

bool InterPassTiledFusedArea(uint3 id, uint3 groupId, uint3 localId)
{
    uint nativeW, nativeH;
    gOriginal.GetDimensions(nativeW, nativeH);
    const uint2 outBegin = groupId.xy * uint2(8, 8);
    const uint2 outEnd = min(outBegin + uint2(8, 8), uint2(gWidth, gHeight));
    if (nativeW == 0u || nativeH == 0u || any(outBegin >= outEnd))
        return false;

    const float2 nativeSize = float2(nativeW, nativeH);
    const float2 workSize = float2(gWidth, gHeight);
    const int2 first = int2(floor(float2(outBegin) * nativeSize / workSize));
    const int2 limit = int2(ceil(float2(outEnd) * nativeSize / workSize));
    const int2 tileSize = limit - first;

    // Uniform per-group guard: unexpected dimensions use the original Mode
    // 28 Area path rather than ever indexing outside groupshared memory.
    if (any(tileSize <= 0) || any(tileSize > int2(kInterPassTilePitch, DLSSNR_TILED_PITCH)))
        return false;
    // Linear cooperative fill keeps the original integration math.
    const uint tileCount = (uint)tileSize.x * (uint)tileSize.y;
    const uint lane = localId.y * 8u + localId.x;
    [loop] for (uint index = lane; index < tileCount; index += 64u)
    {
        const uint x = index % (uint)tileSize.x;
        const uint y = index / (uint)tileSize.x;
        const int2 p100 = first + int2((int)x, (int)y);
        // Same v3 radius-one reconstruction used by regular Fused Area:
        // guided+bilinear reuse, P100 guide and frequency/shadow shaping.
        gInterPassCorrectedTile[y * kInterPassTilePitch + x] =
                InterPassTileValue(InterPassCorrectedP100LoadDynamic(p100));
    }
    GroupMemoryBarrierWithGroupSync();

    if (id.x >= gWidth || id.y >= gHeight)
        return true; // All lanes already participated in the barrier.

    // Mathematically identical Area footprint, integration order and alpha
    // selection to Mode 28, replacing ONLY repeated reconstructed-P100 loads.
    const float x0 = ((float)id.x * (float)nativeW) / (float)gWidth;
    const float x1 = ((float)(id.x + 1u) * (float)nativeW) / (float)gWidth;
    const float y0 = ((float)id.y * (float)nativeH) / (float)gHeight;
    const float y1 = ((float)(id.y + 1u) * (float)nativeH) / (float)gHeight;
    const float area = max((x1 - x0) * (y1 - y0), 1e-8);
    const int i0 = (int)floor(x0);
    const int i1 = (int)ceil(x1) - 1;
    const int j0 = (int)floor(y0);
    const int j1 = (int)ceil(y1) - 1;

    DLSSNR_TILE_VALUE acc = 0.0;
    [loop] for (int j = j0; j <= j1; ++j)
    {
        const float wy = max(min(y1, (float)j + 1.0) - max(y0, (float)j), 0.0);
        [loop] for (int i = i0; i <= i1; ++i)
        {
            const float wx = max(min(x1, (float)i + 1.0) - max(x0, (float)i), 0.0);
            const int2 localP100 = int2(i, j) - first;
            // A valid tile's group bounds encompass every output footprint.
            const DLSSNR_TILE_VALUE corrected = gInterPassCorrectedTile[
                (uint)localP100.y * kInterPassTilePitch + (uint)localP100.x];
            acc += corrected * (wx * wy);
        }
    }
    float4 corrected = float4(acc / area, 0.0);
    const int acx = clamp((int)floor(((float)id.x + 0.5) * (float)nativeW / (float)gWidth),
                          0, (int)nativeW - 1);
    const int acy = clamp((int)floor(((float)id.y + 0.5) * (float)nativeH / (float)gHeight),
                          0, (int)nativeH - 1);
    corrected.a = gOriginal.Load(int3(acx, acy, 0)).a;
    corrected.rgb = saturate(SanitizeFinite3(corrected.rgb, 0.5));
    gTarget[id.xy] = corrected;
    return true;
}
#endif

groupshared float4 gExposureReduce[64];
groupshared float4 gFinalColorReduceOriginal[64];
groupshared float4 gFinalColorReduceNr[64];

// Dedicated opt-in Low36 PSO only: 34x34 quarter-footprint samples cover
// all overlapping 6x6 filters of an 8x8 low-output group (13,872 bytes LDS).
// Keep all taps/weights/sanitization; sharing only changes coordinate FP rounding.
#if defined(DLSSNR_LOW36_CACHE)
#if !defined(DLSSNR_INTERPASS_LOW36)
#error Low36 cache requires the isolated Low36 PSO
#endif
groupshared float3 gLow36Tile[34 * 34];
void CacheLow36(uint2 groupId, uint2 localId)
{
    uint srcW, srcH;
    gSource.GetDimensions(srcW, srcH);
    const float2 srcSize = float2(srcW, srcH);
    const float2 footprint = srcSize / float2(gWidth, gHeight);
    const float2 srcCenter = (float2(groupId * 8u) + 0.5) * footprint;
    const float2 tapStep = 0.25 * footprint;
    const uint thread = localId.y * 8u + localId.x;
    [loop] for (uint i = thread; i < 34u * 34u; i += 64u)
    {
        const float2 uvq = clamp(srcCenter + (float2(i % 34u, i / 34u) - 2.5) * tapStep,
                                0.5, srcSize - 0.5) / srcSize;
        const float3 residual = gModel.SampleLevel(gLinear, uvq, 0).rgb -
                                gSource.SampleLevel(gLinear, uvq, 0).rgb;
        gLow36Tile[i] = SanitizeFinite3(residual, 0.0);
    }
    GroupMemoryBarrierWithGroupSync();
}
#endif

#if defined(DLSSNR_SUPERIOR_LOW)
// Same 64 taps as final low; borrow Low36 cooperative quarter-footprint sampling.
// An 8x16 output group shares 36x68 samples instead of fetching 128x64 taps.
groupshared float3 gLow64Tile[36 * 68];
void CacheLow64(uint2 groupId, uint2 localId)
{
    uint w, h, mw, mh;
    gSource.GetDimensions(w, h);
    gModel.GetDimensions(mw, mh);
    if (w > 0u && h > 0u && w == mw && h == mh)
    {
        const float2 size = float2(w, h), footprint = size / float2(gWidth, gHeight);
        const float2 center = (float2(groupId * uint2(8u, 16u)) + 0.5) * footprint;
        const float2 step = 0.25 * footprint;
        const uint lane = localId.y * 8u + localId.x;
        [loop] for (uint i = lane; i < 36u * 68u; i += 128u)
        {
            const float2 uv = clamp(center + (float2(i % 36u, i / 36u) - 3.5) * step,
                                   0.5, size - 0.5) / size;
            gLow64Tile[i] = SanitizeFinite3(gModel.SampleLevel(gLinear, uv, 0).rgb, 0.0);
        }
    }
    GroupMemoryBarrierWithGroupSync();
}
#endif

// Fixed Superior Scaling carrier: no K texture, analysis, headroom or telemetry.
[numthreads(8, 8, 1)]
void SuperiorCarrierMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight) return;
    const int3 p = int3(id.xy, 0);
    if (gMode == 37u || gMode == 39u)
    {
        const float3 proxy = SanitizeFinite3(gSource.Load(p).rgb, 0.0);
        const float3 model = SanitizeFinite3(gModel.Load(p).rgb, proxy);
        // Preserve the legacy K=1 rounding order; do not reassociate the neutral offset.
        precise float3 residual = model - proxy;
        precise float3 encoded = 0.5 + residual;
        gTarget[id.xy] = float4(clamp(encoded, -1.0, 2.0), 1.0);
        if (gMode == 39u)
        {
            // Identical point coordinates, multiplication and finite-pair handling to Mode 10.
            // Read original motion, not NR's sanitized motion: non-finite pairs and signed zero matter.
            const float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
            const uint2 size = uint2(gTransferStrength, gColourStrength);
            const uint2 mp = min(uint2(uv * size), size - 1) + uint2(gCompareSwap, gTransfer);
            const float2 mv = gOriginal.Load(int3(mp, 0)).xy * float2(gMvScaleX, gMvScaleY);
            gKeep[id.xy] = float4(all(isfinite(mv)) ? mv : float2(0, 0), 0, 1);
        }
    }
    else
    {
        const float3 filtered = SanitizeFinite3(gSource.Load(p).rgb, 0.5);
        gTarget[id.xy] = float4(filtered - 0.5, 1.0);
    }
}

#if defined(DLSSNR_SUPERIOR_LOW)
[numthreads(8, 16, 1)]
#else
[numthreads(8, 8, 1)]
#endif
void CSMain(uint3 id : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    const uint lane = groupThreadId.y * 8u + groupThreadId.x;

    if (gMode == 3)
    {
        if (groupId.x >= gWidth || groupId.y >= gHeight)
            return;

        uint fullW, fullH;
        gSource.GetDimensions(fullW, fullH);

        const uint tx0 = (groupId.x * fullW) / gWidth;
        const uint tx1 = ((groupId.x + 1u) * fullW) / gWidth;
        const uint ty0 = (groupId.y * fullH) / gHeight;
        const uint ty1 = ((groupId.y + 1u) * fullH) / gHeight;
        const uint endX = max(tx1, tx0 + 1u);
        const uint endY = max(ty1, ty0 + 1u);

        float localSum = 0.0;
        [loop] for (uint ty = ty0 + groupThreadId.y; ty < endY; ty += 8u)
        {
            [loop] for (uint tx = tx0 + groupThreadId.x; tx < endX; tx += 8u)
            {
                const float3 c = max(gSource.Load(int3(min(tx, fullW - 1u), min(ty, fullH - 1u), 0)).rgb, 0.0);
                const float luma = dot(c, kLuma);
                localSum += isfinite(luma) ? max(luma, 0.0) : 0.0;
            }
        }

        gExposureReduce[lane] = float4(localSum, 0.0, 0.0, 0.0);
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint stride = 32u; stride > 0u; stride >>= 1u)
        {
            if (lane < stride)
                gExposureReduce[lane].x += gExposureReduce[lane + stride].x;
            GroupMemoryBarrierWithGroupSync();
        }

        if (lane == 0u)
        {
            const uint taken = (endX - tx0) * (endY - ty0);
            gTarget[groupId.xy] = float4(taken > 0u ? gExposureReduce[0].x / (float) taken : 0.0,
                                         0.0, 0.0, 1.0);
        }
        return;
    }

    if (gMode == 11)
    {
        const uint srcW = max(gExposureSourceWidth, 1u);
        const uint srcH = max(gExposureSourceHeight, 1u);
        const float preExposure =
            (isfinite(gPreExposure) && gPreExposure > 1e-6) ? gPreExposure : 1.0;
        const float protection = saturate(gExposureProtection * 0.01);

        float weightedBufferLuma = 0.0;
        float weightedSceneLogLuma = 0.0;
        float totalPixels = 0.0;

        [loop] for (uint index = lane; index < 4096u; index += 64u)
        {
            const uint tx = index & 63u;
            const uint ty = index >> 6u;
            const uint x0 = (tx * srcW) / 64u;
            const uint x1 = ((tx + 1u) * srcW) / 64u;
            const uint y0 = (ty * srcH) / 64u;
            const uint y1 = ((ty + 1u) * srcH) / 64u;
            const uint tileW = max(x1 - x0, 1u);
            const uint tileH = max(y1 - y0, 1u);
            const float pixels = (float) tileW * (float) tileH;
            const float tileMean = max(SanitizeFinite(gSource.Load(int3(tx, ty, 0)).r, 0.0), 0.0);

            weightedBufferLuma += tileMean * pixels;
            totalPixels += pixels;
            if (protection > 0.0)
            {
                const float sceneLuma = max(tileMean / preExposure, 1e-8);
                weightedSceneLogLuma += clamp(log2(sceneLuma), -24.0, 24.0) * pixels;
            }
        }

        gExposureReduce[lane] = float4(weightedBufferLuma, weightedSceneLogLuma, totalPixels, 0.0);
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint stride = 32u; stride > 0u; stride >>= 1u)
        {
            if (lane < stride)
                gExposureReduce[lane].xyz += gExposureReduce[lane + stride].xyz;
            GroupMemoryBarrierWithGroupSync();
        }

        const float allPixels = gExposureReduce[0].z;
        const float averageBufferLuma =
            allPixels > 0.0 ? gExposureReduce[0].x / allPixels : 0.0;
        float meteredSceneLuma = averageBufferLuma / preExposure;

        if (protection > 0.0 && allPixels > 0.0)
        {
            const float referenceLogLuma = gExposureReduce[0].y / allPixels;
            const float highlightKneeEv = lerp(3.0, 1.0, protection);
            const float highlightCompressionSlope = lerp(1.0, 0.35, protection);
            float protectedLinearSum = 0.0;

            [loop] for (uint index2 = lane; index2 < 4096u; index2 += 64u)
            {
                const uint tx2 = index2 & 63u;
                const uint ty2 = index2 >> 6u;
                const uint x0 = (tx2 * srcW) / 64u;
                const uint x1 = ((tx2 + 1u) * srcW) / 64u;
                const uint y0 = (ty2 * srcH) / 64u;
                const uint y1 = ((ty2 + 1u) * srcH) / 64u;
                const uint tileW = max(x1 - x0, 1u);
                const uint tileH = max(y1 - y0, 1u);
                const float pixels = (float) tileW * (float) tileH;
                const float tileMean = max(SanitizeFinite(gSource.Load(int3(tx2, ty2, 0)).r, 0.0), 0.0);
                const float sceneLuma = max(tileMean / preExposure, 1e-8);
                const float logLuma = clamp(log2(sceneLuma), -24.0, 24.0);
                const float deltaEv = logLuma - referenceLogLuma;
                float compressedLogLuma = logLuma;
                if (deltaEv > highlightKneeEv)
                    compressedLogLuma = referenceLogLuma + highlightKneeEv +
                                        (deltaEv - highlightKneeEv) * highlightCompressionSlope;
                protectedLinearSum += exp2(clamp(compressedLogLuma, -24.0, 24.0)) * pixels;
            }

            gExposureReduce[lane].w = protectedLinearSum;
            GroupMemoryBarrierWithGroupSync();
            [unroll] for (uint stride2 = 32u; stride2 > 0u; stride2 >>= 1u)
            {
                if (lane < stride2)
                    gExposureReduce[lane].w += gExposureReduce[lane + stride2].w;
                GroupMemoryBarrierWithGroupSync();
            }

            const float protectedAverage = gExposureReduce[0].w / allPixels;
            if (isfinite(protectedAverage) && protectedAverage > 1e-8)
                meteredSceneLuma = protectedAverage;
        }

        if (lane == 0u)
        {
            float white = preExposure * meteredSceneLuma * (0.82 / 0.18);
            gTarget[uint2(0, 0)] = float4(isfinite(white) && white > 1e-8 ? white : 1.0, 0, 0, 1);
        }
        return;
    }

    if (gMode == 30)
    {
        // Reduce sparse paired OKLab statistics and smooth the measured correction rather than pixels.
        const uint statsW = max(gGuideWidth, 1u);
        const uint statsH = max(gGuideHeight, 1u);
        const uint sampleCount = statsW * statsH;
        float4 sumOriginal = 0.0;
        float4 sumNr = 0.0;
        [loop] for (uint index = lane; index < sampleCount; index += 64u)
        {
            const uint2 p = uint2(index % statsW, index / statsW);
            sumOriginal += gSource.Load(int3(p, 0));
            sumNr += gModel.Load(int3(p, 0));
        }
        gFinalColorReduceOriginal[lane] = sumOriginal;
        gFinalColorReduceNr[lane] = sumNr;
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint stride = 32u; stride > 0u; stride >>= 1u)
        {
            if (lane < stride)
            {
                gFinalColorReduceOriginal[lane] += gFinalColorReduceOriginal[lane + stride];
                gFinalColorReduceNr[lane] += gFinalColorReduceNr[lane + stride];
            }
            GroupMemoryBarrierWithGroupSync();
        }
        if (lane == 0u)
        {
            const float4 totalOriginal = gFinalColorReduceOriginal[0];
            const float4 totalNr = gFinalColorReduceNr[0];
            const bool valid = totalOriginal.w > 1.0e-4 && totalNr.w > 1.0e-4;
            float3 rawCorrection = 0.0; // delta a, delta b, log2(chroma ratio)
            if (valid)
            {
                const float3 meanOriginal = totalOriginal.xyz / totalOriginal.w;
                const float3 meanNr = totalNr.xyz / totalNr.w;
                float2 delta = meanOriginal.xy - meanNr.xy;
                const float deltaLength = length(delta);
                if (deltaLength > 0.5)
                    delta *= 0.5 / deltaLength;
                const float chromaRatio = clamp(meanOriginal.z / max(meanNr.z, 1.0e-5), 0.25, 4.0);
                rawCorrection = float3(delta, log2(chromaRatio));
            }

            const float3 previous = gApplyModel != 0u
                ? SanitizeFinite3(gOriginal.Load(int3(0, 0, 0)).xyz, rawCorrection)
                : rawCorrection;
            const float tauMs = max(SanitizeFinite(gMaxRatio, 0.0), 0.0);
            const float dtMs = clamp(SanitizeFinite(gDebugScale, 16.67), 0.01, 1000.0);
            const float alpha = tauMs <= 0.001 ? 1.0 : 1.0 - exp(-dtMs / tauMs);
            const float3 smoothed = gApplyModel != 0u ? lerp(previous, rawCorrection, saturate(alpha)) : rawCorrection;
            gTarget[uint2(0, 0)] = float4(SanitizeFinite3(smoothed, previous), valid ? 1.0 : 0.0);
        }
        return;
    }

#if defined(DLSSNR_TILED_FUSED)
    // Group-wide v7 must run BEFORE the per-thread bounds return, otherwise
    // partial edge groups would deadlock at GroupMemoryBarrierWithGroupSync.
    // The dispatch bit is set only for valid Fused+Area+radius-one geometry.
    if (gMode == 28u && (gDirectResolveFlags & 2048u) != 0u)
    {
        if (InterPassTiledFusedArea(id, groupId, groupThreadId))
            return;
    }
#endif

#if defined(DLSSNR_LOW36_CACHE)
    // All lanes must participate, including lanes outside the final partial tile.
    CacheLow36(groupId.xy, groupThreadId.xy);
#endif

#if defined(DLSSNR_SUPERIOR_LOW)
    // All lanes participate, including those outside the partial output group.
    CacheLow64(groupId.xy, groupThreadId.xy);
#endif

    if (id.x >= gWidth || id.y >= gHeight)
        return;

    // Normalised, so the source may be any size relative to this dispatch.
    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);

#if !defined(VK_MODE)
    if (gMode == 33u)
    {
        gTarget[id.xy] = InterPassFastGuided(int2(id.xy));
        return;
    }
    if (gMode == 36u)
    {
        gTarget[id.xy] = InterPassFastCross5(int2(id.xy));
        return;
    }
#endif

    if (gMode == 34u)
    {
        // Inter-pass only: same signed N-B domain and low-field dimensions as Mode18.
        // Keep the corrected Mode18 tap spacing; remove only its two outer taps per axis.
        // Positive overlapping +/-0.625 output-pixel support, renormalised signed DC.
        // A wider six-tap spacing introduced additional comb aliases in the frequency audit.
        uint srcW, srcH, modelW, modelH;
        gSource.GetDimensions(srcW, srcH);
        gModel.GetDimensions(modelW, modelH);
        if (srcW == 0u || srcH == 0u || modelW != srcW || modelH != srcH)
        {
            gTarget[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
            return;
        }
        const float2 srcSize = float2(srcW, srcH);
        const float2 footprint = srcSize / float2(gWidth, gHeight);
        const float2 srcCenter = (float2(id.xy) + 0.5) * footprint;
        const float2 tapStep = 0.25 * footprint;
        static const float weights[6] = { 11.0, 15.0, 17.0, 17.0, 15.0, 11.0 };
        float3 sum = 0.0;
        [unroll] for (uint y = 0u; y < 6u; ++y)
        {
            const float sy = clamp(srcCenter.y + ((float)y - 2.5) * tapStep.y,
                                   0.5, srcSize.y - 0.5) / srcSize.y;
            [unroll] for (uint x = 0u; x < 6u; ++x)
            {
#if defined(DLSSNR_LOW36_CACHE)
                const uint2 tap = groupThreadId.xy * 4u + uint2(x, y);
                sum += gLow36Tile[tap.y * 34u + tap.x] * (weights[x] * weights[y]);
#else
                const float sx = clamp(srcCenter.x + ((float)x - 2.5) * tapStep.x,
                                       0.5, srcSize.x - 0.5) / srcSize.x;
                const float2 uv = float2(sx, sy);
                const float3 residual = gModel.SampleLevel(gLinear, uv, 0).rgb -
                                        gSource.SampleLevel(gLinear, uv, 0).rgb;
                sum += SanitizeFinite3(residual, 0.0) * (weights[x] * weights[y]);
#endif
            }
        }
        gTarget[id.xy] = float4(sum * (1.0 / 7396.0), 1.0);
        return;
    }

    if (gMode == 29)
    {
        // Four deterministic sub-cell samples per stats texel keep measurement cheap but representative.
        uint originalW, originalH, nrW, nrH;
        gSource.GetDimensions(originalW, originalH);
        gModel.GetDimensions(nrW, nrH);
        float4 originalStats = 0.0;
        float4 nrStats = 0.0;
        [unroll] for (uint sy = 0u; sy < 2u; ++sy)
        {
            [unroll] for (uint sx = 0u; sx < 2u; ++sx)
            {
                const float2 sub = (float2(id.xy) + (float2(sx, sy) + 0.5) * 0.5) / float2(gWidth, gHeight);
                const int2 po = int2(min(uint2(sub * float2(originalW, originalH)),
                                         uint2(originalW - 1u, originalH - 1u)));
                const int2 pn = int2(min(uint2(sub * float2(nrW, nrH)), uint2(nrW - 1u, nrH - 1u)));
                // Linear HDR/scRGB intentionally permits negative Rec.709 components: after HDR10 BT.2020 ->
                // linear Rec.709 conversion they carry valid wide-gamut chromaticity. Only display-referred SDR
                // is constrained to [0,1] before its sRGB decode.
                float3 original = SanitizeFinite3(gSource.Load(int3(po, 0)).rgb, float3(0.0, 0.0, 0.0));
                float3 edited = SanitizeFinite3(gModel.Load(int3(pn, 0)).rgb, original);
                if (gPassthrough != 0u)
                {
                    original = SrgbToLinear(saturate(original));
                    edited = SrgbToLinear(saturate(edited));
                }
                const float3 originalLab = ToOkLab(original);
                const float3 editedLab = ToOkLab(edited);
                const float lowL = min(abs(originalLab.x), abs(editedLab.x));
                const float highL = max(abs(originalLab.x), abs(editedLab.x));
                // Near-black chroma is numerically unstable; very bright HDR samples get a soft influence cap.
                const float weight = smoothstep(0.015, 0.080, lowL) /
                                     (1.0 + 0.25 * max(highL - 1.0, 0.0));
                const float originalChroma = min(length(originalLab.yz), 4.0);
                const float editedChroma = min(length(editedLab.yz), 4.0);
                originalStats += float4(originalLab.y * weight, originalLab.z * weight,
                                        originalChroma * weight, weight);
                nrStats += float4(editedLab.y * weight, editedLab.z * weight, editedChroma * weight, weight);
            }
        }
        gTarget[id.xy] = originalStats;
        gKeep[id.xy] = nrStats;
        return;
    }

    if (gMode == 31)
    {
        const float4 sourceRaw = gSource.Load(int3(id.xy, 0));
        // Keep signed scRGB/linear Rec.709 intact for HDR. Negative components can be valid wide-gamut colours
        // which are converted back to BT.2020/PQ after this pass. SDR remains bounded before sRGB decoding.
        float3 linearColor = SanitizeFinite3(sourceRaw.rgb, float3(0.0, 0.0, 0.0));
        if (gPassthrough != 0u)
            linearColor = SrgbToLinear(saturate(linearColor));

        const float4 correction = gModel.Load(int3(0, 0, 0));
        float3 lab = ToOkLab(linearColor);
        const float3 safeCorrection = SanitizeFinite3(correction.xyz, float3(0.0, 0.0, 0.0));

        const float chromaBeforeChromaticity = length(lab.yz);
        lab.yz += safeCorrection.xy * saturate(gTransferStrength);
        float chromaAfterChromaticity = length(lab.yz);

        // Optional strict chromaticity-only mode. Keep the new a/b direction/cast, but restore the chroma
        // magnitude the pixel had before Temperature/Hue. This intentionally leaves the legacy behaviour available.
        if (gApplyModel != 0u)
        {
            if (chromaBeforeChromaticity <= 1.0e-6)
            {
                lab.yz = 0.0;
                chromaAfterChromaticity = 0.0;
            }
            else if (chromaAfterChromaticity > 1.0e-6)
            {
                lab.yz *= chromaBeforeChromaticity / chromaAfterChromaticity;
                chromaAfterChromaticity = chromaBeforeChromaticity;
            }
        }

        const float perceptualSaturation = chromaAfterChromaticity / max(abs(lab.x), 0.05);

        // The whole-frame target was measured against raw NR. Temperature/Hue may already have changed local
        // chroma, so subtract that contribution before applying Saturation/Vibrance. At 100% saturation recovery,
        // the combined result targets the same missing chroma instead of blindly stacking two gains.
        const float chromaEpsilon = 1.0e-5;
        const float chromaticityLogChromaChange =
            log2((chromaAfterChromaticity + chromaEpsilon) / (chromaBeforeChromaticity + chromaEpsilon));
        const float remainingTargetLogGain = safeCorrection.z - chromaticityLogChromaChange;
        float recoveryWeight = 1.0;
        if (gTransfer == 1u)
        {
            // Deliberately stronger than the first implementation: muted colours receive most of the recovery,
            // while vivid colours receive only a small fraction. This should be visibly distinct from Saturation.
            const float vivid = smoothstep(0.05, 0.35, perceptualSaturation);
            recoveryWeight *= lerp(1.0, 0.05, vivid);
        }

        const float requestedLogGain = remainingTargetLogGain * saturate(gColourStrength);
        if (requestedLogGain > 0.0 && gMaxDarkening > 0.0)
        {
            const float highSaturation = smoothstep(0.18, 0.45, perceptualSaturation);
            recoveryWeight *= lerp(1.0, 1.0 - highSaturation, saturate(gMaxDarkening));
        }
        lab.yz *= exp2(clamp(requestedLogGain * recoveryWeight, -2.0, 2.0));

        float3 corrected = SanitizeFinite3(FromOkLab(lab), linearColor);
        if (gPassthrough != 0u)
        {
            // Display-referred SDR still needs ordinary gamut protection and legal sRGB output.
            corrected = max(ClampAp1(corrected), 0.0);
            corrected = saturate(LinearToSrgb(corrected));
        }
        // HDR deliberately skips ClampAp1/max(0): preserving signed linear Rec.709 lets the existing
        // Rec.709 -> BT.2020 -> PQ conversion reconstruct wide-gamut colours instead of clipping them here.
        gTarget[id.xy] = float4(corrected, sourceRaw.a);
        return;
    }

    if (gMode == 14)
    {
        gTarget[id.xy] = gSource.SampleLevel(gLinear, uv, 0);
        return;
    }

    if (gMode == 16)
    {
        gTarget[id.xy] = ExperimentLegacyLowResolutionFilter(uv, false);
        return;
    }
    if (gMode == 17)
    {
        gTarget[id.xy] = ExperimentNrArtifactControl(uv);
        return;
    }

#if !defined(VK_MODE)
    if (gMode == 27)
    {
        // Reference inter-pass path: materialize C100 = P100 + shaped Guided(Ncurrent-Boriginal, P100).
        // Do not clamp here; the selected P100->working filter runs next, followed by ClampProxy.
        // Bit 10: standalone v6 A/B. Only selected when exact optimized
        // mode, radius=1, nonzero guide strength and equal source/model dims.
        // OFF remains the byte-for-byte v5 P100 guided shader path.
        gTarget[id.xy] = (gDirectResolveFlags & 1024u) != 0u
            ? InterPassCorrectedClassicSharedStencil(int2(id.xy))
            : InterPassCorrectedP100Load(int2(id.xy));
        return;
    }

    if (gMode == 28)
    {
        // Fused inter-pass path for local filters. Evaluate conceptual C100 texels while integrating
        // them into the working pixel, so no corrected full-resolution surface is written.
        // Transfer is the active proxy downfilter: 0 Area, 1 Bilinear, 4 Point.
        uint nativeW, nativeH;
        gOriginal.GetDimensions(nativeW, nativeH);
        if (nativeW == 0u || nativeH == 0u)
        {
            gTarget[id.xy] = float4(0.5, 0.5, 0.5, 1.0);
            return;
        }

        const float2 sampleUv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
        float4 corrected = 0.0;
        uint sourceW, sourceH, answerW, answerH;
        gSource.GetDimensions(sourceW, sourceH);
        gModel.GetDimensions(answerW, answerH);
        if ((gDirectResolveFlags & 16u) != 0u && gTransfer == 0u &&
            nativeW == 2u * gWidth && nativeH == 2u * gHeight &&
            sourceW == gWidth && sourceH == gHeight &&
            answerW == gWidth && answerH == gHeight)
        {
            // v2 reuses the 3x3 source loads for BOTH the bilateral and
            // bilinear terms; radius 2/3 or no-guide uses the v1 fallback.
            const bool sharedBilinear = (gDirectResolveFlags & 64u) != 0u &&
                                        gResidualHistoryValidUnused == 1u &&
                                        gResidualConfidenceUnused > 0.0;
            corrected = sharedBilinear
                ? InterPassCorrectedAreaP50SharedBilinear(int2(id.xy), uint2(nativeW, nativeH))
                : InterPassCorrectedAreaP50Optimized(int2(id.xy), uint2(nativeW, nativeH));
        }
        else if (gTransfer == 1u)
        {
            corrected = InterPassCorrectedP100Bilinear(sampleUv);
        }
        else if (gTransfer == 4u)
        {
            const int2 p = int2(clamp(floor(sampleUv * float2(nativeW, nativeH)), 0.0,
                                     float2(nativeW - 1, nativeH - 1)));
            corrected = InterPassCorrectedP100Load(p);
        }
        else
        {
            const float x0 = ((float) id.x * (float) nativeW) / (float) gWidth;
            const float x1 = ((float) (id.x + 1) * (float) nativeW) / (float) gWidth;
            const float y0 = ((float) id.y * (float) nativeH) / (float) gHeight;
            const float y1 = ((float) (id.y + 1) * (float) nativeH) / (float) gHeight;
            const float area = max((x1 - x0) * (y1 - y0), 1e-8);
            const int i0 = (int) floor(x0);
            const int i1 = (int) ceil(x1) - 1;
            const int j0 = (int) floor(y0);
            const int j1 = (int) ceil(y1) - 1;
            float4 acc = 0.0;
            [loop] for (int j = j0; j <= j1; ++j)
            {
                const float wy = max(min(y1, (float) j + 1.0) - max(y0, (float) j), 0.0);
                [loop] for (int i = i0; i <= i1; ++i)
                {
                    const float wx = max(min(x1, (float) i + 1.0) - max(x0, (float) i), 0.0);
                    // Reuse each native pixel's bilinear and guided stencil.
                    const bool dynamicShared = (gDirectResolveFlags & 128u) != 0u &&
                        gResidualHistoryValidUnused == 1u && gResidualConfidenceUnused > 0.0 &&
                        sourceW == gWidth && sourceH == gHeight &&
                        answerW == gWidth && answerH == gHeight;
                        const float4 sampleCorrected =
                            dynamicShared ? InterPassCorrectedP100LoadDynamic(int2(i, j))
                                          : InterPassCorrectedP100Load(int2(i, j));
                        acc += sampleCorrected * (wx * wy);
                }
            }
            corrected = acc / area;
            const int acx = clamp((int) floor(((float) id.x + 0.5) * (float) nativeW / (float) gWidth),
                                  0, (int) nativeW - 1);
            const int acy = clamp((int) floor(((float) id.y + 0.5) * (float) nativeH / (float) gHeight),
                                  0, (int) nativeH - 1);
            corrected.a = gOriginal.Load(int3(acx, acy, 0)).a;
        }

        // Same domain contract as ClampProxy in the reference path.
        corrected.rgb = saturate(SanitizeFinite3(corrected.rgb, 0.5));
        gTarget[id.xy] = corrected;
        return;
    }
#endif

    if (gMode == 18)
    {
        // Low-frequency residual for P100-guided shaping. The old non-overlapping 8x8
        // box reduction has weak stop-band rejection and can fold P50 detail into
        // the 1/8-resolution field. With high/mid suppressed, that folded detail
        // becomes visible as a coarse moving pattern in the final P100 resolve.
        //
        // Keep the SAME signed raw-domain residual and output dimensions, but use
        // an overlapping separable 8-tap positive-window reconstruction kernel. Its
        // taps are 1/4 of one output pixel apart in source space; spacing them
        // by two P50 pixels at P50->mip3 also rejects high-frequency comb aliases
        // that wider regularly-spaced taps would fold into E_low. Hardware bilinear
        // filtering handles each tap without another GPU pass.
        // Nonnegative normalized weights preserve constant/DC edits and darkening.
        uint srcW, srcH;
        uint modelW, modelH;
        gSource.GetDimensions(srcW, srcH);
        gModel.GetDimensions(modelW, modelH);
        if (srcW == 0u || srcH == 0u || modelW != srcW || modelH != srcH)
        {
            gTarget[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
            return;
        }

        const float2 srcSize = float2(srcW, srcH);
        const float2 footprint = srcSize / float2(gWidth, gHeight);
        const float2 srcCenter = (float2(id.xy) + 0.5) * footprint;
        const float2 tapStep = 0.25 * footprint;
        // Symmetric positive weights sum to 100; tuned to reject frequencies
        // above the low-field Nyquist without any negative-lobe ringing.
        static const float weights[8] = { 7.0, 11.0, 15.0, 17.0, 17.0, 15.0, 11.0, 7.0 };

        float3 sum = 0.0;
        [unroll] for (uint y = 0u; y < 8u; ++y)
        {
            const float sy = clamp(srcCenter.y + ((float) y - 3.5) * tapStep.y,
                                   0.5, srcSize.y - 0.5) / srcSize.y;
            [unroll] for (uint x = 0u; x < 8u; ++x)
            {
                const float sx = clamp(srcCenter.x + ((float) x - 3.5) * tapStep.x,
                                       0.5, srcSize.x - 0.5) / srcSize.x;
                const float2 uv = float2(sx, sy);
#if defined(DLSSNR_SUPERIOR_LOW)
                const uint2 tileP = groupThreadId.xy * 4u + uint2(x, y);
                const float3 modelTap = gLow64Tile[tileP.y * 36u + tileP.x];
#else
                const float3 modelTap = gModel.SampleLevel(gLinear, uv, 0).rgb;
#endif
                // Temporal modes 9/10 already contain the signed residual; other
                // modes still compute N-P in exactly the original texture domain.
                const float3 residualTap = gTransfer != 0u
                    ? modelTap : modelTap - gSource.SampleLevel(gLinear, uv, 0).rgb;
                sum += SanitizeFinite3(residualTap, 0.0) * (weights[x] * weights[y]);
            }
        }
        // sum(weights) = 100 in each dimension, total weight = 10000.
        gTarget[id.xy] = float4(sum * (1.0 / 10000.0), 1.0);
        return;
    }

    if (gMode == 20)
    {
        // First-stage temporal-carrier analysis. One output texel scans one source tile (normally ~32x32).
        // gTarget = strict K, robust K, minimum white-side headroom, minimum black-side headroom.
        // gKeep   = white-zero pixels, black-zero pixels, raw Proxy<0 pixels, raw Proxy>1 pixels.
        const uint srcW = max(gGuideWidth, 1u);
        const uint srcH = max(gGuideHeight, 1u);
        const uint x0 = (id.x * srcW) / gWidth;
        const uint x1 = max(((id.x + 1u) * srcW) / gWidth, x0 + 1u);
        const uint y0 = (id.y * srcH) / gHeight;
        const uint y1 = max(((id.y + 1u) * srcH) / gHeight, y0 + 1u);
        const float margin = clamp(abs(gResidualScale), 0.0, 0.49);
        const float anchorStrength = saturate(gTransferStrength);
        // Linear/image carriers use the manually selected FP16 range. Neutral is always its midpoint.
        // Nonlinear encoding remains its original bounded [0,1] mapping and ignores these values.
        const float2 carrierRange = TemporalCarrierRange();
        const float carrierLow = carrierRange.x;
        const float carrierHigh = carrierRange.y;
        const float carrierNeutral = 0.5 * (carrierLow + carrierHigh);
        const float huge = 1.0e20;

        float strictMin = huge;
        float secondMin = huge;
        float positiveHeadroomMin = huge;
        float negativeHeadroomMin = huge;
        float whiteLimitedPixels = 0.0;
        float blackLimitedPixels = 0.0;
        float proxyBelowZeroPixels = 0.0;
        float proxyAboveOnePixels = 0.0;
        uint samples = 0u;

        [loop] for (uint y = y0; y < min(y1, srcH); ++y)
        {
            [loop] for (uint x = x0; x < min(x1, srcW); ++x)
            {
                if (gPassthrough != 0u)
                {
                    const float nativeW = max((float) gExposureSourceWidth, 1.0);
                    const float nativeH = max((float) gExposureSourceHeight, 1.0);
                    const float workFromNativeX = (float) srcW / nativeW;
                    const float workFromNativeY = (float) srcH / nativeH;
                    const float marginX = max(gMvScaleX, 0.0);
                    const float marginY = max(gMvScaleY, 0.0);
                    const uint srMaskW =
                        min(srcW, max(1u, (uint) ceil((960.0 + marginX) * workFromNativeX)));
                    const uint srMaskH =
                        min(srcH, max(1u, (uint) ceil((112.0 + marginY) * workFromNativeY)));
                    const uint nrMaskW = min(srcW, max(1u, (uint) ceil(832.0 + marginX)));
                    const uint nrMaskH = min(srcH, max(1u, (uint) ceil(64.0 + marginY)));
                    const bool inSrWatermark =
                        x < srMaskW && y >= (srcH > srMaskH ? srcH - srMaskH : 0u);
                    const bool inNrWatermark =
                        x < nrMaskW && y >= (srcH > nrMaskH ? srcH - nrMaskH : 0u);
                    if (inSrWatermark || inNrWatermark)
                        continue;
                }

                const int2 p = int2(x, y);
                const float3 proxyRaw = SanitizeFinite3(gSource.Load(int3(p, 0)).rgb, 0.0);
                const float3 modelRaw = SanitizeFinite3(gModel.Load(int3(p, 0)).rgb, proxyRaw);
                const float3 e = modelRaw - proxyRaw;
                if (proxyRaw.x < 0.0 || proxyRaw.y < 0.0 || proxyRaw.z < 0.0)
                    proxyBelowZeroPixels += 1.0;
                if (proxyRaw.x > 1.0 || proxyRaw.y > 1.0 || proxyRaw.z > 1.0)
                    proxyAboveOnePixels += 1.0;

                float pixelLimit = huge;
                bool whiteLimited = false;
                bool blackLimited = false;

                [unroll] for (int ch = 0; ch < 3; ++ch)
                {
                    const float edit = e[ch];
                    float bound = huge;
                    float positiveHeadroom = huge;
                    float negativeHeadroom = huge;

                    if (gTransfer == 0u)
                    {
                        // Nonlinear carrier c=.5+.5*x/(1+abs(x)). K-safe is range-safe only; very high
                        // K can still amplify DLAA errors during the inverse mapping.
                        const float d = clamp(1.0 - 2.0 * margin, 1.0e-4, 0.999999);
                        const float fieldLimit = d / max(1.0 - d, 1.0e-6);
                        const float room = max(0.5 - margin, 0.0);
                        if (edit > 1.0e-12)
                        {
                            bound = fieldLimit / edit;
                            positiveHeadroom = room;
                        }
                        else if (edit < -1.0e-12)
                        {
                            bound = fieldLimit / -edit;
                            negativeHeadroom = room;
                        }
                    }
                    else if (gTransfer == 1u)
                    {
                        const float roomWhite = max((carrierHigh - margin) - carrierNeutral, 0.0);
                        const float roomBlack = max(carrierNeutral - (carrierLow + margin), 0.0);
                        if (edit > 1.0e-12)
                        {
                            bound = roomWhite / edit;
                            positiveHeadroom = roomWhite;
                        }
                        else if (edit < -1.0e-12)
                        {
                            bound = roomBlack / -edit;
                            negativeHeadroom = roomBlack;
                        }
                    }
                    else
                    {
                        // Compressed image anchor: preserve scene structure while reserving headroom.
                        // Raw Proxy is saturated only for the carrier anchor, never for E itself.
                        const float anchor = carrierNeutral + anchorStrength * (saturate(proxyRaw[ch]) - 0.5);
                        const float roomWhite = max((carrierHigh - margin) - anchor, 0.0);
                        const float roomBlack = max(anchor - (carrierLow + margin), 0.0);
                        if (edit > 1.0e-12)
                        {
                            bound = roomWhite / edit;
                            positiveHeadroom = roomWhite;
                            whiteLimited = whiteLimited || roomWhite <= 1.0e-6;
                        }
                        else if (edit < -1.0e-12)
                        {
                            bound = roomBlack / -edit;
                            negativeHeadroom = roomBlack;
                            blackLimited = blackLimited || roomBlack <= 1.0e-6;
                        }
                    }

                    pixelLimit = min(pixelLimit, bound);
                    positiveHeadroomMin = min(positiveHeadroomMin, positiveHeadroom);
                    negativeHeadroomMin = min(negativeHeadroomMin, negativeHeadroom);
                }

                pixelLimit = max(SanitizeFinite(pixelLimit, 0.0), 0.0);
                if (whiteLimited)
                    whiteLimitedPixels += 1.0;
                if (blackLimited)
                    blackLimitedPixels += 1.0;

                if (pixelLimit < strictMin)
                {
                    secondMin = strictMin;
                    strictMin = pixelLimit;
                }
                else if (pixelLimit < secondMin)
                {
                    secondMin = pixelLimit;
                }
                ++samples;
            }
        }

        if (samples < 2u || secondMin >= huge * 0.5)
            secondMin = strictMin;
        gTarget[id.xy] = float4(strictMin, secondMin, positiveHeadroomMin, negativeHeadroomMin);
        gKeep[id.xy] = float4(whiteLimitedPixels, blackLimitedPixels,
                              proxyBelowZeroPixels, proxyAboveOnePixels);
        return;
    }

    if (gMode == 21)
    {
        // Min-reduce K/headroom statistics.
        const uint srcW = max(gGuideWidth, 1u);
        const uint srcH = max(gGuideHeight, 1u);
        const uint x0 = (id.x * srcW) / gWidth;
        const uint x1 = max(((id.x + 1u) * srcW) / gWidth, x0 + 1u);
        const uint y0 = (id.y * srcH) / gHeight;
        const uint y1 = max(((id.y + 1u) * srcH) / gHeight, y0 + 1u);
        float4 limits = 1.0e20;
        [loop] for (uint y = y0; y < min(y1, srcH); ++y)
            [loop] for (uint x = x0; x < min(x1, srcW); ++x)
                limits = min(limits, gSource.Load(int3(x, y, 0)));
        gTarget[id.xy] = limits;
        return;
    }

    if (gMode == 24)
    {
        // Sum-reduce diagnostic pixel counts emitted by mode 20.
        const uint srcW = max(gGuideWidth, 1u);
        const uint srcH = max(gGuideHeight, 1u);
        const uint x0 = (id.x * srcW) / gWidth;
        const uint x1 = max(((id.x + 1u) * srcW) / gWidth, x0 + 1u);
        const uint y0 = (id.y * srcH) / gHeight;
        const uint y1 = max(((id.y + 1u) * srcH) / gHeight, y0 + 1u);
        float4 counts = 0.0;
        [loop] for (uint y = y0; y < min(y1, srcH); ++y)
            [loop] for (uint x = x0; x < min(x1, srcW); ++x)
                counts += gSource.Load(int3(x, y, 0));
        gTarget[id.xy] = counts;
        return;
    }

    if (gMode == 22)
    {
        const float4 stats = gAux2.Load(int3(0, 0, 0));
        const uint gainMode = min(gDebugView, 2u);
        const float strictSafe = max(SanitizeFinite(stats.x, 0.0), 0.0);
        const float robustSafe = max(SanitizeFinite(stats.y, strictSafe), 0.0);
        const float selectedSafe = gainMode == 2u ? robustSafe : strictSafe;
        const float ceiling = max(abs(gResidualConfidenceUnused), 1.0e-6);
        const float manualK = max(abs(gResidualScale), 1.0e-6);
        const float autoTarget = max(min(selectedSafe, ceiling), 1.0e-6);
        const float previousK = max(abs(gTransferStrength), 1.0e-6);
        const float riseMultiplier = max(gColourStrength, 1.0);
        const float K = gainMode == 0u ? manualK : min(autoTarget, previousK * riseMultiplier);
        const float anchorStrength = saturate(gMaxRatio);
        const float2 carrierRange = TemporalCarrierRange();
        const float carrierLow = carrierRange.x;
        const float carrierHigh = carrierRange.y;
        const float carrierNeutral = 0.5 * (carrierLow + carrierHigh);

        const int2 p = int2(id.xy);
        const float3 proxyRaw = SanitizeFinite3(gSource.Load(int3(p, 0)).rgb, 0.0);
        const float3 modelRaw = SanitizeFinite3(gModel.Load(int3(p, 0)).rgb, proxyRaw);
        const float3 e = modelRaw - proxyRaw;
        float3 carrier;
        if (gTransfer == 0u)
            carrier = NrEncodeResizeField(K * e);
        else if (gTransfer == 1u)
            carrier = clamp(carrierNeutral + K * e, carrierLow, carrierHigh);
        else
        {
            const float3 anchor = carrierNeutral + anchorStrength * (saturate(proxyRaw) - 0.5);
            carrier = clamp(anchor + K * e, carrierLow, carrierHigh);
        }

        gTarget[id.xy] = float4(carrier, 1.0);
        if (id.x == 0u && id.y == 0u)
            gKeep[uint2(0, 0)] = float4(K, selectedSafe, stats.z, stats.w);
        return;
    }

    if (gMode == 25)
    {
        const float anchorStrength = saturate(gTransferStrength);
        const float2 carrierRange = TemporalCarrierRange();
        const float carrierNeutral = 0.5 * (carrierRange.x + carrierRange.y);
        const float3 proxyRaw = SanitizeFinite3(gSource.Load(int3(id.xy, 0)).rgb, 0.0);
        const float3 anchor = carrierNeutral + anchorStrength * (saturate(proxyRaw) - 0.5);
        gTarget[id.xy] = float4(anchor, 1.0);
        return;
    }

    if (gMode == 26)
    {
        gTarget[id.xy] = gSource.Load(int3(id.xy, 0));
        return;
    }

    if (gMode == 23)
    {
        const float K = max(abs(gAux2.Load(int3(0, 0, 0)).x), 1.0e-6);
        const float2 carrierRange = TemporalCarrierRange();
        const float carrierNeutral = 0.5 * (carrierRange.x + carrierRange.y);
        const bool defaultRange = TemporalCarrierDefaultRange(carrierRange);
        const int2 p = int2(id.xy);
        const float3 filtered = SanitizeFinite3(gSource.Load(int3(p, 0)).rgb, carrierNeutral.xxx);
        const float3 baseRaw = SanitizeFinite3(gModel.Load(int3(p, 0)).rgb, 0.0);
        // Preserve the legacy [0,1] decode exactly. Any custom diagnostic range intentionally keeps
        // raw DLAA excursions so the test can reveal whether the network itself collapses one side.
        const float3 decodedCarrier = defaultRange ? saturate(filtered) : filtered;
        float3 e;
        if (gTransfer == 0u)
            e = NrDecodeResizeField(filtered) / K;
        else if (gTransfer == 1u)
            e = (decodedCarrier - carrierNeutral) / K;
        else if (gDebugView != 0u)
            e = (filtered - baseRaw) / K; // paired DLAA baseline: cancel the two raw DLAA outputs directly
        else
        {
            const float anchorStrength = saturate(gTransferStrength);
            const float3 anchor = carrierNeutral + anchorStrength * (saturate(baseRaw) - 0.5);
            e = (decodedCarrier - anchor) / K;
        }
        gTarget[id.xy] = float4(SanitizeFinite3(e, 0.0), 1.0);
        return;
    }

    if (gMode == 19)
    {
        // Temporal-residual experiment: preserve the exact RAW-domain residual used by P100-guided,
        // but remap signed values to an LDR-like neutral-0.5 carrier before same-resolution DLAA.
        const int2 p = int2(id.xy);
        const float3 proxyRaw = gSource.Load(int3(p, 0)).rgb;
        const float3 modelRaw = gModel.Load(int3(p, 0)).rgb;
        const float3 residualRaw = SanitizeFinite3(modelRaw - proxyRaw, 0.0);
        gTarget[id.xy] = float4(NrEncodeResizeField(residualRaw), 1.0);
        return;
    }

    // Experimental private-DLSS carrier, not an ordinary colour image. Neutral 0.5 encodes zero;
    // values below it carry darkening. A reversible signed compression avoids clipping negative
    // edits at the DLSS input. Scale small linear-light edits up before storing them in FP16;
    // at unit scale, a dark scene's edits round to neutral before DLSS even sees them.
    if (gMode == 12)
    {
        gTarget[id.xy] = float4(NrEncodeResizeField(NrPairedResizeField(int2(id.xy), int2(gWidth, gHeight))), 1);
        return;
    }
#if !defined(VK_MODE)
    if (gMode == 13)
    {
        // Compatibility fallback for non-FP16 model outputs: preserve the old combined carrier+gate pass.
        const int2 p = int2(id.xy);
        const float4 proxyRaw = gSource.Load(int3(p, 0));
        const float4 answerRaw = gModel.Load(int3(p, 0));
        gTarget[id.xy] = answerRaw;
        gKeep[id.xy] = float4(proxyRaw.rgb, DirectDetailGateAt(p));
        return;
    }
    if (gMode == 15)
    {
        // Normal Direct NR gated-detail path: alpha-only gate, with no redundant NR50 copy.
        gTarget[id.xy] = float4(0.0, 0.0, 0.0, DirectDetailGateAt(int2(id.xy)));
        return;
    }
#endif
    if (gMode == 9)
    {
        float3 source = gSource.Load(int3(id.xy, 0)).rgb;
        float3 answer = gModel.Load(int3(id.xy, 0)).rgb;
        if (gPassthrough == 0) { source = SrgbToLinear(source); answer = SrgbToLinear(answer); }
        float3 d = SanitizeFinite3(answer - source, 0.0);
        gTarget[id.xy] = float4(0.5 + 0.5 * d / (1.0 / 64.0 + abs(d)), 1.0);
        return;
    }
    if (gMode == 10)
    {
        // Mode-local fields: guide active sizes/origins, and motion-to-working-pixel scale.
        uint2 dp = min(uint2(uv * uint2(gGuideWidth, gGuideHeight)),
                       uint2(gGuideWidth, gGuideHeight) - 1) + uint2(gDebugView, gCompareMode);
        uint2 size = uint2(gTransferStrength, gColourStrength);
        uint2 mp = min(uint2(uv * size), size - 1) + uint2(gCompareSwap, gTransfer);
        float z = gSource.Load(int3(dp, 0)).r;
        float2 mv = gModel.Load(int3(mp, 0)).xy * float2(gMvScaleX, gMvScaleY);
        gTarget[id.xy] = float4(isfinite(z) ? z : 0.0, 0, 0, 1);
        gKeep[id.xy] = float4(all(isfinite(mv)) ? mv : float2(0, 0), 0, 1);
        return;
    }
    if (gMode == 5)
    {
        float3 difference = SanitizeFinite3(gModel.Load(int3(id.xy, 0)).rgb -
                                            gSource.Load(int3(id.xy, 0)).rgb, 0.0);
        float3 d = difference / max(gResidualScale, 1e-4);
        gTarget[id.xy] = float4(0.5 + 0.5 * d / (1.0 + abs(d)), 1.0);
        return;
    }
    if (gMode == 6)
    {
        float4 base = gSource.Load(int3(id.xy, 0));
        float3 encoded = SanitizeFinite3(gModel.Load(int3(id.xy, 0)).rgb, 0.5);
        // Limit the inverse near its poles: DLSS can ring outside the carrier's [0,1] range.
        float3 signedEdit = clamp(2.0 * encoded - 1.0, -0.999, 0.999);
        float3 edit = signedEdit / (1.0 - abs(signedEdit)) * max(gResidualScale, 1e-4);
        gTarget[id.xy] = float4(max(SanitizeFinite3(base.rgb + edit, base.rgb), 0.0), base.a);
        return;
    }
    if (gMode == 7)
    {
        gTarget[id.xy] = 1.0;
        return;
    }

    if (gMode == 8)
    {
        // Already encoded: restore the input range without applying the tone curve again.
        float4 raw = gSource.Load(int3(id.xy, 0));
        gTarget[id.xy] = float4(saturate(SanitizeFinite3(raw.rgb, 0.5)), raw.a);
        return;
    }

    if (gMode == 2 || gMode == 32)
    {
        const bool fusedClamp = gMode == 32;
        uint srcW, srcH;
        gSource.GetDimensions(srcW, srcH);

        // Nothing to do when the sizes already agree.
        if (srcW == gWidth && srcH == gHeight)
        {
            gTarget[id.xy] = DownsampleMaybeClampProxy(gSource.Load(int3(id.xy, 0)), fusedClamp);
            return;
        }

        // Exact Output Scaling filters are dispatched outside this shader. This local path handles
        // Area, Bilinear, Point and SSIM Sharp, plus Area fallback when an external filter fails.
        const uint filter = min(gTransfer, 11u);
        const float2 sampleUv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
        if (filter == 1u)
        {
            float4 sampled = gSource.SampleLevel(gLinear, sampleUv, 0);
            const int2 center = int2(clamp(floor(sampleUv * float2(srcW, srcH)), 0.0,
                                           float2(srcW - 1, srcH - 1)));
            sampled.a = DownsampleLoadClamped(center, srcW, srcH).a;
            gTarget[id.xy] = DownsampleMaybeClampProxy(sampled, fusedClamp);
            return;
        }
        if (filter == 4u)
        {
            const int2 center = int2(clamp(floor(sampleUv * float2(srcW, srcH)), 0.0,
                                           float2(srcW - 1, srcH - 1)));
            gTarget[id.xy] = DownsampleMaybeClampProxy(DownsampleLoadClamped(center, srcW, srcH), fusedClamp);
            return;
        }
        if (filter == 11u)
        {
            gTarget[id.xy] = DownsampleMaybeClampProxy(DownsampleSsimSharp(sampleUv, srcW, srcH, gWidth, gHeight), fusedClamp);
            return;
        }

        // Exact area-weighted downsampling avoids the aliasing of a single bilinear tap.
        // Adapted from hhkbble's multi-pass contribution.
        DLSSNR_AREA_COORD float x0 = ((float) id.x * (float) srcW) / (float) gWidth;
        DLSSNR_AREA_COORD float x1 = ((float) (id.x + 1) * (float) srcW) / (float) gWidth;
        DLSSNR_AREA_COORD float y0 = ((float) id.y * (float) srcH) / (float) gHeight;
        DLSSNR_AREA_COORD float y1 = ((float) (id.y + 1) * (float) srcH) / (float) gHeight;
        DLSSNR_AREA_COORD float area = (x1 - x0) * (y1 - y0);

        const int i0 = (int) floor(x0);
        const int i1 = (int) ceil(x1) - 1;
        const int j0 = (int) floor(y0);
        const int j1 = (int) ceil(y1) - 1;

        DLSSNR_AREA_ACC float3 acc = 0.0;

        for (int j = j0; j <= j1; ++j)
        {
            const int jj = clamp(j, 0, (int) srcH - 1);
            const float aY = max(y0, (float) j);
            const float bY = min(y1, (float) j + 1.0);
            const float wy = max(bY - aY, 0.0);

            for (int i = i0; i <= i1; ++i)
            {
                const int ii = clamp(i, 0, (int) srcW - 1);
                const float aX = max(x0, (float) i);
                const float bX = min(x1, (float) i + 1.0);
                acc += gSource.Load(int3(ii, jj, 0)).rgb * (max(bX - aX, 0.0) * wy);
            }
        }

        const int acx = clamp((int) floor(((float) id.x + 0.5) * (float) srcW / (float) gWidth), 0, (int) srcW - 1);
        const int acy = clamp((int) floor(((float) id.y + 0.5) * (float) srcH / (float) gHeight), 0, (int) srcH - 1);

        gTarget[id.xy] = DownsampleMaybeClampProxy(
            float4(acc / area, gSource.Load(int3(acx, acy, 0)).a), fusedClamp);
        return;
    }

    if (gMode == 0)
    {
        float4 source = gSource.Load(int3(id.xy, 0));
        // Keep two domains separate. NVIDIA NR continues to see the same non-negative proxy as before,
        // but linear HDR keeps its signed scRGB/Rec.709 original. Negative components are valid wide-gamut
        // chromaticity after BT.2020/PQ -> linear Rec.709 and must survive until the final BT.2020/PQ encode.
        float3 frame = max(source.rgb, float3(0.0, 0.0, 0.0));
        const float3 originalFrame = gPassthrough != 0 ? frame : source.rgb;

        // This really is the untouched HDR frame now; only the model-facing proxy is clipped to its safe domain.
        gKeep[id.xy] = float4(originalFrame, source.a);

        // Some games hand DLSS a frame that has already been through their tonemapper. The game says
        // which in its own DLSS creation flags, and converting one that needs no conversion is pure
        // damage, so it goes through untouched.
        if (gPassthrough != 0)
        {
            gTarget[id.xy] = float4(frame, source.a);
            return;
        }

        // What the model is shown. Mode 2 -- the default -- scales the frame and encodes it, and that
        // is all: the game is going to tone map this picture later, so tone mapping it here as well
        // shows the model a doubly compressed image. Measured against Cyberpunk's own numbers, the
        // Reinhard proxy handed the model a scene value of 1.0 as 0.55 and 1.5 as 0.64 -- flat, dark,
        // and nothing like the finished frame it was trained on. The model then synthesised weakly,
        // judged tone on a picture that does not exist, and its answer had to be un-crushed on the way
        // back. Mode 0 keeps that old curve, mode 1 the fitted one.
        // A soft knee instead of a hard ceiling. Anything above 0.75 is rolled off rather than
        // clipped, so the model is never shown a field of flat white whose blown pixels flip between
        // frames -- unstable input is unstable output, and this is where a bright scene would produce
        // it. The resolve reproduces this exactly, so the two agree on what the frame's own proxy is.
        // The classic soft knee, or -- when the reversible proxy is on -- the unclipped Neutwo encode
        // that shows the model highlight gradation the knee throws away. Reached only when the frame
        // is not passthrough (handled and returned above), so NeutwoEncode never sees a tone-mapped
        // frame. Both are undone by the resolve: the knee approximately, Neutwo exactly.
        float3 normalized = frame / WhitePoint();
        float3 display;
        if (gReversibleMode == 0)
            display = SoftKnee(normalized);        // soft knee
        else if (gReversibleMode >= 3)
            display = HybridEncode(normalized);    // 3 hybrid composed, 4 hybrid replace -- same curve
        else
            display = NeutwoEncode(normalized);    // 1 composed, 2 replace -- both the full Neutwo proxy

        // The reversible proxy forces opaque alpha -- feature 18 expects an opaque colour input, and
        // the frame's own alpha is not part of what the model reads. The knee path keeps the frame's
        // alpha, so the default stays byte-identical.
        float alpha = gReversibleMode != 0 ? 1.0 : source.a;

        gTarget[id.xy] = float4(LinearToSrgb(display), alpha);
        return;
    }

    // Comparison, decided before anything is read, because side by side changes which part of the
    // frame this pixel is showing rather than just which version of it.
    //
    //   1  side by side  each half carries the whole frame, so both are squeezed horizontally
    //   2  wipe          one frame cut at the split, nothing resampled
    //
    // Neither needs the menu open to stay up. The wipe's split is a setting like any other; the menu
    // is only how you drag it.
    float2 cmpUv = uv;
    bool showOriginal = false;
    bool onDivider = false;
    bool outsideFrame = false;

    if (gCompareMode == 1)
    {
        showOriginal = (uv.x < 0.5) != (gCompareSwap != 0);

        // Each half is half as wide as the frame and just as tall, so the frame cannot fill it and
        // keep its shape. Stretching it to fit is what made both sides look squashed. Fitting it
        // properly leaves the halves letterboxed, which is the honest way round: a comparison that
        // changes the shape of what it is comparing is not showing you the picture.
        //
        // Zoom decides which is given up. At 1 the whole frame is there at its right proportions
        // with bars above and below; at 2 the half is filled and the sides are cropped away.
        float2 half2 = float2(uv.x < 0.5 ? uv.x * 2.0 : (uv.x - 0.5) * 2.0, uv.y) - 0.5;
        cmpUv = float2(0.5 + half2.x / gCompareZoom, 0.5 + half2.y * 2.0 / gCompareZoom);

        outsideFrame = cmpUv.x < 0.0 || cmpUv.x > 1.0 || cmpUv.y < 0.0 || cmpUv.y > 1.0;
        onDivider = abs(uv.x - 0.5) < (1.0 / max(gWidth, 1u));
    }
    else if (gCompareMode == 2)
    {
        showOriginal = (uv.x < gCompareSplit) != (gCompareSwap != 0);
        onDivider = abs(uv.x - gCompareSplit) < (1.0 / max(gWidth, 1u));
    }

    // Sampled rather than loaded: when the model ran at a reduced resolution these are smaller than the
    // frame, and its edit is enlarged here while the frame underneath stays untouched.
    float4 proxySample = gSource.SampleLevel(gLinear, cmpUv, 0);
    float4 modelSample;
#if !defined(VK_MODE)
    if (gTransfer == 7u)
        modelSample = DirectSpatialSample(cmpUv);
    else
#endif
        modelSample = gModel.SampleLevel(gLinear, cmpUv, 0);

    // Nothing was encoded on the way in, so nothing is decoded here either.
    float3 proxy = gPassthrough != 0 ? proxySample.rgb : SrgbToLinear(proxySample.rgb);
    float3 model = gPassthrough != 0 ? modelSample.rgb : SrgbToLinear(modelSample.rgb);

    // The model's own answer, kept before the matched-residual block below can rewrite `model`, so the
    // replace decode uses what the model returned rather than the residual reconstruction.
    float3 modelDirect = model;
    float4 originalSample = gCompareMode == 1 ? gOriginal.SampleLevel(gLinear, cmpUv, 0)
                                              : gOriginal.Load(int3(id.xy, 0));

    // All three pictures have to share a scale before their luminances can be compared. The proxy and
    // the model come back from an sRGB decode, so they sit in 0..1 where 1 is the white point; the
    // frame is raw linear and runs well past that. Comparing them unnormalised is a real bug and it
    // reads exactly like the model has stopped adding detail: with the frame several times larger,
    // the shadow branch never fires, every pixel takes the highlight branch, and the clamp flattens
    // the result to a near-constant scale. Colour still moves, because that comes from the model's
    // own hue, which is what makes the failure so confusing to look at.
    const float normScale = gPassthrough != 0 ? 1.0 : WhitePoint();
    const float3 originalSigned = originalSample.rgb / normScale;
    // Keep legacy/model composition in the same non-negative domain it used before. Wide-gamut information
    // is carried separately and re-attached after the NR edit, so the private model never has to consume
    // negative RGB and existing transfer math does not suddenly start operating on signed values.
    float3 original = gPassthrough != 0 ? originalSigned : max(originalSigned, 0.0);

    float originalLuma = dot(original, kLuma);
    float proxyLuma = dot(proxy, kLuma);

    // Apply the model. Off outputs the frame as the upscaler produced it (clean) while the pass keeps
    // running -- so with Hold frame you can freeze a frame and toggle this to A/B the same frozen frame
    // with and without Neural Rendering. In passthrough the frame is already display-referred.
    if (gApplyModel == 0)
    {
        const float3 cleanFrame = gPassthrough != 0 ? max(originalSample.rgb, 0.0) : originalSample.rgb;
        gTarget[id.xy] = float4(cleanFrame, originalSample.a);
        return;
    }

    if (gDebugView == 5 && gTransfer == 9u && (gDirectResolveFlags & 4u) != 0u)
    {
        const float3 carrierRaw = gAux.SampleLevel(gLinear, cmpUv, 0).rgb;
        // Custom carrier debug maps the selected Min/Max back to [0,1], so its midpoint is always 0.5.
        float3 carrier = saturate(carrierRaw);
        if ((gDirectResolveFlags & 8u) != 0u)
        {
            const float rangeLow = asfloat(gResidualMotionBaseXUnused);
            const float rangeHigh = asfloat(gResidualMotionBaseYUnused);
            const float rangeWidth = rangeHigh - rangeLow;
            if (isfinite(rangeLow) && isfinite(rangeHigh) && rangeWidth > 1.0e-6)
                carrier = saturate((carrierRaw - rangeLow) / rangeWidth);
        }
        gTarget[id.xy] = float4(SrgbToLinear(carrier) * gDebugScale, originalSample.a);
        return;
    }

    if (gDebugView == 6 && gTransfer == 9u)
    {
        const float3 signedResidual = SanitizeFinite3(modelSample.rgb, 0.0);
        const float3 shown = saturate(0.5 + signedResidual * 20.0);
        gTarget[id.xy] = float4(SrgbToLinear(shown) * gDebugScale, originalSample.a);
        return;
    }

    if (gDebugView == 1)
    {
        gTarget[id.xy] = float4(proxy * gDebugScale, originalSample.a);
        return;
    }

    if (gDebugView == 2)
    {
        gTarget[id.xy] = float4(model * gDebugScale, originalSample.a);
        return;
    }

    uint proxyW, proxyH;
    gSource.GetDimensions(proxyW, proxyH);
#if defined(DLSSNR_SUPERIOR_NORMAL)
    const bool modelRanSmall = true; // Actual source dimensions are checked before selecting this PSO.
#else
    const bool modelRanSmall = proxyW != gWidth || proxyH != gHeight;
#endif
    if ((gTransfer == 3 || gTransfer == 4) && (proxyW < gWidth || proxyH < gHeight))
    {
        proxy = gPassthrough != 0 ? saturate(original)
                : (gReversibleMode == 0 ? saturate(SoftKnee(original))
                   : gReversibleMode >= 3 ? HybridEncode(original) : NeutwoEncode(original));
        model = NrReconstructModel(proxy, cmpUv, modelSample.rgb);
        modelDirect = model;
        proxyLuma = dot(proxy, kLuma);
    }

#if !defined(VK_MODE)
    if ((gTransfer == 8u || gTransfer == 9u) && modelRanSmall)
    {
        // Build the exact native proxy the encoder would have shown at P100. It is only a guide/base;
        // the NVIDIA model still ran exclusively at the reduced size.
        const float3 nativeProxyLinear =
            gPassthrough != 0 ? saturate(original)
            : (gReversibleMode == 0u ? saturate(SoftKnee(original))
               : gReversibleMode >= 3u ? HybridEncode(original) : NeutwoEncode(original));
        const float3 nativeProxyRaw =
            gPassthrough != 0 ? nativeProxyLinear : LinearToSrgb(nativeProxyLinear);

        float3 guidedEditRaw = P100GuidedResidualAt(cmpUv, nativeProxyRaw);

        if (gGuideWidth != 0u)
        {
            // Aux2 contains raw-domain low-frequency E50. The gains were fitted in that same domain.
            const float3 lowEditRaw = gAux2.SampleLevel(gLinear, cmpUv, 0).rgb;
            guidedEditRaw = gMvScaleX * guidedEditRaw + (gMvScaleY - gMvScaleX) * lowEditRaw;
        }

#if !defined(DLSSNR_SUPERIOR_SCALING)
        if (gGuideHeight != 0u)
        {
            const float shadowLowRaw = asfloat(gExposureSourceWidth);
            const float shadowHighRaw = asfloat(gExposureSourceHeight);
            const float shadowFloorRaw = asfloat(gExposurePadding);
            const float shadowLow = isfinite(shadowLowRaw) ? shadowLowRaw : 0.02;
            const float shadowHigh = isfinite(shadowHighRaw) ? shadowHighRaw : 0.08;
            const float shadowFloor = isfinite(shadowFloorRaw) ? shadowFloorRaw : 0.15;
            const float lo = min(shadowLow, shadowHigh);
            const float hi = max(max(shadowLow, shadowHigh), lo + 1e-6);
            const float y = max(dot(nativeProxyRaw, kLuma), 0.0);
            const float confidence = lerp(saturate(shadowFloor), 1.0, smoothstep(lo, hi, y));
            guidedEditRaw *= confidence;
        }
#endif

        const float3 modelRaw = nativeProxyRaw + guidedEditRaw;
        proxy = nativeProxyLinear;
        model = gPassthrough != 0 ? modelRaw : SrgbToLinear(modelRaw);
        modelDirect = model;
        proxyLuma = dot(proxy, kLuma);
    }


    // Optional native-P100-guided detail remapping. The entire operation is folded into this resolve:
    // no NR100/P100-detail intermediate is written. Direct uses Aux2 as its selected reconstructed
    // P50 reference; Upscaled NR residual uses Source, which already is reconstructed P100.
    const bool finalDetailExperiment =
        ((gDirectResolveFlags & 1u) != 0u || gTransfer == 6u) &&
        (gResidualHistoryValidUnused != 0u || gResidualMotionBaseXUnused != 0u);
    if (finalDetailExperiment)
    {
        const bool direct = (gDirectResolveFlags & 1u) != 0u;
        const uint structureMode = min(gResidualMotionBaseXUnused, 2u);
        const float3 baselineCenter = proxy;
        const float3 gainCenter =
            direct && structureMode != 0u ? ExperimentGainReferenceAt(cmpUv) : baselineCenter;
        const float3 nativeProxyCenter =
            direct ? baselineCenter : ExperimentNativeProxyAt(cmpUv, normScale);
        float limiterActivity = 0.0;
        const float3 guidedEdit =
            ExperimentP100GuidedEdit(cmpUv, model, baselineCenter, gainCenter, nativeProxyCenter, normScale,
                                     limiterActivity);

        // DirectResolveFlags bit 1 requests a literal intervention mask. White means the resulting-edge
        // cap changed the band strongly; black means the limiter was inactive. Show the mask in the
        // frame's output units so HDR/SDR presentation does not hide it.
        if ((gDirectResolveFlags & 2u) != 0u && gResidualHistoryValidUnused != 0u)
        {
            gTarget[id.xy] = float4(limiterActivity.xxx * normScale, originalSample.a);
            return;
        }

        model = proxy + guidedEdit;
        modelDirect = model;
    }
#endif

    float3 edit = model - proxy;
    if (gTransfer == 2)
    {
        float3 carrier = clamp(2.0 * SanitizeFinite3(modelSample.rgb, 0.5) - 1.0, -0.999, 0.999);
        edit = (1.0 / 64.0) * carrier / (1.0 - abs(carrier));
    }

    // Coring was tried here and removed: the per-frame churn's amplitude overlaps the real detail's,
    // so an amplitude threshold cannot separate them -- it only relocated the noise to the threshold.

    if (gDebugView == 3)
    {
        // Amplified and centred on grey, so both directions of the edit are visible at once.
        float3 shown = saturate(0.5 + edit * 20.0);
        gTarget[id.xy] = float4(SrgbToLinear(shown) * gDebugScale, originalSample.a);
        return;
    }

    // Composition uses the current model answer; temporal residual accumulation is a separate pass.

    // Rebuild the full-resolution proxy and add only the upsampled model difference.
    // Skip ordinary matched residual at native resolution to preserve Classic's exact arithmetic.
    // Residual transfer and cube scaling are adapted from hhkbble's multi-pass contribution.
    if ((gTransfer == 1 && modelRanSmall) || gTransfer == 2 || gTransfer == 6 || gTransfer == 8)
    {
        // Match the encode's curve, passthrough and saturation before cube-scaling the residual.
        // An out-of-range reconstructed proxy would collapse the residual scale to zero.
        float3 fullProxy = gPassthrough != 0
                               ? saturate(original)
                               : (gReversibleMode == 0   ? saturate(SoftKnee(original))
                                  : gReversibleMode >= 3 ? HybridEncode(original)
                                                         : NeutwoEncode(original));
        proxy = fullProxy;
        proxyLuma = dot(proxy, kLuma);

        // At the same rate there is no residual to carry: the model's own picture is already at the
        // frame's resolution, and P + (m - p) collapses to m exactly.
        model = CubeScaleResidual(fullProxy, fullProxy + edit);
        if (gTransfer == 2 || gTransfer == 6 || gTransfer == 8) modelDirect = model;
    }

    // Rescale the model answer to the original luminance and restore headroom lost by the proxy.
    float modelLuma = dot(model, kLuma);
    float3 upgraded;

    if (modelLuma <= 1e-5)
    {
        // The model can return an empty frame for an input it cannot read. Rescaling that collapses
        // the picture to black, so the frame is handed back untouched instead.
        upgraded = original;
    }
    else
    {
        float ratio;

        if (originalLuma < proxyLuma)
        {
            // Below what the proxy showed: the frame's own luminance is the target.
            ratio = originalLuma / max(proxyLuma, 1e-6);
        }
        else
        {
            // Above it, the difference is headroom the proxy could not represent -- brightness the
            // frame really has and the model never saw. It is handed back on top of the model's own
            // answer rather than scaled away, which is what kept highlights from being muted.
            ratio = (modelLuma + max(0.0, originalLuma - proxyLuma)) / modelLuma;
        }

        // Keep the RGB blend within [0,1]; strength above 1 amplifies the bounded luminance ratio below.
        upgraded = lerp(original, HueOkLab(model * ratio, model), saturate(gTransferStrength));
    }

    // Detail strength decides how much of the model's picture is reached at all; colour strength
    // decides whether its colour comes with it. At 0 the frame keeps the game's own hue exactly and
    // only its light carries the model's verdict; at 1 the model's colour arrives as well.
    float upgradedLuma = dot(upgraded, kLuma);

    // A common luminance floor suppresses unstable ratios in near-black pixels.
    const float kRatioFloor = 1.0 / 512.0;
    float lumaRatio = (upgradedLuma + kRatioFloor) / (originalLuma + kRatioFloor);

    // Amplify detail through a luminance-ratio power while preserving neutral edits.
    const float amplified = pow(max(lumaRatio, 1e-6), 1.0 + max(gTransferStrength - 1.0, 0.0));

    // Independent one-sided guards. Highlight guard only limits brightening. Darkening guard only
    // raises the lower luminance-ratio bound; at 100% its floor is zero, so darkening is uncapped.
    const float highlightGuard = max(gMaxRatio, 1.0);
    const float darkeningFloor = 1.0 - saturate(gMaxDarkening / 100.0);
    float boundedRatio = clamp(amplified, darkeningFloor, highlightGuard);

    // Exactly one while the ratio is already inside the guard, so a frame that never needed bounding
    // is untouched rather than rounded, and strength zero stays bit-identical.
    upgraded *= boundedRatio / max(lumaRatio, 1e-6);

    // Both blend endpoints obey the luminance guard. Above colour strength 1, boost OkLab chroma
    // while preserving lightness/hue, then compress out-of-gamut colours toward neutral.
    float3 result = lerp(original * boundedRatio, upgraded, min(gColourStrength, 1.0));

    if (gColourStrength > 1.0)
        result = ClampAp1(FromOkLab(float3(1.0, gColourStrength, gColourStrength) * ToOkLab(max(result, 0.0))));

    // Replace modes decode the model answer directly; passthrough colour needs no inverse transform.
    if (gReversibleMode == 2)
        result = gPassthrough != 0 ? modelDirect : NeutwoDecode(modelDirect);
    else if (gReversibleMode == 4)
        result = gPassthrough != 0 ? modelDirect : HybridDecode(modelDirect);

    // Restore native luminance detail with a bounded, positive ratio (including near-black edges).
    if ((gReversibleMode == 2 || gReversibleMode == 4) && gModelWorkScale > 0.0 &&
        gModelWorkScale < 0.999 && gReplaceDetailStrength > 0.0)
    {
        float2 tap = clamp(round(1.0 / gModelWorkScale), 1.0, 4.0) / float2(gWidth, gHeight);
        float3 neighbours = gOriginal.SampleLevel(gLinear, cmpUv + float2(tap.x, 0), 0).rgb +
                            gOriginal.SampleLevel(gLinear, cmpUv - float2(tap.x, 0), 0).rgb +
                            gOriginal.SampleLevel(gLinear, cmpUv + float2(0, tap.y), 0).rgb +
                            gOriginal.SampleLevel(gLinear, cmpUv - float2(0, tap.y), 0).rgb;
        float blur = dot(original + neighbours / normScale, kLuma) / 5.0;
        float contrast = (originalLuma - blur) / (max(originalLuma, blur) + kRatioFloor);
        result *= exp2(clamp(gReplaceDetailStrength, 0.0, 2.0) * clamp(contrast, -1.0, 1.0));
    }

#if !defined(VK_MODE)
    // Direct NR detail recovery. gAux2 is the selected full-resolution reconstruction of P50.
    // Full-lost-detail needs no gate texture at all. NR-gated mode samples only gAux.a.
    if (gDirectDetailMode != 0)
    {
        const float4 p50Reference = gAux2.SampleLevel(gLinear, cmpUv, 0);
        const float3 p50 = gPassthrough != 0 ? p50Reference.rgb : SrgbToLinear(p50Reference.rgb);
        const float p100Y = max(dot(proxy, kLuma), 0.0);
        const float p50Y = max(dot(p50, kLuma), 0.0);
        float lostStops = log2((p100Y + kRatioFloor) / (p50Y + kRatioFloor));
        if (!isfinite(lostStops))
            lostStops = 0.0;

        float gate = 1.0;
        if (gDirectDetailMode == 2u)
        {
            const float4 detailInfo = gAux.SampleLevel(gLinear, cmpUv, 0);
            gate = lerp(1.0, saturate(detailInfo.a), saturate(gDirectDetailMaskStrength));
        }
        const float detailRatio = exp2(lostStops * gate);
        if (isfinite(detailRatio) && detailRatio > 0.0)
            result *= detailRatio;
    }
#endif

    // Back out of the normalised space the composition worked in.
    result *= normScale;

    if (gSkinProtection != 0)
    {
        // Classify the untouched frame, never NR's recoloured output. Controls
        // attenuate the final edit, including replace mode and all model passes.
        float3 displayRgb = gPassthrough != 0 ? original : LinearToSrgb(saturate(original));
        float mask = SkinColourWeight(displayRgb);
        float detail = lerp(gEnvironmentDetail, gSkinDetail, mask);
        float colour = lerp(gEnvironmentColour, gSkinColour, mask);
        float baseY = dot(max(originalSample.rgb, 0.0), kLuma);
        float editedY = dot(max(result, 0.0), kLuma);
        float wantedY = lerp(baseY, editedY, detail);
        float3 baseChroma = originalSample.rgb / max(baseY, 1e-6);
        float3 editedChroma = result / max(editedY, 1e-6);
        // Exact endpoints avoid changing the default image or fully protected pixels.
        if (detail == 0.0 && colour == 0.0)
            result = original * normScale;
        else if (detail != 1.0 || colour != 1.0)
            result = ClampAp1(lerp(baseChroma, editedChroma, colour) * wantedY);
        if (gShowSkinMask != 0)
            result = mask.xxx * normScale;
    }

    // The stabilized ratio above deliberately adds kRatioFloor, which is useful for model composition
    // but can hide large real percentage losses in very dark pixels. Enforce the darkening control
    // once more on the final edited luminance without that stabilization term. This makes 0% mean
    // exactly "no luminance darkening" and N% cap the final reduction to N%, including deep shadows,
    // replace modes and colour/skin composition. 100% remains a no-op.
    const float finalDarkeningFloor = 1.0 - saturate(gMaxDarkening / 100.0);
    if (finalDarkeningFloor > 0.0 && gShowSkinMask == 0)
    {
        const float baseY = dot(max(originalSample.rgb, 0.0), kLuma);
        const float editedY = dot(max(result, 0.0), kLuma);
        const float minimumY = baseY * finalDarkeningFloor;
        if (editedY < minimumY)
        {
            if (editedY > 1e-6)
                result *= minimumY / editedY;
            else if (baseY > 1e-6)
                result = max(originalSample.rgb, 0.0) * finalDarkeningFloor;
        }
    }

    // In linear HDR, any negative Rec.709 component means the original chromaticity lies outside
    // the Rec.709 triangle (typically P3/BT.2020 content). NVIDIA NR deliberately sees a non-negative
    // proxy, so letting its RGB answer replace chromaticity here would collapse those colours toward
    // Rec.709. For wide-gamut pixels preserve the exact original chromaticity and transfer only NR's
    // final luminance verdict. In-gamut pixels keep the ordinary NR colour composition unchanged.
    if (gPassthrough == 0u && gShowSkinMask == 0u &&
        any(originalSample.rgb < float3(0.0, 0.0, 0.0)))
    {
        const float originalY = dot(originalSample.rgb, kLuma);
        const float editedY = dot(result, kLuma);
        if (isfinite(originalY) && isfinite(editedY) && originalY > 1.0e-6 && editedY >= 0.0)
        {
            const float luminanceScale = editedY / originalY;
            if (isfinite(luminanceScale) && luminanceScale >= 0.0)
                result = originalSample.rgb * luminanceScale;
        }
        else
        {
            // Degenerate wide-gamut pixels are safer untouched than projected into the model's Rec.709 proxy.
            result = originalSample.rgb;
        }
    }

    // The side being shown untouched takes the frame as it arrived, past every step above.
    if (showOriginal)
        result = originalSample.rgb;

    // The letterbox. The sampler clamps rather than wrapping, so without this the bars would be the
    // frame's edge row smeared down the screen.
    if (outsideFrame)
        result = float3(0.0, 0.0, 0.0);

    // A hairline so the two sides are never mistaken for one picture.
    if (onDivider)
        result = float3(WhitePoint(), WhitePoint(), WhitePoint());

    // SDR/display-referred output keeps the historical non-negative contract. Linear HDR must stay signed:
    // the finished-picture path converts this linear Rec.709/scRGB signal back to BT.2020/PQ afterwards.
    const float3 outputResult = gPassthrough != 0u
        ? max(result, float3(0.0, 0.0, 0.0))
        : SanitizeFinite3(result, originalSample.rgb);
    gTarget[id.xy] = float4(outputResult, originalSample.a);
}
