// Private DLSS uses the unit-gain signed NR edit with a 0.5 neutral point.
#define gModel gUnusedPrivateModel
#define gAux2 gUnusedPrivateAux
#include "precompile/dlssnr.hlsl"
#undef gModel
#undef gAux2
#if defined(VK_PRIVATE_TARGETS)
// Vulkan: the frozen file's two storage targets (renamed away by the wrapper, unused here) are declared again with the
// formats of the images bound to them: the carrier in RGBA16F, the second target (the exact motion, the feather rings) in
// RGBA32F.
#undef gTarget
#undef gKeep
[[vk::image_format("rgba16f")]] [[vk::binding(5, 0)]] RWTexture2D<float4> gTarget : register(u0);
[[vk::image_format("rgba32f")]] [[vk::binding(6, 0)]] RWTexture2D<float4> gKeep : register(u1);
#endif
#include "GhostDepth.hlsli"
#ifdef SUPERIOR_PRIVATE_ENCODE_PACKED
#include "WarpMap.hlsli"
#endif

float3 CurrentEdit(int2 pixel)
{
    int3 p = int3(clamp(pixel, int2(0, 0), int2(gWidth, gHeight) - 1), 0);
    float3 proxy = SanitizeFinite3(gOriginal.Load(p).rgb, 0);
    return SanitizeFinite3(gAux.Load(p).rgb, proxy) - proxy;
}

