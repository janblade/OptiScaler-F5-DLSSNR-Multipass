// Private DLSS uses the unit-gain signed NR edit with a 0.5 neutral point.
#define gModel gUnusedPrivateModel
#define gAux2 gUnusedPrivateAux
#include "precompile/dlssnr.hlsl"
#undef gModel
#undef gAux2
#include "GhostDepth.hlsli"

float3 CurrentEdit(int2 pixel)
{
    int3 p = int3(clamp(pixel, int2(0, 0), int2(gWidth, gHeight) - 1), 0);
    float3 proxy = SanitizeFinite3(gOriginal.Load(p).rgb, 0);
    return SanitizeFinite3(gAux.Load(p).rgb, proxy) - proxy;
}

float MaxChannel(float3 rgb) { return max(rgb.r, max(rgb.g, rgb.b)); }
float3 Carrier(float3 edit) { return clamp(.5 + edit, 0, 65504); }

#ifdef SUPERIOR_GHOST_DECODE
Texture2D<float4> gGhostContrast : register(t1);
Texture2D<float> Visibility : register(t5);
float4 LoadOccluder(int2 pixel)
{
    uint w, h;
    gGhostContrast.GetDimensions(w, h);
    if (w == 0 || h == 0)
        return 0;
    float2 pos = (float2(pixel) + .5) * float2(w, h) / float2(gWidth, gHeight) - .5;
    int2 nearest = clamp(int2(round(pos)), int2(0, 0), int2(w, h) - 1);
    float4 reference = gGhostContrast.Load(int3(nearest, 0));
    if (reference.a <= 0)
    {
        // Follow the strongest valid footprint contributor at a soft edge.
        // Never average opposite RGB references into an artificial low-contrast colour.
        int2 base = int2(floor(pos));
        float2 f = frac(pos);
        float best = 0;
        [unroll] for (int y = 0; y < 2; ++y)[unroll] for (int x = 0; x < 2; ++x)
        {
            int2 q = clamp(base + int2(x, y), int2(0, 0), int2(w, h) - 1);
            float4 candidate = gGhostContrast.Load(int3(q, 0));
            float weight = (x ? f.x : 1 - f.x) * (y ? f.y : 1 - f.y);
            if (candidate.a > 0 && weight > best)
            {
                reference = candidate;
                best = weight;
            }
        }
    }
    return reference;
}

// One colour-consistent history cue and one local cue for trails with multiple old poses.
// The same local range can attenuate a history match that current neighbours explain.
float GhostWeight(int2 pixel, float3 raw, float3 filtered)
{
    float4 reference = LoadOccluder(pixel);
    if (!all(isfinite(reference)) || reference.a <= 0)
        return 1;

    float3 current = Carrier(raw);
    float3 residual = filtered - current;
    float3 precision = max(1.0 / 1024.0, abs(current) / 1024.0);
    if (!all(isfinite(residual)))
        return 1;
    if (all(abs(residual) <= precision))
        return 0;

    float3 direction = Carrier(reference.rgb / reference.a) - current;
    float d2 = dot(direction, direction);
    float v2 = dot(residual, residual);
    float projection = dot(residual, direction);
    float history = 1;
    if (d2 > 1e-8)
    {
        float fraction = max(0, projection) / d2;
        float alignment = projection > 0 ? projection * projection / max(d2 * v2, 1e-20) : 0;
        history = smoothstep(.18, .23, fraction) * smoothstep(.64, .81, alignment);
    }
    if (!isfinite(history))
        return 1;

    float scale = .02 + MaxChannel(abs(current - .5));
    if (history <= 0 && MaxChannel(abs(residual)) <= .18 * scale)
        return 0;
    float3 low = current, high = current;
    int radius = int(clamp(round(6.0 * gHeight / 1404.0), 2.0, 12.0));
    [unroll] for (int y = -1; y <= 1; y += 2)[unroll] for (int x = -1; x <= 1; x += 2)
    {
        float3 sample = Carrier(CurrentEdit(pixel + int2(x, y) * radius));
        low = min(low, sample);
        high = max(high, sample);
    }
    float3 outside = max(filtered - high, low - filtered);
    if (all(outside <= precision))
        return 0;
    scale = .02 + MaxChannel(max(abs(low - .5), abs(high - .5)));
    float score = MaxChannel(outside) / scale;
    return max(history * smoothstep(0, .025, score), smoothstep(.18, .20, score));
}
#endif

[numthreads(8, 8, 1)] void PrivateDecode(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;
    int3 p = int3(id.xy, 0);
    float3 encoded = gSource.Load(p).rgb;
    float3 filtered = SanitizeFinite3(encoded, .5) - .5;
#ifdef SUPERIOR_GHOST_DECODE
    float field = Visibility.SampleLevel(gLinear, (float2(id.xy) + .5) / float2(gWidth, gHeight), 0);
    float mask = isfinite(field) ? saturate(field) : 0;
    if (mask > 0 && GhostNearDepth(gMotion.Load(p).x, gPassthrough) > 0)
    {
        float3 raw = CurrentEdit(p.xy);
        mask *= GhostWeight(p.xy, raw, encoded);
        gTarget[id.xy] = float4(mask >= 1 ? raw : lerp(filtered, raw, mask), mask);
        return;
    }
    gTarget[id.xy] = float4(filtered, 0);
#else
    float rawWeight = asfloat(gExposurePadding);
    float3 raw = CurrentEdit(p.xy);
    gTarget[id.xy] = float4(rawWeight >= 1 ? raw : lerp(filtered, raw, rawWeight), 1);
#endif
}

    [numthreads(8, 8, 1)] void PrivateEncode(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;
    int3 p = int3(id.xy, 0);
    float3 proxy = SanitizeFinite3(gSource.Load(p).rgb, 0);
    // t1 holds the NR answer in the encoding specialization.
    float3 answer = SanitizeFinite3(gUnusedPrivateModel.Load(p).rgb, proxy);
    precise float3 edit = answer - proxy;
    gTarget[id.xy] = float4(Carrier(edit), 1);
    float2 uv = (float2(id.xy) + .5) / float2(gWidth, gHeight);
    uint2 extent = uint2(gTransferStrength, gColourStrength);
    uint2 source = min(uint2(uv * extent), extent - 1) + uint2(gCompareSwap, gTransfer);
    float2 mv = gOriginal.Load(int3(source, 0)).xy * float2(gMvScaleX, gMvScaleY);
    gKeep[id.xy] = float4(all(isfinite(mv)) ? mv : float2(0, 0), 0, 1);
}
