// Direct HDR10 write for a render target without UAV support. No original SRV:
// RGB-only writes preserve alpha; discard retains the original invalid-result pixel.
Texture2D<float4> source : register(t0);
#include "dlssnr_pq.hlsli"

float4 VSMain(uint vertex : SV_VertexID) : SV_Position
{
    float2 position = float2((vertex << 1) & 2, vertex & 2);
    return float4(position * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PSMain(float4 position : SV_Position) : SV_Target0
{
    float3 light = source.Load(int3(int2(position.xy), 0)).rgb;
    float3 result = EncodePQ(mul(kBt709To2020, light));
    if (!all(isfinite(result))) discard;
    return float4(result, 1);
}