float MaxChannel(float3 rgb) { return max(rgb.r, max(rgb.g, rgb.b)); }
// C1 linear-expanded soft-shoulder carrier strictly inside (0, 1) for SDR DLSS:
// - 100% linear with 1.5x gain for |edit| <= 0.20 (maps to [0.20, 0.80] — zero Jensen bias, +50% SNR)
// - Smooth C1 hyperbolic shoulder above 0.20 (zero shadow clipping even at edit = -1.0!)
float3 Carrier(float3 edit)
{
    const float3 a = abs(edit);
    const float3 lin = 1.5 * a;
    const float3 u = max(a - 0.20, 0.0);
    const float3 ku = 1.5 * u;
    const float3 shoulder = 0.30 + 0.20 * (ku / (0.20 + ku));
    const float3 mag = lerp(lin, shoulder, step(0.20, a));
    return clamp(0.5 + sign(edit) * mag, 0.0, 1.0);
}
float3 DecodeCarrier(float3 encoded)
{
    const float3 s = SanitizeFinite3(encoded, 0.5) - 0.5;
    const float3 a = abs(s);
    const float3 lin = a * (1.0 / 1.5);
    const float3 t = min(max(a - 0.30, 0.0) * (1.0 / 0.20), 0.96);
    const float3 shoulder = 0.20 + (0.20 / 1.5) * (t / (1.0 - t));
    const float3 mag = lerp(lin, shoulder, step(0.30, a));
    return sign(s) * mag;
}
float3 ApplyDetailRecovery(int2 pixel, float3 filtered, float3 raw)
{
    const float recovery = asfloat(gExposureSourceWidth);
    [branch] if (!isfinite(recovery) || recovery <= 0.001)
        return filtered;
    // Exact 3x3 isotropic Gaussian [1,2,1]x[1,2,1]/16 via 4 hardware-bilinear half-texel taps at (+-0.5, +-0.5):
    // mathematically identical to the 9-tap kernel at 2x fewer texture fetches (12 vs 24) and 8x fewer DecodeCarrier calls (1 vs 8).
    const float2 invSize = rcp(float2(gWidth, gHeight));
    const float2 uvC = (float2(pixel) + 0.5) * invSize;
    const float2 h = 0.5 * invSize;
    const float2 uv00 = uvC + float2(-h.x, -h.y);
    const float2 uv10 = uvC + float2( h.x, -h.y);
    const float2 uv01 = uvC + float2(-h.x,  h.y);
    const float2 uv11 = uvC + float2( h.x,  h.y);

    const float3 cAvg = 0.25 * (
        gSource.SampleLevel(gLinear, uv00, 0).rgb +
        gSource.SampleLevel(gLinear, uv10, 0).rgb +
        gSource.SampleLevel(gLinear, uv01, 0).rgb +
        gSource.SampleLevel(gLinear, uv11, 0).rgb);
    const float3 blurDlss = all(isfinite(cAvg)) ? DecodeCarrier(cAvg) : filtered;

    const float3 r00 = gAux.SampleLevel(gLinear, uv00, 0).rgb - gOriginal.SampleLevel(gLinear, uv00, 0).rgb;
    const float3 r10 = gAux.SampleLevel(gLinear, uv10, 0).rgb - gOriginal.SampleLevel(gLinear, uv10, 0).rgb;
    const float3 r01 = gAux.SampleLevel(gLinear, uv01, 0).rgb - gOriginal.SampleLevel(gLinear, uv01, 0).rgb;
    const float3 r11 = gAux.SampleLevel(gLinear, uv11, 0).rgb - gOriginal.SampleLevel(gLinear, uv11, 0).rgb;
    const float3 q00 = all(isfinite(r00)) ? r00 : raw;
    const float3 q10 = all(isfinite(r10)) ? r10 : raw;
    const float3 q01 = all(isfinite(r01)) ? r01 : raw;
    const float3 q11 = all(isfinite(r11)) ? r11 : raw;
    const float3 blurRaw = 0.25 * (q00 + q10 + q01 + q11);
    const float3 minRaw = min(raw, min(min(q00, q10), min(q01, q11)));
    const float3 maxRaw = max(raw, max(max(q00, q10), max(q01, q11)));

    const float3 hRaw = raw - blurRaw;
    const float3 hDlss = filtered - blurDlss;
    const float3 deltaH = hRaw - hDlss;

    // Luma/chroma split: restore 100% of crisp luminance micro-detail (eyes, skin, hair, grates) while keeping chroma steady (55%).
    const float3 kY = float3(0.2126, 0.7152, 0.0722);
    const float dY = dot(deltaH, kY);
    const float3 dC = deltaH - dY;
    const float3 deltaHBal = dY + dC * 0.55;

    const float core = smoothstep(0.0025, 0.0085, abs(dY) + 0.5 * MaxChannel(abs(dC)));
    float3 outEdit = filtered + deltaHBal * (min(recovery, 1.0) * core);

    const float3 localRange = maxRaw - minRaw;
    if (recovery > 1.0)
    {
        // Contrast-adaptive (CAS-style) boost above 100%: lifts fine interior textures without ringing hard edges.
        const float edgeGuard = 1.0 - smoothstep(0.16, 0.42, MaxChannel(localRange));
        const float hRawY = dot(hRaw, kY);
        const float3 hRawBal = hRawY + (hRaw - hRawY) * 0.50;
        const float coreRaw = smoothstep(0.0015, 0.0080, abs(hRawY));
        outEdit += clamp(hRawBal, -0.10, 0.10) * ((min(recovery, 1.5) - 1.0) * coreRaw * edgeGuard);
    }

    // Anti-ringing neighborhood envelope clamp: mathematically eliminates overshoot halos.
    const float3 minEnv = min(minRaw, filtered);
    const float3 maxEnv = max(maxRaw, filtered);
    const float3 margin = 0.015 + 0.12 * localRange;
    return clamp(outEdit, minEnv - margin, maxEnv + margin);
}

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

