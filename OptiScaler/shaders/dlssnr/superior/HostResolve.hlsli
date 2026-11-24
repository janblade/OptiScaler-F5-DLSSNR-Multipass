// Superior Scaling host resolve, folded into janblade's final composition.
// Residual DLAA / No DLSS (gGuideHeight == 0 or 1):
//   t0: working proxy, t1: decoded signed DLSS residual (0) or raw NR answer (1), t5: Low64 field (when gGuideWidth != 0).
// Direct DLSS SR (gGuideHeight == 2):
//   t0: working proxy, t1: full-native DLSS SR super-resolved C1 residual carrier (gWidth x gHeight),
//   t2: untouched native frame, t5: working NR answer (Quality mode HUD guard; omitted in Performance mode).
// Intrinsic 4K-Oracle (gGuideHeight == 3):
//   t0: rectilinear oracleProxy (workW x workH), t1: outIntrinsic (workW x workH, RGBA16F = logI.rgb + G_detail),
//   t2: untouched 4K native frame, t5: 4K/render depth buffer (optional, when (gGuideWidth & 1u) != 0u).
// No exposure, colour encoding or tone-mapping logic belongs in this include.
#include "GhostDepth.hlsli"
#if defined(VK_MODE)
// Binding 8 is the live shader's depth fade slot; the Superior resolve leaves the fade out and uses it for the low field / work answer.
[[vk::binding(8, 0)]]
#endif
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
// Exact inverse of PrivateCarrier.hlsl::Carrier (C1 linear 1.5x inside |edit|<=0.20 + smooth hyperbolic shoulder).
float3 SuperiorDecodeCarrier(float3 encoded)
{
    const float3 s = SanitizeFinite3(encoded, 0.5) - 0.5;
    const float3 a = abs(s);
    const float3 lin = a * (1.0 / 1.5);
    const float3 t = min(max(a - 0.30, 0.0) * (1.0 / 0.20), 0.96);
    const float3 shoulder = 0.20 + (0.20 / 1.5) * (t / (1.0 - t));
    const float3 mag = lerp(lin, shoulder, step(0.30, a));
    return sign(s) * mag;
}
// Forward declaration of dlssnr.hlsl's ProxyEncode (defined below this include) for exact HDR/SDR proxy-domain RCAS.
float3 ProxyEncode(float3 n);

// Perceptual [0,1] edge-detection guide for Stage 1 (4K Guided Edge-Snap).
// In SDR (gPassthrough != 0) this matches nativeRaw bit-for-bit; in HDR it is a monotone Reinhard-gamma2 curve
// that stays unclipped across highlights for reliable geometric edge weights.
float3 SuperiorDirectSrGuide(float3 origNorm)
{
    const float3 pos = max(SanitizeFinite3(origNorm, 0.0), 0.0);
    return gPassthrough != 0u ? saturate(pos) : sqrt(pos / (1.0 + pos));
}

// Exact nativeRaw proxy domain for Stage 3 (AMD FidelityFX RCAS): matches dlssnr.hlsl's nativeRaw bit-for-bit
// across SDR passthrough and all HDR proxy curves (Knee, Neutwo, Hybrid, HLG, PQ, Linear).
float3 SuperiorDirectSrRaw(float3 origNorm)
{
    const float3 pos = max(SanitizeFinite3(origNorm, 0.0), 0.0);
    return gPassthrough != 0u ? saturate(pos) : LinearToSrgb(saturate(ProxyEncode(pos)));
}

