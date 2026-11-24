// Intrinsic 4K-Oracle Stage B1: MV-reprojected temporal accumulation of work-res intrinsic field.
// Inputs:
//   t0 (gSource):            curIntrinsic  (workW x workH, RGBA16F)
//   t1 (gUnusedOracleModel): prevIntrinsic (workW x workH, RGBA16F)
//   t2 (gOriginal):          curDepth      (depthRect in gDebugView/gCompareMode/gGuideWidth/gGuideHeight)
//   t3 (gMotion):            motionIn      (motionRect in gCompareSwap/gTransfer/gTransferStrength/gColourStrength, MvScaleX/Y -> work pixels)
//   t4 (gAux):               prevDepth     (workW x workH, R32F / RGBA16F)
// Outputs:
//   u0 (gTarget):            outIntrinsic  (workW x workH, RGBA16F)
//   u1 (gKeep):              float4(zCur, 0, 0, 0)
#define gModel gUnusedOracleModel
#define gAux2 gUnusedOracleAux
#include "precompile/dlssnr.hlsl"
#undef gModel
#undef gAux2
#include "GhostDepth.hlsli"

float4 SanitizeFinite4(float4 v, float4 fallback)
{
    return float4(SanitizeFinite(v.x, fallback.x), SanitizeFinite(v.y, fallback.y),
                  SanitizeFinite(v.z, fallback.z), SanitizeFinite(v.w, fallback.w));
}

// 5-tap Catmull-Rom (Jimenez/Unreal) clamped to the caller-provided true unblurred neighborhood [clampMin, clampMax]:
// eliminates bilinear blur on jittered cur and moving history while guaranteeing zero ringing/overshoot on step edges.
float4 SampleCatmullRomClamped(Texture2D<float4> tex, float2 uv, float2 texSize, float2 invSize, float4 fallback,
                               float4 clampMin, float4 clampMax)
{
    const float2 samplePos = uv * texSize;
    const float2 tc1 = floor(samplePos - 0.5) + 0.5;
    const float2 f = samplePos - tc1;
    const float2 f2 = f * f;
    const float2 f3 = f2 * f;

    const float2 w0 = f2 - 0.5 * (f3 + f);
    const float2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    const float2 w3 = 0.5 * (f3 - f2);
    const float2 w2 = 1.0 - w0 - w1 - w3;

    const float2 w12 = w1 + w2;
    const float2 tc0 = saturate((tc1 - 1.0) * invSize);
    const float2 tc3 = saturate((tc1 + 2.0) * invSize);
    const float2 tc12 = saturate((tc1 + w2 / max(w12, 1e-5)) * invSize);

    const float4 s0 = SanitizeFinite4(tex.SampleLevel(gLinear, float2(tc12.x, tc0.y), 0), fallback);
    const float4 s1 = SanitizeFinite4(tex.SampleLevel(gLinear, float2(tc0.x,  tc12.y), 0), fallback);
    const float4 s2 = SanitizeFinite4(tex.SampleLevel(gLinear, float2(tc12.x, tc12.y), 0), fallback);
    const float4 s3 = SanitizeFinite4(tex.SampleLevel(gLinear, float2(tc3.x,  tc12.y), 0), fallback);
    const float4 s4 = SanitizeFinite4(tex.SampleLevel(gLinear, float2(tc12.x, tc3.y), 0), fallback);

    const float cw0 = w12.x * w0.y;
    const float cw1 = w0.x  * w12.y;
    const float cw2 = w12.x * w12.y;
    const float cw3 = w3.x  * w12.y;
    const float cw4 = w12.x * w3.y;
    const float wSum = cw0 + cw1 + cw2 + cw3 + cw4;

    const float4 cr = (s0 * cw0 + s1 * cw1 + s2 * cw2 + s3 * cw3 + s4 * cw4) * rcp(max(wSum, 1e-5));
    return clamp(cr, clampMin, clampMax);
}

float LoadWorkDepth(int2 wp, int2 hi, float2 invSize, uint depthInv)
{
    const float2 uv = (float2(clamp(wp, int2(0, 0), hi)) + 0.5) * invSize;
    const uint2 dExt = uint2(gGuideWidth, gGuideHeight);
    const uint2 dCoord = min(uint2(uv * float2(dExt)), dExt - 1u) + uint2(gDebugView, gCompareMode);
    return GhostNearDepth(gOriginal.Load(int3(dCoord, 0)).x, depthInv);
}