#ifdef SUPERIOR_GHOST_DECODE_SEED
float4 DecodePixel(uint2 pixel, out float nearDepth)
{
    int3 p = int3(pixel, 0);
    nearDepth = 0;
    float3 encoded = gSource.Load(p).rgb;
    float3 filtered = DecodeCarrier(encoded);
#ifdef SUPERIOR_GHOST_DECODE
    float field = Visibility.SampleLevel(gLinear, (float2(pixel) + .5) / float2(gWidth, gHeight), 0);
    float mask = isfinite(field) ? saturate(field) : 0;
    float3 raw = CurrentEdit(p.xy);
    // Only fetch depth + run GhostWeight on the ~5% disoccluded pixels (mask > 0);
    // skip ApplyDetailRecovery entirely when mask >= 1 replaces with raw anyway.
    [branch] if (mask > 0)
    {
        nearDepth = GhostNearDepth(gMotion.Load(p).x, gPassthrough);
        [branch] if (nearDepth > 0)
        {
            mask *= GhostWeight(p.xy, raw, encoded);
            if (mask >= 1)
                return float4(raw, mask);
            filtered = ApplyDetailRecovery(p.xy, filtered, raw);
            return float4(lerp(filtered, raw, mask), mask);
        }
    }
    filtered = ApplyDetailRecovery(p.xy, filtered, raw);
    return float4(filtered, 0);
#else
    float rawWeight = asfloat(gExposurePadding);
    float3 raw = CurrentEdit(p.xy);
    filtered = ApplyDetailRecovery(p.xy, filtered, raw);
    return float4(rawWeight >= 1 ? raw : lerp(filtered, raw, rawWeight), 1);
#endif
}

// Decode writes binary16 alpha. Reduce the same rounded values before selecting the winning depth.
groupshared float2 MaskTile[128];

[numthreads(16, 8, 1)]
void PrivateDecode(uint3 id : SV_DispatchThreadID, uint3 local : SV_GroupThreadID, uint3 group : SV_GroupID)
{
    const uint lane = local.y * 16 + local.x;
    float2 seed = 0;
    if (id.x < gWidth && id.y < gHeight)
    {
        float4 value = DecodePixel(id.xy, seed.y);
        gTarget[id.xy] = value;
        seed.x = f16tof32(f32tof16(value.a));
    }
    MaskTile[lane] = seed;
    GroupMemoryBarrierWithGroupSync();
    if (lane < 8)
    {
        uint2 origin = uint2(lane % 4, lane / 4) * 4;
        uint2 cell = group.xy * uint2(4, 2) + origin / 4;
        if (any(cell >= (uint2(gWidth, gHeight) + 3) / 4))
            return;
        float2 best = 0;
        // Match the seed shader's first-maximum tie break in row-major order.
        [unroll] for (uint y = 0; y < 4; ++y)
        [unroll] for (uint x = 0; x < 4; ++x)
        {
            float2 sample = MaskTile[(origin.y + y) * 16 + origin.x + x];
            if (sample.x > best.x)
                best = sample;
        }
        gKeep[cell] = best.y > 0 ? float4(best, 0, 0) : 0;
    }
}

#else
[numthreads(8, 8, 1)] void PrivateDecode(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;
    int3 p = int3(id.xy, 0);
    float3 encoded = gSource.Load(p).rgb;
    float3 filtered = DecodeCarrier(encoded);
    float3 raw = CurrentEdit(p.xy);
#ifdef SUPERIOR_GHOST_DECODE
    float field = Visibility.SampleLevel(gLinear, (float2(id.xy) + .5) / float2(gWidth, gHeight), 0);
    float mask = isfinite(field) ? saturate(field) : 0;
    [branch] if (mask > 0)
    {
        if (GhostNearDepth(gMotion.Load(p).x, gPassthrough) > 0)
        {
            mask *= GhostWeight(p.xy, raw, encoded);
            if (mask >= 1)
            {
                gTarget[id.xy] = float4(raw, mask);
                return;
            }
            filtered = ApplyDetailRecovery(p.xy, filtered, raw);
            gTarget[id.xy] = float4(lerp(filtered, raw, mask), mask);
            return;
        }
    }
    filtered = ApplyDetailRecovery(p.xy, filtered, raw);
    gTarget[id.xy] = float4(filtered, 0);
#else
    filtered = ApplyDetailRecovery(p.xy, filtered, raw);
    float rawWeight = asfloat(gExposurePadding);
    gTarget[id.xy] = float4(rawWeight >= 1 ? raw : lerp(filtered, raw, rawWeight), 1);
#endif
}

#endif

