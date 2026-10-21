// Independent scalar reference matching external/peripheral_warp/SpatialWarp.h's CPU specification.
#if defined(DLSSNR_SUPERIOR_HOST_PACKED)
float PackedReferenceAxis(float pixel, bool vertical)
{
    const uint c = vertical ? 1u : 0u;
    const float4 sizes = gMotion.Load(int3(0, 0, 0));
    const float4 centers = gMotion.Load(int3(4, 0, 0));
    const float delta = pixel - centers[c];
    const bool positive = delta >= 0.0;
    const float4 side = gMotion.Load(int3(positive ? 6 : 5, 0, 0));
    const float4 work = gMotion.Load(int3(positive ? 8 : 7, 0, 0));
    const float4 edges = gMotion.Load(int3(9, 0, 0));
    const float scale = gMotion.Load(int3(10, 0, 0))[c];
    const float span = side[c], center = side[c+2], compression = work[c+2];
    float radius = abs(delta) / span;
    if (work[c] < 1.0 && center < 1.0 && radius > center)
    {
        if (radius > 1.0)
            radius = work[c] + (radius - 1.0) * edges[c + (positive ? 2u : 0u)];
        else
        {
            const float t = (radius - center) / (1.0 - center);
            radius = center + (work[c] - center) * t / (compression + (1.0 - compression) * t);
        }
    }
    precise float result = centers[c+2] + (positive ? 1.0 : -1.0) * radius * span * scale;
    return result;
}
float2 PackedReferenceUv(float2 uv)
{
    const float4 sizes = gMotion.Load(int3(0, 0, 0));
    return float2(PackedReferenceAxis(uv.x * gWidth, false), PackedReferenceAxis(uv.y * gHeight, true)) / sizes.zw;
}
float2 SuperiorPackedUv(float2 uv) { return PackedReferenceUv(uv); }
#endif
