// v59's radius-one estimator, folded into janblade's existing final composition.
// t0: working proxy, t1: decoded signed DLSS residual (or raw NR with GuideHeight=1), t5: Low64 field.
// No exposure, colour encoding or tone-mapping logic belongs in this include.
Texture2D<float4> gSuperiorLow : register(t5);
#if defined(DLSSNR_SUPERIOR_HOST_PACKED)
#include "WarpMap.hlsli"
#if defined(DLSSNR_SUPERIOR_HOST_CACHED)
static uint2 gSuperiorPixel;
#endif
float2 SuperiorPackedUv(float2 uv)
{
#if defined(DLSSNR_SUPERIOR_HOST_CACHED)
    return float2(gMotion.Load(int3(gSuperiorPixel.x, 1, 0)).x,
                  gMotion.Load(int3(gSuperiorPixel.y, 2, 0)).x);
#else
    return SuperiorArithmeticPackedUv(uv);
#endif
}
#endif
float3 SuperiorShapeEdit(float3 edit, float2 uvq)
{
    return gGuideWidth != 0u
        ? gMvScaleX * edit + (gMvScaleY - gMvScaleX) * gSuperiorLow.SampleLevel(gLinear, uvq, 0).rgb
        : edit;
}
float3 SuperiorGuidedEdit(float2 uvq, float3 nativeGuide)
{
    // Non-temporal fallback (e.g. an above-native ordinary grid with reduced packed NR).
    // Packed temporal reconstruction instead uses the direct warped query below.
    // Non-finite NR or proxy values (an Inf from a pass) must not reach the frame. SanitizeFinite3 returns finite
    // values unchanged, so finite results are bit-identical.
    if (gGuideHeight != 0u)
        return SuperiorShapeEdit(SanitizeFinite3(gModel.SampleLevel(gLinear, uvq, 0).rgb -
                                                 gSource.SampleLevel(gLinear, uvq, 0).rgb, 0.0), uvq);
    uint srcW, srcH;
    gSource.GetDimensions(srcW, srcH);
#if defined(DLSSNR_SUPERIOR_HOST_PACKED)
    // Map the native query directly to the packed lattice; no bilinear intermediate images.
    uvq = SuperiorPackedUv(uvq);
#endif
    const float3 fallback = SanitizeFinite3(gModel.SampleLevel(gLinear, saturate(uvq), 0).rgb, 0.0);
    const float spatialSigma = max(abs(3.0), 1e-4);
    // CPU computes the cubic Model-resolution policy once per frame. Exposure
    // meter dimensions are unused by this final Superior composition stage.
    const float suppliedRange2 = asfloat(gExposureSourceWidth);
    const float invRange2 = isfinite(suppliedRange2) && suppliedRange2 > 0.0
        ? suppliedRange2 : 1.0 / (0.015 * 0.015);
    const float invSpatial2 = 1.0 / (spatialSigma * spatialSigma);
    const float gaussianExp2 = 0.7213475204444817;
    const float2 sourcePos = uvq * float2(srcW, srcH) - 0.5;
    const int2 base = int2(floor(sourcePos + 0.5));
    float3 weighted = 0.0;
    float weightSum = 0.0;
#if defined(DLSSNR_SUPERIOR_HOST_SDR)
    [loop]
#else
    [unroll]
#endif
    for (int oy = -1; oy <= 1; ++oy)
    {
#if defined(DLSSNR_SUPERIOR_HOST_SDR)
        [loop]
#else
        [unroll]
#endif
        for (int ox = -1; ox <= 1; ++ox)
        {
            const int2 p = clamp(base + int2(ox, oy), int2(0, 0), int2((int)srcW - 1, (int)srcH - 1));
            // A non-finite proxy tap gets ~zero range weight, a non-finite residual tap contributes nothing.
            const float3 proxy = SanitizeFinite3(gSource.Load(int3(p, 0)).rgb, 1e4);
            const float3 residual = SanitizeFinite3(gModel.Load(int3(p, 0)).rgb, 0.0);
            const float3 colourDelta = proxy - nativeGuide;
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
    float3 edit = weightSum > 1e-8 ? weighted / weightSum : fallback;
    return SuperiorShapeEdit(edit, uvq);
}