[numthreads(8, 8, 1)] void PrivateEncode(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;
    float2 uv = (float2(id.xy) + .5) / float2(gWidth, gHeight);
    // Direct DLSS SR (gPassthrough != 0) feeds the raw NR answer straight to DLSS SR and never reads gTarget (carrier).
    if (gPassthrough == 0u)
    {
#ifdef SUPERIOR_PRIVATE_ENCODE_PACKED
        // Fused spatial unpack -> rectilinear work-res C1 carrier: samples packed proxy/answer (modelW x modelH)
        // using PwPack via the 11x1 warp metadata bound at t3 (gMotion), avoiding a separate Unpack pass.
        const float2 workInv = rcp(float2(gWidth, gHeight));
        const float2 puvC = saturate(PwPack(uv * gNativeWorkSize.xy) / gNativeWorkSize.zw);
        const float2 puvL = saturate(PwPack(saturate(uv + float2(-workInv.x, 0.0)) * gNativeWorkSize.xy) / gNativeWorkSize.zw);
        const float2 puvR = saturate(PwPack(saturate(uv + float2( workInv.x, 0.0)) * gNativeWorkSize.xy) / gNativeWorkSize.zw);
        const float2 puvU = saturate(PwPack(saturate(uv + float2(0.0, -workInv.y)) * gNativeWorkSize.xy) / gNativeWorkSize.zw);
        const float2 puvD = saturate(PwPack(saturate(uv + float2(0.0,  workInv.y)) * gNativeWorkSize.xy) / gNativeWorkSize.zw);
        const float3 proxyC = SanitizeFinite3(gSource.SampleLevel(gLinear, puvC, 0).rgb, 0);
        const float3 proxyL = SanitizeFinite3(gSource.SampleLevel(gLinear, puvL, 0).rgb, proxyC);
        const float3 proxyR = SanitizeFinite3(gSource.SampleLevel(gLinear, puvR, 0).rgb, proxyC);
        const float3 proxyU = SanitizeFinite3(gSource.SampleLevel(gLinear, puvU, 0).rgb, proxyC);
        const float3 proxyD = SanitizeFinite3(gSource.SampleLevel(gLinear, puvD, 0).rgb, proxyC);
        // t1 holds the NR answer in the encoding specialization.
        const float3 answerC = SanitizeFinite3(gUnusedPrivateModel.SampleLevel(gLinear, puvC, 0).rgb, proxyC);
        const float3 answerL = SanitizeFinite3(gUnusedPrivateModel.SampleLevel(gLinear, puvL, 0).rgb, proxyL);
        const float3 answerR = SanitizeFinite3(gUnusedPrivateModel.SampleLevel(gLinear, puvR, 0).rgb, proxyR);
        const float3 answerU = SanitizeFinite3(gUnusedPrivateModel.SampleLevel(gLinear, puvU, 0).rgb, proxyU);
        const float3 answerD = SanitizeFinite3(gUnusedPrivateModel.SampleLevel(gLinear, puvD, 0).rgb, proxyD);
#else
        const int2 hi = int2((int)gWidth - 1, (int)gHeight - 1);
        const int2 pC = int2(id.xy);
        const int2 pL = max(pC + int2(-1, 0), int2(0, 0));
        const int2 pR = min(pC + int2( 1, 0), hi);
        const int2 pU = max(pC + int2(0, -1), int2(0, 0));
        const int2 pD = min(pC + int2(0,  1), hi);
        const float3 proxyC = SanitizeFinite3(gSource.Load(int3(pC, 0)).rgb, 0);
        const float3 proxyL = SanitizeFinite3(gSource.Load(int3(pL, 0)).rgb, proxyC);
        const float3 proxyR = SanitizeFinite3(gSource.Load(int3(pR, 0)).rgb, proxyC);
        const float3 proxyU = SanitizeFinite3(gSource.Load(int3(pU, 0)).rgb, proxyC);
        const float3 proxyD = SanitizeFinite3(gSource.Load(int3(pD, 0)).rgb, proxyC);
        // t1 holds the NR answer in the encoding specialization.
        const float3 answerC = SanitizeFinite3(gUnusedPrivateModel.Load(int3(pC, 0)).rgb, proxyC);
        const float3 answerL = SanitizeFinite3(gUnusedPrivateModel.Load(int3(pL, 0)).rgb, proxyL);
        const float3 answerR = SanitizeFinite3(gUnusedPrivateModel.Load(int3(pR, 0)).rgb, proxyR);
        const float3 answerU = SanitizeFinite3(gUnusedPrivateModel.Load(int3(pU, 0)).rgb, proxyU);
        const float3 answerD = SanitizeFinite3(gUnusedPrivateModel.Load(int3(pD, 0)).rgb, proxyD);
#endif
        const float3 eC = answerC - proxyC;
        const float3 eL = answerL - proxyL;
        const float3 eR = answerR - proxyR;
        const float3 eU = answerU - proxyU;
        const float3 eD = answerD - proxyD;

        // Gentle work-res proxy-guided 1px isolated-spike anti-speckle (never bleeds across silhouettes).
        const float invSig2 = 1.0 / (3.0 * 0.045 * 0.045);
        const float3 dpL = proxyL - proxyC;
        const float3 dpR = proxyR - proxyC;
        const float3 dpU = proxyU - proxyC;
        const float3 dpD = proxyD - proxyC;
        const float wL = exp2(-dot(dpL, dpL) * invSig2);
        const float wR = exp2(-dot(dpR, dpR) * invSig2);
        const float wU = exp2(-dot(dpU, dpU) * invSig2);
        const float wD = exp2(-dot(dpD, dpD) * invSig2);

        const float3 sL = wL > 0.25 ? eL : eC;
        const float3 sR = wR > 0.25 ? eR : eC;
        const float3 sU = wU > 0.25 ? eU : eC;
        const float3 sD = wD > 0.25 ? eD : eC;
        const float3 eMin4 = min(min(sL, sR), min(sU, sD));
        const float3 eMax4 = max(max(sL, sR), max(sU, sD));

        const float mL = wL > 0.25 ? wL : 0.0;
        const float mR = wR > 0.25 ? wR : 0.0;
        const float mU = wU > 0.25 ? wU : 0.0;
        const float mD = wD > 0.25 ? wD : 0.0;
        const float mSum = mL + mR + mU + mD;
        const float3 eAvg = mSum > 1e-4 ? (mL * eL + mR * eR + mU * eU + mD * eD) / mSum : eC;

        const float3 kY = float3(0.2126, 0.7152, 0.0722);
        const float3 dC = eC - eAvg;
        const float dY = dot(dC, kY);
        const float thresh = 0.25 * dY * dY;
        const float sameSignCount =
            ((wL > 0.25 && dot(eL - eAvg, kY) * dY > thresh) ? 1.0 : 0.0) +
            ((wR > 0.25 && dot(eR - eAvg, kY) * dY > thresh) ? 1.0 : 0.0) +
            ((wU > 0.25 && dot(eU - eAvg, kY) * dY > thresh) ? 1.0 : 0.0) +
            ((wD > 0.25 && dot(eD - eAvg, kY) * dY > thresh) ? 1.0 : 0.0);
        const float isolated = saturate(1.0 - 0.70 * sameSignCount);
        const float peak = max(abs(dY), max(abs(dC.r), max(abs(dC.g), abs(dC.b))));
        const float smallToMed = 1.0 - smoothstep(0.015, 0.040, peak);
        precise float3 edit = lerp(eC, clamp(eC, eMin4, eMax4), isolated * smallToMed * 0.60);
        gTarget[id.xy] = float4(Carrier(edit), 1);
    }
    uint2 extent = uint2(gTransferStrength, gColourStrength);
    uint2 source = min(uint2(uv * extent), extent - 1) + uint2(gCompareSwap, gTransfer);
    float2 mv = gOriginal.Load(int3(source, 0)).xy * float2(gMvScaleX, gMvScaleY);
    gKeep[id.xy] = float4(all(isfinite(mv)) ? mv : float2(0, 0), 0, 1);
}
