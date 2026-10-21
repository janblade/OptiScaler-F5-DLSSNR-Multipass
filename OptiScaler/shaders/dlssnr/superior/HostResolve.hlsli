// v59's radius-one estimator, folded into janblade's existing final composition.
// t0: working proxy, t1: decoded signed DLSS residual, t5: Low64 field.
// No exposure, colour encoding or tone-mapping logic belongs in this include.
Texture2D<float4> gSuperiorLow : register(t5);
float3 SuperiorGuidedEdit(float2 uvq, float3 nativeGuide)
{
    uint srcW, srcH;
    gSource.GetDimensions(srcW, srcH);
    const float3 fallback = gModel.SampleLevel(gLinear, saturate(uvq), 0).rgb;
    const float rangeSigma = max(abs(0.015), 1e-5);
    const float spatialSigma = max(abs(3.0), 1e-4);
    const float invRange2 = 1.0 / (rangeSigma * rangeSigma);
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
            const float3 proxy = gSource.Load(int3(p, 0)).rgb;
            const float3 residual = gModel.Load(int3(p, 0)).rgb;
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
    if (gGuideWidth != 0u)
        edit = gMvScaleX * edit + (gMvScaleY - gMvScaleX) * gSuperiorLow.SampleLevel(gLinear, uvq, 0).rgb;
    return edit;
}
