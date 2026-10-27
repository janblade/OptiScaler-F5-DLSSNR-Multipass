// Same conversion and FP16 NR input as Finished Picture's mode 1. Fuse the final
// conversion with the write to UAV-capable pictures; no intermediate PQ copy.
cbuffer Params : register(b0)
{
    uint mode; float exposureScale; uint width; uint height;
    float sceneIsLinear; float unusedColour; uint unusedDebug; float maxRatio;
};
Texture2D<float4> source : register(t0);
RWTexture2D<float4> target : register(u0);
#include "dlssnr_pq.hlsli"
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;
    float4 pixel = source.Load(int3(id.xy, 0));
    float4 base = target[id.xy];
    float3 result = EncodePQ(mul(kBt709To2020, pixel.rgb));
    target[id.xy] = float4(all(isfinite(result)) ? result : base.rgb, base.a);
}
