// Private DLAA only: identical uniform joint reconstruction and fixed carrier,
// with no materialized raw-motion texture between the two stages.
#include "JointGuides.hlsl"
Texture2D<float4> gPrivateAnswer : register(t3);
RWTexture2D<float4> gPrivateCarrier : register(u2);

void PrivateGuideAt(uint2 id, out float depth, out float2 raw)
{
    const float2 nativePixel = (float2(id) + 0.5) * (gNativeWorkSize.xy / float2(gWidth, gHeight));
    float2 query, gradient;
    bool validDepth;
    depth = JointDepth(nativePixel, query, gradient, validDepth);
    const int2 lo = int2(gMotionRect.xy), hi = int2(gMotionRect.xy + gMotionRect.zw) - 1;
    const int2 mp = clamp(int2(gMotionRect.xy + (nativePixel / gNativeWorkSize.xy) * gMotionRect.zw), lo, hi);
    raw = SuperiorSurfaceMotion(nativePixel, mp, query, validDepth ? depth : asfloat(0x7fc00000u));
    depth = validDepth && isfinite(depth) ? depth : (gUnused > 0.5 ? 0.0 : 1.0);
    raw = all(isfinite(raw)) ? raw : 0.0;
}
float3 PrivateFinite(float3 value, float3 fallback)
{
    return float3(isfinite(value.x) ? value.x : fallback.x, isfinite(value.y) ? value.y : fallback.y,
                  isfinite(value.z) ? value.z : fallback.z);
}
[numthreads(8, 8, 1)] void PrivateGuidesMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;
    float depth;
    float2 raw;
    PrivateGuideAt(id.xy, depth, raw);
    gTarget0[id.xy] = depth;
    // Preserve the carrier's point coordinate calculation too. Normally mp is
    // id; the fallback keeps the same result even for an unusual floating-point phase.
    const uint2 size = uint2(gWidth, gHeight);
    const float2 uv = (float2(id.xy) + 0.5) / float2(size);
    const uint2 mp = min(uint2(uv * size), size - 1);
    if (any(mp != id.xy))
    {
        float ignored;
        PrivateGuideAt(mp, ignored, raw);
    }
    const float2 mv = raw * float2(gPadding0.y, gPadding1.x);
    gTarget1[id.xy] = all(isfinite(mv)) ? mv : 0.0;
    const float3 proxy = PrivateFinite(gSource0.Load(int3(id.xy, 0)).rgb, 0.0);
    const float3 model = PrivateFinite(gPrivateAnswer.Load(int3(id.xy, 0)).rgb, proxy);
    precise float3 residual = model - proxy;
    // Exact same C1 1.5x soft-shoulder carrier as PrivateCarrier.hlsl::Carrier so PrivateDecode matches 1:1.
    const float3 a = abs(residual);
    const float3 lin = 1.5 * a;
    const float3 u = max(a - 0.20, 0.0);
    const float3 ku = 1.5 * u;
    const float3 shoulder = 0.30 + 0.20 * (ku / (0.20 + ku));
    const float3 mag = lerp(lin, shoulder, step(0.20, a));
    gPrivateCarrier[id.xy] = float4(clamp(0.5 + sign(residual) * mag, 0.0, 1.0), 1.0);
}