[numthreads(8, 8, 1)]
void OracleTemporalMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    const int2 hi = int2((int)gWidth - 1, (int)gHeight - 1);
    const float2 texSize = float2(max(gWidth, 1u), max(gHeight, 1u));
    const float2 invSize = rcp(texSize);
    const int2 center = int2(id.xy);

    const uint depthInv = (gPassthrough & 1u) != 0u ? 1u : 0u;
    const bool reset = (gPassthrough & 2u) != 0u;
    const bool haveDepth = ((gPassthrough & 4u) != 0u) && (gGuideWidth > 0u) && (gGuideHeight > 0u);
    const float2 jitter = float2(
        clamp(SanitizeFinite(gCompareSplit, 0.0), -0.5, 0.5),
        clamp(SanitizeFinite(gCompareZoom, 0.0), -0.5, 0.5));

    // 3x3 raw neighborhood mean, standard deviation, and true unblurred [min, max] box.
    const float4 centerRaw = SanitizeFinite4(gSource.Load(int3(center, 0)), float4(0.0, 0.0, 0.0, 1.0));
    float4 m1 = 0.0;
    float4 m2 = 0.0;
    float4 nMin = centerRaw;
    float4 nMax = centerRaw;
    [unroll] for (int oy = -1; oy <= 1; ++oy)
    {
        [unroll] for (int ox = -1; ox <= 1; ++ox)
        {
            const int3 p = int3(clamp(center + int2(ox, oy), int2(0, 0), hi), 0);
            const float4 s = SanitizeFinite4(gSource.Load(p), centerRaw);
            m1 += s;
            m2 += s * s;
            nMin = min(nMin, s);
            nMax = max(nMax, s);
        }
    }
    const float4 mu = m1 * (1.0 / 9.0);
    const float4 sigma = sqrt(max(m2 * (1.0 / 9.0) - mu * mu, 0.0));

    // Unjitter current intrinsic sample onto the canonical unjittered grid via 5-tap Catmull-Rom clamped to [nMin, nMax].
    float4 cur = centerRaw;
    if (any(abs(jitter) > 1e-5))
    {
        const float2 curUv = saturate((float2(id.xy) + 0.5 - jitter) * invSize);
        cur = SampleCatmullRomClamped(gSource, curUv, texSize, invSize, centerRaw, nMin, nMax);
    }

    // Current depth for temporal disocclusion check (mv tracks the same 3D surface point across frames,
    // so previous-frame depth at prevUv matches zCur directly without spatial plane extrapolation).
    const float kSkyEps = 1e-10;
    float zCur = 0.0;
    if (haveDepth)
        zCur = LoadWorkDepth(center, hi, invSize, depthInv);

    // Motion vector in work pixels (OptiScaler / NGX sign: previousPos = currentPos + mv).
    float2 mv = float2(0.0, 0.0);
    if (gTransferStrength >= 1.0 && gColourStrength >= 1.0)
    {
        const uint2 mExt = uint2(gTransferStrength, gColourStrength);
        const float2 uv = (float2(id.xy) + 0.5) * invSize;
        const uint2 mCoord = min(uint2(uv * float2(mExt)), mExt - 1u) + uint2(gCompareSwap, gTransfer);
        const float2 rawMv = gMotion.Load(int3(mCoord, 0)).xy * float2(gMvScaleX, gMvScaleY);
        if (all(isfinite(rawMv)))
            mv = rawMv;
    }

    const float2 prevUv = (float2(id.xy) + 0.5 + mv) * invSize;
    const int2 prevCoord = clamp(int2(floor(float2(id.xy) + mv + 0.5)), int2(0, 0), hi);
    const float2 mvFrac = abs(frac(mv + 0.5) - 0.5);
    const bool nearIntMv = all(mvFrac < 0.005);
    bool validHist = !reset && prevUv.x > 0.0 && prevUv.x < 1.0 && prevUv.y > 0.0 && prevUv.y < 1.0;

    if (validHist && haveDepth && zCur > 0.0)
    {
        // Nearest-texel depth check eliminates bilinear silhouette contamination with 1 depth load/px.
        const bool curIsSky = zCur <= kSkyEps;
        const float zTol = 0.045 * zCur + 1e-6;
        const float zPrev = gAux.Load(int3(prevCoord, 0)).x;
        const bool prevValid = isfinite(zPrev) && (zPrev > 0.0);
        const bool prevIsSky = prevValid && (zPrev <= kSkyEps);
        if (!prevValid || (curIsSky != prevIsSky) || (!curIsSky && abs(zPrev - zCur) > zTol))
            validHist = false;
    }

    float4 outVal = cur;
    if (validHist)
    {
        const float4 hist = nearIntMv
            ? SanitizeFinite4(gUnusedOracleModel.Load(int3(prevCoord, 0)), cur)
            : SampleCatmullRomClamped(gUnusedOracleModel, prevUv, texSize, invSize, cur, nMin, nMax);
        const float mvLen = length(mv);
        const float gammaRgb = lerp(1.25, 0.85, saturate(mvLen * 0.25));
        const float4 gamma = float4(gammaRgb, gammaRgb, gammaRgb, 0.95);
        const float4 histClamped = clamp(hist, mu - gamma * sigma, mu + gamma * sigma);
        const float alpha = lerp(0.10, 0.35, saturate(mvLen * 0.20));
        outVal = lerp(histClamped, cur, alpha);
    }

    gTarget[id.xy] = float4(clamp(outVal.rgb, -4.3, 3.0), clamp(outVal.a, 0.60, 1.65));
    gKeep[id.xy] = float4(max(zCur, 0.0), 0.0, 0.0, 0.0);
}