// Direct DLSS SR resolve (gGuideHeight == 2):
// Full-native (work -> native) DLSS SR super-resolved C1 residual carrier over stationary 4K nativeRaw,
// enhanced by carrier-only sharpness (Stage 1 4K Guided Edge-Snap in Quality mode + clean carrier deconvolution;
// 1px isolated-spike anti-speckle runs upstream at work resolution in PrivateEncode, and nativeRaw is never RCAS'd):
float3 SuperiorDirectSrEdit(float2 uvq, float3 nativeRaw, float normScale, float origLuma)
{
    const float2 suv = saturate(uvq);
    uint srW, srH;
    gModel.GetDimensions(srW, srH);
    if (srW == 0u || srH == 0u)
        return 0.0;

    const bool cmpZoom = (gCompareMode == 1u);
    const int2 hi = int2((int)srW - 1, (int)srH - 1);
    const int2 center = clamp(int2(suv * float2(srW, srH)), int2(0, 0), hi);
    const float2 srInv = rcp(float2(srW, srH));
    const float3 cC = SanitizeFinite3(cmpZoom ? gModel.SampleLevel(gLinear, suv, 0).rgb
                                              : gModel.Load(int3(center, 0)).rgb, 0.5);

    const float detailStrength = isfinite(gMvScaleX) ? saturate(gMvScaleX) : 0.0;
    const float rawSharp = asfloat(gExposureSourceWidth);
    const float nrSharp = isfinite(rawSharp) ? saturate(rawSharp) : 0.65;

    // Fast early-out on neutral carrier pixels (untouched HUD/static areas): skips HUD reads, Option 5 & decode.
    const float3 dev = abs(cC - 0.5);
    const float maxDev = max(dev.r, max(dev.g, dev.b));
    [branch] if (maxDev <= 0.0004 && detailStrength <= 0.001)
        return 0.0;

    // Smooth untouched-pixel / 2D HUD guard (Quality mode only, only on low-deviation pixels, zero seam at 0.025).
    float editKeep = 1.0;
    [branch] if (gGuideWidth != 0u && maxDev < 0.025)
    {
        const float3 pWork = SanitizeFinite3(gSource.SampleLevel(gLinear, suv, 0).rgb, 0.5);
        const float3 aWork = SanitizeFinite3(gSuperiorLow.SampleLevel(gLinear, suv, 0).rgb, pWork);
        const float3 nrDiff = abs(aWork - pWork);
        const float activityKeep = smoothstep(0.001, 0.007, max(nrDiff.r, max(nrDiff.g, nrDiff.b)));
        editKeep = lerp(activityKeep, 1.0, smoothstep(0.012, 0.025, maxDev));
        [branch] if (editKeep <= 0.001 && detailStrength <= 0.001)
            return 0.0;
    }

    const float3 kY = float3(0.2126, 0.7152, 0.0722);
    const bool fastResolve = isfinite(gMvScaleY) && (gMvScaleY < 0.5);
    const bool runOpt5 = (nrSharp > 0.001) && (editKeep > 0.001);
    const bool needOrigCross = (runOpt5 && !fastResolve) || (detailStrength > 0.001);

    // Shared 4-tap cardinal load of untouched 4K gOriginal (sanitized; CompareMode==1 uses aligned SampleLevel).
    float3 oL = 0.0, oR = 0.0, oU = 0.0, oD = 0.0;
    float3 gC = nativeRaw;
    [branch] if (needOrigCross)
    {
        const float invNorm = normScale > 1e-6 ? rcp(normScale) : 1.0;
        const int2 origHi = int2((int)gWidth - 1, (int)gHeight - 1);
        const int2 op = clamp(int2(suv * float2(gWidth, gHeight)), int2(0, 0), origHi);
        if (cmpZoom)
        {
            const float2 origInv = rcp(float2(max(gWidth, 1u), max(gHeight, 1u)));
            oL = max(SanitizeFinite3(OriginalInPassDomain(gOriginal.SampleLevel(gLinear, saturate(suv + float2(-origInv.x, 0.0)), 0)).rgb * invNorm, 0.0), 0.0);
            oR = max(SanitizeFinite3(OriginalInPassDomain(gOriginal.SampleLevel(gLinear, saturate(suv + float2( origInv.x, 0.0)), 0)).rgb * invNorm, 0.0), 0.0);
            oU = max(SanitizeFinite3(OriginalInPassDomain(gOriginal.SampleLevel(gLinear, saturate(suv + float2(0.0, -origInv.y)), 0)).rgb * invNorm, 0.0), 0.0);
            oD = max(SanitizeFinite3(OriginalInPassDomain(gOriginal.SampleLevel(gLinear, saturate(suv + float2(0.0,  origInv.y)), 0)).rgb * invNorm, 0.0), 0.0);
            if (gPassthrough == 0u && runOpt5 && !fastResolve)
            {
                const float3 oC = max(SanitizeFinite3(OriginalInPassDomain(gOriginal.SampleLevel(gLinear, suv, 0)).rgb * invNorm, 0.0), 0.0);
                gC = SuperiorDirectSrGuide(oC);
            }
        }
        else
        {
            oL = max(SanitizeFinite3(OriginalInPassDomain(gOriginal.Load(int3(max(op + int2(-1, 0), int2(0, 0)), 0))).rgb * invNorm, 0.0), 0.0);
            oR = max(SanitizeFinite3(OriginalInPassDomain(gOriginal.Load(int3(min(op + int2( 1, 0), origHi), 0))).rgb * invNorm, 0.0), 0.0);
            oU = max(SanitizeFinite3(OriginalInPassDomain(gOriginal.Load(int3(max(op + int2(0, -1), int2(0, 0)), 0))).rgb * invNorm, 0.0), 0.0);
            oD = max(SanitizeFinite3(OriginalInPassDomain(gOriginal.Load(int3(min(op + int2(0,  1), origHi), 0))).rgb * invNorm, 0.0), 0.0);
            if (gPassthrough == 0u && runOpt5 && !fastResolve)
            {
                const float3 oC = max(SanitizeFinite3(OriginalInPassDomain(gOriginal.Load(int3(op, 0))).rgb * invNorm, 0.0), 0.0);
                gC = SuperiorDirectSrGuide(oC);
            }
        }
    }

    float3 edit = 0.0;
    [branch] if (runOpt5 && fastResolve)
    {
        // Route 3 Performance fast-path: 5-tap carrier-only luma deconvolution
        // (5 total fetches, zero gOriginal loads, zero Stage 1 exp2).
        const int2 pL = max(center + int2(-1, 0), int2(0, 0));
        const int2 pR = min(center + int2( 1, 0), hi);
        const int2 pU = max(center + int2(0, -1), int2(0, 0));
        const int2 pD = min(center + int2(0,  1), hi);

        const float3 cL = SanitizeFinite3(cmpZoom ? gModel.SampleLevel(gLinear, saturate(suv + float2(-srInv.x, 0.0)), 0).rgb : gModel.Load(int3(pL, 0)).rgb, cC);
        const float3 cR = SanitizeFinite3(cmpZoom ? gModel.SampleLevel(gLinear, saturate(suv + float2( srInv.x, 0.0)), 0).rgb : gModel.Load(int3(pR, 0)).rgb, cC);
        const float3 cU = SanitizeFinite3(cmpZoom ? gModel.SampleLevel(gLinear, saturate(suv + float2(0.0, -srInv.y)), 0).rgb : gModel.Load(int3(pU, 0)).rgb, cC);
        const float3 cD = SanitizeFinite3(cmpZoom ? gModel.SampleLevel(gLinear, saturate(suv + float2(0.0,  srInv.y)), 0).rgb : gModel.Load(int3(pD, 0)).rgb, cC);

        const float3 cBlur4 = 0.25 * (cL + cR + cU + cD);
        const float3 cMin4 = min(cC, min(min(cL, cR), min(cU, cD)));
        const float3 cMax4 = max(cC, max(max(cL, cR), max(cU, cD)));

        const float3 cHigh = cC - cBlur4;
        const float hY = dot(cHigh, kY);
        const float3 hBal = hY + (cHigh - hY) * 0.05;
        const float core = smoothstep(0.008, 0.022, abs(hY));
        const float deconvGain = (1.15 * pow(nrSharp, 0.72)) * core;
        float3 shDamp = saturate(1.0 - max(abs(cC - 0.5) - 0.30, 0.0) * (1.0 / 0.20));
        shDamp *= shDamp;
        const float3 ringMargin = min(0.16 * (cMax4 - cMin4), 0.022) * nrSharp * shDamp;
        const float3 cDeconv = saturate(clamp(cC + hBal * (deconvGain * shDamp), cMin4 - ringMargin, cMax4 + ringMargin));
        edit = SuperiorDecodeCarrier(cDeconv);
    }
    else if (runOpt5)
    {
        const int2 pL = max(center + int2(-1, 0), int2(0, 0));
        const int2 pR = min(center + int2( 1, 0), hi);
        const int2 pU = max(center + int2(0, -1), int2(0, 0));
        const int2 pD = min(center + int2(0,  1), hi);

        const float3 cL = SanitizeFinite3(cmpZoom ? gModel.SampleLevel(gLinear, saturate(suv + float2(-srInv.x, 0.0)), 0).rgb : gModel.Load(int3(pL, 0)).rgb, cC);
        const float3 cR = SanitizeFinite3(cmpZoom ? gModel.SampleLevel(gLinear, saturate(suv + float2( srInv.x, 0.0)), 0).rgb : gModel.Load(int3(pR, 0)).rgb, cC);
        const float3 cU = SanitizeFinite3(cmpZoom ? gModel.SampleLevel(gLinear, saturate(suv + float2(0.0, -srInv.y)), 0).rgb : gModel.Load(int3(pU, 0)).rgb, cC);
        const float3 cD = SanitizeFinite3(cmpZoom ? gModel.SampleLevel(gLinear, saturate(suv + float2(0.0,  srInv.y)), 0).rgb : gModel.Load(int3(pD, 0)).rgb, cC);

        const float3 gL = SuperiorDirectSrGuide(oL);
        const float3 gR = SuperiorDirectSrGuide(oR);
        const float3 gU = SuperiorDirectSrGuide(oU);
        const float3 gD = SuperiorDirectSrGuide(oD);

        // Stage 1: Axis-asymmetric 4K Guided Edge-Snap.
        const float invSig2 = 1.0 / (3.0 * 0.045 * 0.045);
        const float3 dL = gL - gC;
        const float3 dR = gR - gC;
        const float3 dU = gU - gC;
        const float3 dD = gD - gC;
        const float wL = exp2(-dot(dL, dL) * invSig2);
        const float wR = exp2(-dot(dR, dR) * invSig2);
        const float wU = exp2(-dot(dU, dU) * invSig2);
        const float wD = exp2(-dot(dD, dD) * invSig2);
        const float nx = abs(wL - wR);
        const float ny = abs(wU - wD);
        const float edgeAsym = smoothstep(0.15, 0.65, max(nx, ny));
        const float wNormSum = max(nx * (wL + wR) + ny * (wU + wD), 1e-5);
        const float3 cSameSide = (nx * (wL * cL + wR * cR) + ny * (wU * cU + wD * cD)) / wNormSum;
        const float3 cSnapped = lerp(cC, cSameSide, edgeAsym * (0.45 * nrSharp));

        // Stage 2: 3x3 Isotropic Anti-Ringing Carrier Deconvolution (restores work->native NR micro-detail).
        const float3 cUL = SanitizeFinite3(cmpZoom ? gModel.SampleLevel(gLinear, saturate(suv + float2(-srInv.x, -srInv.y)), 0).rgb : gModel.Load(int3(int2(pL.x, pU.y), 0)).rgb, cC);
        const float3 cUR = SanitizeFinite3(cmpZoom ? gModel.SampleLevel(gLinear, saturate(suv + float2( srInv.x, -srInv.y)), 0).rgb : gModel.Load(int3(int2(pR.x, pU.y), 0)).rgb, cC);
        const float3 cDL = SanitizeFinite3(cmpZoom ? gModel.SampleLevel(gLinear, saturate(suv + float2(-srInv.x,  srInv.y)), 0).rgb : gModel.Load(int3(int2(pL.x, pD.y), 0)).rgb, cC);
        const float3 cDR = SanitizeFinite3(cmpZoom ? gModel.SampleLevel(gLinear, saturate(suv + float2( srInv.x,  srInv.y)), 0).rgb : gModel.Load(int3(int2(pR.x, pD.y), 0)).rgb, cC);

        const float3 cMin4 = min(min(cL, cR), min(cU, cD));
        const float3 cMax4 = max(max(cL, cR), max(cU, cD));
        const float3 cBlur = (2.0 * (cL + cR + cU + cD) + (cUL + cUR + cDL + cDR)) * (1.0 / 12.0);
        const float3 cMin3 = min(cSnapped, min(cMin4, min(min(cUL, cUR), min(cDL, cDR))));
        const float3 cMax3 = max(cSnapped, max(cMax4, max(max(cUL, cUR), max(cDL, cDR))));

        const float3 cHigh = cSnapped - cBlur;
        const float hY = dot(cHigh, kY);
        const float3 hBal = hY + (cHigh - hY) * 0.05;
        const float core = smoothstep(0.008, 0.022, abs(hY));
        const float deconvGain = (1.15 * pow(nrSharp, 0.72)) * core;
        float3 shDamp = saturate(1.0 - max(abs(cSnapped - 0.5) - 0.30, 0.0) * (1.0 / 0.20));
        shDamp *= shDamp;
        const float3 ringMargin = min(0.16 * (cMax3 - cMin3), 0.022) * nrSharp * shDamp;
        const float3 cDeconv = saturate(clamp(cSnapped + hBal * (deconvGain * shDamp), cMin3 - ringMargin, cMax3 + ringMargin));
        edit = SuperiorDecodeCarrier(cDeconv);
    }
    else
    {
        edit = SuperiorDecodeCarrier(cC);
    }

    // Optional edge-safe native micro-contrast lift (0% default -> completely skipped via [branch], reuses oL/oR/oU/oD).
    [branch] if (detailStrength > 0.001)
    {
        const float safeOrigY = isfinite(origLuma) ? max(origLuma, 0.0) : 0.0;
        const float yL = dot(oL, kY);
        const float yR = dot(oR, kY);
        const float yU = dot(oU, kY);
        const float yD = dot(oD, kY);
        const float yAvg = 0.25 * (yL + yR + yU + yD);
        const float yRange = max(safeOrigY, max(max(yL, yR), max(yU, yD))) - min(safeOrigY, min(min(yL, yR), min(yU, yD)));
        const float yHigh = safeOrigY - yAvg;
        const float edgeGuard = 1.0 - smoothstep(0.14, 0.38, yRange);
        const float boostScale = (gPassthrough == 0u && gReversibleMode == 7u) ? 0.55 : 0.35;
        const float lift = clamp(yHigh * (detailStrength * boostScale * edgeGuard), -0.06, 0.06);
        edit += isfinite(lift) ? lift : 0.0;
    }

    return SanitizeFinite3(edit * editKeep, 0.0);
}

