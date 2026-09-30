// HDR10 <-> linear Rec.709 in scRGB units (1 = 80 nits).
// Separate from the existing NR shader so SDR and ordinary NR keep their compiled code.
cbuffer Params : register(b0)
{
    uint mode; float exposureScale; uint width; uint height;
    float sceneIsLinear; float unusedColour; uint unusedDebug; float maxRatio;
};
Texture2D<float4> source : register(t0);
Texture2D<float4> reference : register(t1);
Texture2D<float4> original : register(t2);
RWTexture2D<float4> target : register(u0);

#include "dlssnr_pq.hlsli"
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;
    float4 pixel = source.Load(int3(id.xy, 0));
    const float3x3 to709 = kBt2020To709;
    const float3x3 to2020 = kBt709To2020;
    if (mode == 5)
    {
        // Carry a bounded relative change through DLSS. An absolute signed residual
        // around 0.5 loses small dark-scene edits when stored in FP16; dividing that
        // quantization error by the dark SR reference produces large colour noise.
        float3 base = max(pixel.rgb, 0.0);
        float3 edited = max(reference.Load(int3(id.xy, 0)).rgb, 0.0);
        if (!all(isfinite(base)) || !all(isfinite(edited)))
        { target[id.xy] = float4(0.5, 0.5, 0.5, 1.0); return; }
        if (sceneIsLinear < 0.5)
        { base = pow(base, 2.2); edited = pow(edited, 2.2); }
        float floorValue = max(max(base.r, max(base.g, base.b)) * 0.02,
                               max(exposureScale, 1e-4) * 1e-4);
        float limit = clamp(maxRatio, 1.0, 8.0);
        float3 gain = clamp(1.0 + (edited - base) / max(base, floorValue), 1.0 / limit, limit);
        target[id.xy] = float4(0.5 + log2(gain) / 8.0, 1.0);
        return;
    }
    if (mode >= 2)
    {
        // t0 = finished picture, t2 = log-gain carrier upscaled by private DLSS.
        // Apply bounded relative changes in linear light. This approximates the game's
        // unknown tonemapper and colour grading; no scene-linear delta is added to display code.
        float3 carrier = original.Load(int3(id.xy, 0)).rgb;
        if (!all(isfinite(carrier)) || all(carrier == 0.5))
        { target[id.xy] = pixel; return; }
        float limit = clamp(maxRatio, 1.0, 8.0);
        float3 gain = exp2(clamp((carrier - 0.5) * 8.0, -log2(limit), log2(limit)));
        float3 light = mode == 2 ? pow(max(pixel.rgb, 0.0), 2.2) :
                       mode == 4 ? mul(to709, DecodePQ(pixel.rgb)) : pixel.rgb;
        light *= gain;
        float3 result = mode == 2 ? pow(saturate(light), 1.0 / 2.2) :
                        mode == 4 ? EncodePQ(mul(to2020, light)) : light;
        target[id.xy] = float4(all(isfinite(result)) ? result : pixel.rgb, pixel.a);
        return;
    }
    if (mode == 0)
    {
        // Negative components carry wide-gamut colours; retain them in FP16.
        target[id.xy] = float4(mul(to709, DecodePQ(pixel.rgb)), pixel.a);
    }
    else
    {
        float4 base = original.Load(int3(id.xy, 0));
        float3 result = EncodePQ(mul(to2020, pixel.rgb));
        target[id.xy] = float4(all(isfinite(result)) ? result : base.rgb, base.a);
    }
}