// Intrinsic 4K-Oracle resolve (gGuideHeight == 3):
// Stage B2 (2x2 4K-Oracle plane-aware depth/albedo joint bilateral upsample of temporally stabilized intrinsic field)
// + Stage C (physical multiplicative irradiance + brightness-normalized 4K micro-detail reconstruction).
float3 SuperiorIntrinsicOracleEdit(float2 uvq, float3 nativeRaw, float normScale, float origLuma)
{
    const float2 suv = saturate(uvq);
    uint srcW, srcH;
    gSource.GetDimensions(srcW, srcH);
    if (srcW == 0u || srcH == 0u)
        return 0.0;

    const float3 kY = float3(0.2126, 0.7152, 0.0722);
    const bool cmpZoom = (gCompareMode == 1u);
    const float invNorm = normScale > 1e-6 ? rcp(normScale) : 1.0;
    const float2 nativeSize = float2(max(gWidth, 1u), max(gHeight, 1u));
    const float2 origInv = rcp(nativeSize);
    const int2 origHi = int2((int)gWidth - 1, (int)gHeight - 1);
    const int2 op = clamp(int2(suv * nativeSize), int2(0, 0), origHi);

    float3 oL, oR, oU, oD;
    if (cmpZoom)
    {
        oL = max(SanitizeFinite3(OriginalInPassDomain(gOriginal.SampleLevel(gLinear, saturate(suv + float2(-origInv.x, 0.0)), 0)).rgb * invNorm, 0.0), 0.0);
        oR = max(SanitizeFinite3(OriginalInPassDomain(gOriginal.SampleLevel(gLinear, saturate(suv + float2( origInv.x, 0.0)), 0)).rgb * invNorm, 0.0), 0.0);
        oU = max(SanitizeFinite3(OriginalInPassDomain(gOriginal.SampleLevel(gLinear, saturate(suv + float2(0.0, -origInv.y)), 0)).rgb * invNorm, 0.0), 0.0);
        oD = max(SanitizeFinite3(OriginalInPassDomain(gOriginal.SampleLevel(gLinear, saturate(suv + float2(0.0,  origInv.y)), 0)).rgb * invNorm, 0.0), 0.0);
    }
    else
    {
        oL = max(SanitizeFinite3(OriginalInPassDomain(gOriginal.Load(int3(max(op + int2(-1, 0), int2(0, 0)), 0))).rgb * invNorm, 0.0), 0.0);
        oR = max(SanitizeFinite3(OriginalInPassDomain(gOriginal.Load(int3(min(op + int2( 1, 0), origHi), 0))).rgb * invNorm, 0.0), 0.0);
        oU = max(SanitizeFinite3(OriginalInPassDomain(gOriginal.Load(int3(max(op + int2(0, -1), int2(0, 0)), 0))).rgb * invNorm, 0.0), 0.0);
        oD = max(SanitizeFinite3(OriginalInPassDomain(gOriginal.Load(int3(min(op + int2(0,  1), origHi), 0))).rgb * invNorm, 0.0), 0.0);
    }

    const float3 rawL = SuperiorDirectSrRaw(oL);
    const float3 rawR = SuperiorDirectSrRaw(oR);
    const float3 rawU = SuperiorDirectSrRaw(oU);
    const float3 rawD = SuperiorDirectSrRaw(oD);

    const float3 pC = SrgbToLinear(saturate(nativeRaw));
    const float3 pL = SrgbToLinear(saturate(rawL));
    const float3 pR = SrgbToLinear(saturate(rawR));
    const float3 pU = SrgbToLinear(saturate(rawU));
    const float3 pD = SrgbToLinear(saturate(rawD));

    const float3 drL = rawL - nativeRaw;
    const float3 drR = rawR - nativeRaw;
    const float3 drU = rawU - nativeRaw;
    const float3 drD = rawD - nativeRaw;
    const float dotL = dot(drL, drL);
    const float dotR = dot(drR, drR);
    const float dotU = dot(drU, drU);
    const float dotD = dot(drD, drD);
    const float maxDot = max(max(dotL, dotR), max(dotU, dotD));

    // Compact-support quadratic edge weights: exact 0.0 bleed across hard coplanar edges, zero exp2().
    const float tL = saturate(1.0 - dotL * 10.0);
    const float tR = saturate(1.0 - dotR * 10.0);
    const float tU = saturate(1.0 - dotU * 10.0);
    const float tD = saturate(1.0 - dotD * 10.0);
    const float ewL = tL * tL;
    const float ewR = tR * tR;
    const float ewU = tU * tU;
    const float ewD = tD * tD;
    const float3 pLow4K = (2.0 * pC + ewL * pL + ewR * pR + ewU * pU + ewD * pD) / (2.0 + ewL + ewR + ewU + ewD);
    const float3 hf4K = pC - pLow4K;

    // Stage B2: If intermediate-resolution JBU already ran (bit 2 = preJbu, e.g. 2880x1620 at 4K),
    // skip all 4K depth loads: use 1-tap hardware bilinear on smooth areas (maxDot <= 0.008, ~90% of pixels)
    // and lightweight 2x2 color-only JBU on hard edges/silhouettes to preserve crisp 4K boundaries.
    const bool preJbu = (gGuideWidth & 4u) != 0u;
    float4 intr4K;
    [branch] if (preJbu)
    {
        [branch] if (maxDot <= 0.008)
        {
            const float4 rawMid = gModel.SampleLevel(gLinear, suv, 0);
            intr4K = float4(SanitizeFinite3(rawMid.rgb, 0.0), SanitizeFinite(rawMid.a, 1.0));
        }
        else
        {
            const float2 midPos = suv * float2(srcW, srcH) - 0.5;
            const int2 mBase = int2(floor(midPos));
            const float2 mf = saturate(midPos - float2(mBase));
            const int2 mMax = int2((int)srcW - 1, (int)srcH - 1);
            const float invMidColSig2 = 1.0 / (3.0 * 0.042 * 0.042);
            float4 mWeighted = 0.0;
            float mWeightSum = 0.0;
            [unroll] for (int my = 0; my <= 1; ++my)
            {
                const float wy = my != 0 ? mf.y : 1.0 - mf.y;
                [unroll] for (int mx = 0; mx <= 1; ++mx)
                {
                    const float wx = mx != 0 ? mf.x : 1.0 - mx;
                    const int2 mp = clamp(mBase + int2(mx, my), int2(0, 0), mMax);
                    const float3 mpCol = saturate(SanitizeFinite3(gSource.Load(int3(mp, 0)).rgb, nativeRaw));
                    const float4 mr = gModel.Load(int3(mp, 0));
                    const float4 mi = float4(SanitizeFinite3(mr.rgb, 0.0), SanitizeFinite(mr.a, 1.0));
                    const float3 md = mpCol - nativeRaw;
                    const float wc = exp2(-dot(md, md) * invMidColSig2);
                    const float w = (wx * wy) * (wc + 1e-5);
                    mWeighted += mi * w;
                    mWeightSum += w;
                }
            }
            intr4K = mWeighted / max(mWeightSum, 1e-5);
        }
    }
    else
    {
        // Fused native 4K Depth + Tangent-Plane Oracle (used at <=1440p or high work scales >=75%),
        // reading pre-linearized work-res oracleDepth from gMotion (t3) when bit 3 (haveWorkDepth) is set.
        const bool haveDepth = (gGuideWidth & 1u) != 0u;
        const uint depthInv = (gGuideWidth & 2u) != 0u ? 1u : 0u;
        const bool haveWorkDepth = (gGuideWidth & 8u) != 0u;
        const uint depthX = gExposureSourceWidth & 0xFFFFu;
        const uint depthY = (gExposureSourceWidth >> 16u) & 0xFFFFu;
        const uint depthW = gExposureSourceHeight & 0xFFFFu;
        const uint depthH = (gExposureSourceHeight >> 16u) & 0xFFFFu;
        const bool useDepth = haveDepth && depthW > 0u && depthH > 0u;

        const float kSkyEps = 1e-10;
        float zC = -1.0;
        float dzdx = 0.0;
        float dzdy = 0.0;
        int2 dcC = int2(0, 0);
        const int2 depthHi = int2((int)max(depthW, 1u) - 1, (int)max(depthH, 1u) - 1);
        const int2 depthOrigin = int2(depthX, depthY);
        const float2 depthSize = float2(max(depthW, 1u), max(depthH, 1u));
        if (useDepth)
        {
            const int2 dcLocal = clamp(int2(suv * depthSize), int2(0, 0), depthHi);
            dcC = depthOrigin + dcLocal;
            zC = GhostNearDepth(gSuperiorLow.Load(int3(dcC, 0)).x, depthInv);
            if (zC > kSkyEps)
            {
                const int2 dcL = depthOrigin + clamp(dcLocal + int2(-1, 0), int2(0, 0), depthHi);
                const int2 dcR = depthOrigin + clamp(dcLocal + int2( 1, 0), int2(0, 0), depthHi);
                const int2 dcU = depthOrigin + clamp(dcLocal + int2(0, -1), int2(0, 0), depthHi);
                const int2 dcD = depthOrigin + clamp(dcLocal + int2(0,  1), int2(0, 0), depthHi);
                const float zL = GhostNearDepth(gSuperiorLow.Load(int3(dcL, 0)).x, depthInv);
                const float zR = GhostNearDepth(gSuperiorLow.Load(int3(dcR, 0)).x, depthInv);
                const float zU = GhostNearDepth(gSuperiorLow.Load(int3(dcU, 0)).x, depthInv);
                const float zD = GhostNearDepth(gSuperiorLow.Load(int3(dcD, 0)).x, depthInv);
                const float maxSlope = 0.08 * zC + 1e-6;
                const bool gL = zL > kSkyEps;
                const bool gR = zR > kSkyEps;
                const bool gU = zU > kSkyEps;
                const bool gD = zD > kSkyEps;
                const float dL = gL ? (zC - zL) : 0.0;
                const float dR = gR ? (zR - zC) : 0.0;
                const bool okL = gL && (abs(dL) <= maxSlope);
                const bool okR = gR && (abs(dR) <= maxSlope);
                dzdx = (okL && okR) ? ((abs(dR) < abs(dL)) ? dR : dL) : (okR ? dR : (okL ? dL : 0.0));

                const float dU = gU ? (zC - zU) : 0.0;
                const float dD = gD ? (zD - zC) : 0.0;
                const bool okU = gU && (abs(dU) <= maxSlope);
                const bool okD = gD && (abs(dD) <= maxSlope);
                dzdy = (okU && okD) ? ((abs(dD) < abs(dU)) ? dD : dU) : (okD ? dD : (okU ? dU : 0.0));
            }
        }

        const float2 sourcePos = suv * float2(srcW, srcH) - 0.5;
        const int2 base = int2(floor(sourcePos));
        const float2 f = saturate(sourcePos - float2(base));
        const int2 maxCoord = int2((int)srcW - 1, (int)srcH - 1);
        const float2 workInv = rcp(float2(srcW, srcH));
        const float invColorSig2 = 1.0 / (3.0 * 0.042 * 0.042);

        float4 weighted = 0.0;
        float weightSum = 0.0;
        [unroll] for (int oy = 0; oy <= 1; ++oy)
        {
            const float wy = oy != 0 ? f.y : 1.0 - f.y;
            [unroll] for (int ox = 0; ox <= 1; ++ox)
            {
                const float wx = ox != 0 ? f.x : 1.0 - f.x;
                const int2 p = clamp(base + int2(ox, oy), int2(0, 0), maxCoord);
                const float3 proxyWork = saturate(SanitizeFinite3(gSource.Load(int3(p, 0)).rgb, nativeRaw));
                const float4 rawIntr = gModel.Load(int3(p, 0));
                const float4 intrWork = float4(SanitizeFinite3(rawIntr.rgb, 0.0), SanitizeFinite(rawIntr.a, 1.0));

                const float3 dCol = proxyWork - nativeRaw;
                const float wColor = exp2(-dot(dCol, dCol) * invColorSig2);

                float wPlane = 1.0;
                if (useDepth && zC > 0.0)
                {
                    const float2 tapUv = (float2(p) + 0.5) * workInv;
                    const int2 tapDc = clamp(int2(saturate(tapUv) * depthSize), int2(0, 0), depthHi) + depthOrigin;
                    const float zWork = haveWorkDepth
                        ? gMotion.Load(int3(p, 0)).x
                        : GhostNearDepth(gSuperiorLow.Load(int3(tapDc, 0)).x, depthInv);
                    if (zC > kSkyEps)
                    {
                        if (zWork > kSkyEps)
                        {
                            const float2 deltaDepth = float2(tapDc - dcC);
                            const float zPred = zC + dot(deltaDepth, float2(dzdx, dzdy));
                            const float zRel = (zWork - zPred) / max(0.018 * zC + 1e-6, 1e-6);
                            wPlane = exp2(-zRel * zRel);
                        }
                        else
                        {
                            wPlane = 0.0;
                        }
                    }
                    else
                    {
                        wPlane = (zWork > 0.0 && zWork <= kSkyEps) ? 1.0 : 0.0;
                    }
                }

                const float w = (wx * wy) * (wColor * wPlane + 1e-5);
                weighted += intrWork * w;
                weightSum += w;
            }
        }

        intr4K = weighted / max(weightSum, 1e-5);
    }

    // Stage C: Physical irradiance + brightness-normalized 4K micro-detail reconstruction.
    const float nrSharp = isfinite(gMvScaleX) ? saturate(gMvScaleX) : 0.65;
    const float detailStrength = isfinite(gMvScaleY) ? saturate(gMvScaleY) : 0.0;
    const float3 I_4K = exp2(clamp(intr4K.rgb, -4.3, 3.0));
    const float3 baseLin = (pLow4K + 0.04) * I_4K - 0.04;
    const float gRaw = clamp(intr4K.a, 0.60, 1.65);
    float gMod = lerp(1.0, gRaw, 0.35 + 0.65 * nrSharp);
    // Band-pass micro-detail mask when boosting detail (gMod > 1.0), without sqrt():
    // noise-floor Weber coring + hard coplanar edge guard (eliminates diagonal zipper/halo).
    if (gMod > 1.0)
    {
        const float pLowY = max(dot(pLow4K, kY), 0.0);
        const float hfSq = dot(hf4K, hf4K);
        const float denom = pLowY + 0.04;
        const float hfRelSq = hfSq / (denom * denom);
        const float hfCore = smoothstep(0.000225, 0.003025, hfRelSq);
        const float edgeGuard = 1.0 - smoothstep(0.045, 0.16, maxDot);
        gMod = lerp(1.0, gMod, hfCore * edgeGuard);
    }
    const float3 recLin = max(baseLin + hf4K * (I_4K * gMod), 0.0);
    float3 edit = LinearToSrgb(saturate(recLin)) - nativeRaw;

    // Optional edge-safe native micro-contrast lift (reuses oL/oR/oU/oD).
    [branch] if (detailStrength > 0.001)
    {
        const float safeOrigY = isfinite(origLuma) ? max(origLuma, 0.0) : 0.0;
        const float yL = dot(oL, kY);
        const float yR = dot(oR, kY);
        const float yU = dot(oU, kY);
        const float yD = dot(oD, kY);
        const float yAvg = 0.25 * (yL + yR + yU + yD);
        const float yRange = max(safeOrigY, max(max(yL, yR), max(yU, yD))) - min(safeOrigY, min(min(yL, yR), min(yU, yD)));
        const float yHigh = safeOrigY - yAvg;
        const float edgeGuard = 1.0 - smoothstep(0.14, 0.38, yRange);
        const float boostScale = (gPassthrough == 0u && gReversibleMode == 7u) ? 0.55 : 0.35;
        const float lift = clamp(yHigh * (detailStrength * boostScale * edgeGuard), -0.06, 0.06);
        edit += isfinite(lift) ? lift : 0.0;
    }

    return SanitizeFinite3(edit, 0.0);
}
float3 SuperiorGuidedEdit(float2 uvq, float3 nativeGuide, float normScale, float origLuma)
{
#if !defined(DLSSNR_SUPERIOR_GUIDED_RAW) && !defined(DLSSNR_SUPERIOR_HOST_PACKED)
    if (gGuideHeight == 2u)
        return SuperiorDirectSrEdit(uvq, nativeGuide, normScale, origLuma);
    if (gGuideHeight == 3u)
        return SuperiorIntrinsicOracleEdit(uvq, nativeGuide, normScale, origLuma);
#endif
    uint srcW, srcH;
    gSource.GetDimensions(srcW, srcH);
#if defined(DLSSNR_SUPERIOR_HOST_PACKED)
    // Map the native query directly to the packed lattice; no bilinear intermediate images.
    uvq = SuperiorPackedUv(uvq);
#else
    // Exact 1:1 bypass at native resolution (e.g. SuperiorAtNative): zero spatial blur.
    if (srcW == gWidth && srcH == gHeight)
    {
        const int2 p = clamp(int2(saturate(uvq) * float2(srcW, srcH)), int2(0, 0), int2((int)srcW - 1, (int)srcH - 1));
        float3 edit = gModel.Load(int3(p, 0)).rgb;
#if defined(DLSSNR_SUPERIOR_GUIDED_RAW)
        edit -= gSource.Load(int3(p, 0)).rgb;
#else
        if (gGuideHeight == 1u)
            edit -= gSource.Load(int3(p, 0)).rgb;
#endif
        return SuperiorShapeEdit(all(isfinite(edit)) ? edit : 0.0, uvq);
    }
#endif
    // 2x2 Bilinear-Tent Joint Bilateral Upsampling (JBU):
    // - Crisp rangeSigma (0.042 -> 0.036) keeps facial/skin micro-shading smooth (zero blotches)
    //   while snapping sharply to brick/armor/stone edges (visibly sharper than bilinear Matched Residual)
    //   and rejecting >99.9% of silhouette bleed.
    const float suppliedRange2 = asfloat(gExposureSourceWidth);
    const float invRange2 = isfinite(suppliedRange2) && suppliedRange2 > 0.0
        ? suppliedRange2 : 1.0 / (0.042 * 0.042);
    const float gaussianExp2 = 0.7213475204444817;
    const float2 sourcePos = saturate(uvq) * float2(srcW, srcH) - 0.5;
    const int2 base = int2(floor(sourcePos));
    const float2 f = saturate(sourcePos - float2(base));
    const int2 maxCoord = int2((int)srcW - 1, (int)srcH - 1);
    float3 weighted = 0.0;
    float weightSum = 0.0;
    [unroll] for (int oy = 0; oy <= 1; ++oy)
    {
        const float wy = oy != 0 ? f.y : 1.0 - f.y;
        [unroll] for (int ox = 0; ox <= 1; ++ox)
        {
            const float wx = ox != 0 ? f.x : 1.0 - f.x;
            const int2 p = clamp(base + int2(ox, oy), int2(0, 0), maxCoord);
            const float3 rawProxy = gSource.Load(int3(p, 0)).rgb;
            const float3 proxy = all(isfinite(rawProxy)) ? rawProxy : 1e4;
            float3 answer = gModel.Load(int3(p, 0)).rgb;
#if defined(DLSSNR_SUPERIOR_GUIDED_RAW)
            answer -= rawProxy;
#else
            if (gGuideHeight == 1u)
                answer -= rawProxy;
#endif
            const float3 residual = all(isfinite(answer)) ? answer : 0.0;
            const float3 colourDelta = proxy - nativeGuide;
            const float rangeDistance2 = dot(colourDelta, colourDelta) * (1.0 / 3.0);
            const float wRange = exp2(-gaussianExp2 * rangeDistance2 * invRange2) + 1e-3;
            const float w = wx * wy * wRange;
            weighted += residual * w;
            weightSum += w;
        }
    }
    float3 edit = weightSum > 1e-8 ? weighted / weightSum : 0.0;
    return SuperiorShapeEdit(edit, uvq);
}
