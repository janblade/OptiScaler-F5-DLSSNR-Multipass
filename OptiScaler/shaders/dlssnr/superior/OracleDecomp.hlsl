// Intrinsic 4K-Oracle Stage A: work-resolution proxy-bilateral intrinsic decomposition.
// Outputs:
//   u0 (gTarget): float4(logI.rgb, G_detail) -- regularized log-irradiance ratio & micro-detail gain
//   u1 (gKeep):   float4(proxyC, 1.0)        -- clean rectilinear oracleProxy for 4K JBU
#define gModel gUnusedOracleModel
#define gAux2 gUnusedOracleAux
#include "precompile/dlssnr.hlsl"
#undef gModel
#undef gAux2
#ifdef ORACLE_DECOMP_PACKED
#include "WarpMap.hlsli"
#endif

float3 OracleSrgbToLinear(float3 c)
{
    c = saturate(c);
    return lerp(c / 12.92, pow((c + 0.055) / 1.055, 2.4), step(0.04045, c));
}

void LoadOracleSample(int2 coord, int2 hi, float2 invSize, out float3 proxy, out float3 answer)
{
#ifdef ORACLE_DECOMP_PACKED
    const float2 uv = saturate((float2(clamp(coord, int2(0, 0), hi)) + 0.5) * invSize);
    const float2 packedUv = saturate(PwPack(uv * gNativeWorkSize.xy) / gNativeWorkSize.zw);
    proxy = saturate(SanitizeFinite3(gSource.SampleLevel(gLinear, packedUv, 0).rgb, 0.0));
    answer = saturate(SanitizeFinite3(gUnusedOracleModel.SampleLevel(gLinear, packedUv, 0).rgb, proxy));
#else
    const int3 p = int3(clamp(coord, int2(0, 0), hi), 0);
    proxy = saturate(SanitizeFinite3(gSource.Load(p).rgb, 0.0));
    answer = saturate(SanitizeFinite3(gUnusedOracleModel.Load(p).rgb, proxy));
#endif
}

[numthreads(8, 8, 1)]
void OracleDecompMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    const int2 hi = int2((int)gWidth - 1, (int)gHeight - 1);
    const float2 invSize = rcp(float2(max(gWidth, 1u), max(gHeight, 1u)));
    const int2 center = int2(id.xy);

    float3 proxyArr[9];
    float3 answerArr[9];
    [unroll] for (int oy = -1; oy <= 1; ++oy)
    {
        [unroll] for (int ox = -1; ox <= 1; ++ox)
        {
            const int idx = (oy + 1) * 3 + (ox + 1);
            LoadOracleSample(center + int2(ox, oy), hi, invSize, proxyArr[idx], answerArr[idx]);
        }
    }

    const float3 proxyC = proxyArr[4];
    const float3 proxyL = proxyArr[3];
    const float3 proxyR = proxyArr[5];
    const float3 proxyU = proxyArr[1];
    const float3 proxyD = proxyArr[7];

    // Gentle proxy-guided 1px isolated-spike anti-speckle on center edit before intrinsic decomposition.
    const float3 eC = answerArr[4] - proxyC;
    const float3 eL = answerArr[3] - proxyL;
    const float3 eR = answerArr[5] - proxyR;
    const float3 eU = answerArr[1] - proxyU;
    const float3 eD = answerArr[7] - proxyD;

    const float invSpkSig2 = 1.0 / (3.0 * 0.045 * 0.045);
    const float3 dpL = proxyL - proxyC;
    const float3 dpR = proxyR - proxyC;
    const float3 dpU = proxyU - proxyC;
    const float3 dpD = proxyD - proxyC;
    const float swL = exp2(-dot(dpL, dpL) * invSpkSig2);
    const float swR = exp2(-dot(dpR, dpR) * invSpkSig2);
    const float swU = exp2(-dot(dpU, dpU) * invSpkSig2);
    const float swD = exp2(-dot(dpD, dpD) * invSpkSig2);

    const float3 sL = swL > 0.25 ? eL : eC;
    const float3 sR = swR > 0.25 ? eR : eC;
    const float3 sU = swU > 0.25 ? eU : eC;
    const float3 sD = swD > 0.25 ? eD : eC;
    const float3 eMin4 = min(min(sL, sR), min(sU, sD));
    const float3 eMax4 = max(max(sL, sR), max(sU, sD));

    const float mL = swL > 0.25 ? swL : 0.0;
    const float mR = swR > 0.25 ? swR : 0.0;
    const float mU = swU > 0.25 ? swU : 0.0;
    const float mD = swD > 0.25 ? swD : 0.0;
    const float mSum = mL + mR + mU + mD;
    const float3 eAvg = mSum > 1e-4 ? (mL * eL + mR * eR + mU * eU + mD * eD) / mSum : eC;

    const float3 kY = float3(0.2126, 0.7152, 0.0722);
    const float3 dC = eC - eAvg;
    const float dY = dot(dC, kY);
    const float thresh = 0.25 * dY * dY;
    const float sameSignCount =
        ((swL > 0.25 && dot(eL - eAvg, kY) * dY > thresh) ? 1.0 : 0.0) +
        ((swR > 0.25 && dot(eR - eAvg, kY) * dY > thresh) ? 1.0 : 0.0) +
        ((swU > 0.25 && dot(eU - eAvg, kY) * dY > thresh) ? 1.0 : 0.0) +
        ((swD > 0.25 && dot(eD - eAvg, kY) * dY > thresh) ? 1.0 : 0.0);
    const float isolated = saturate(1.0 - 0.70 * sameSignCount);
    const float peak = max(abs(dY), max(abs(dC.r), max(abs(dC.g), abs(dC.b))));
    const float smallToMed = 1.0 - smoothstep(0.015, 0.040, peak);
    answerArr[4] = saturate(proxyC + lerp(eC, clamp(eC, eMin4, eMax4), isolated * smallToMed * 0.60));

    // Convert 3x3 neighborhood to linear proxy light and compute proxy-bilateral low-pass fields.
    const float invBilatSig2 = 1.0 / (3.0 * 0.055 * 0.055);
    float3 pLin[9];
    float3 aLin[9];
    float weights[9];
    float3 pLowSum = 0.0;
    float3 aLowSum = 0.0;
    float wSum = 0.0;
    float maxDpSq = 0.0;

    [unroll] for (int oy = -1; oy <= 1; ++oy)
    {
        const float wy = (oy == 0) ? 2.0 : 1.0;
        [unroll] for (int ox = -1; ox <= 1; ++ox)
        {
            const float wx = (ox == 0) ? 2.0 : 1.0;
            const int idx = (oy + 1) * 3 + (ox + 1);
            pLin[idx] = OracleSrgbToLinear(proxyArr[idx]);
            aLin[idx] = OracleSrgbToLinear(answerArr[idx]);
            const float3 dp = proxyArr[idx] - proxyC;
            const float dpSq = dot(dp, dp);
            maxDpSq = max(maxDpSq, dpSq);
            const float w = (wx * wy) * exp2(-dpSq * invBilatSig2);
            weights[idx] = w;
            pLowSum += pLin[idx] * w;
            aLowSum += aLin[idx] * w;
            wSum += w;
        }
    }

    const float invWSum = rcp(max(wSum, 1e-6));
    const float3 pLow = pLowSum * invWSum;
    const float3 aLow = aLowSum * invWSum;

    // Regularized low-frequency irradiance ratio (exact 1.0 when a == p).
    const float3 iLow = (aLow + 0.04) / (pLow + 0.04);

    // Perceptual sRGB-luminance micro-detail statistics (0 log2() calls in the 3x3 loop).
    float pY[9];
    float aY[9];
    float pLowYSum = 0.0;
    float aLowYSum = 0.0;
    [unroll] for (int i = 0; i < 9; ++i)
    {
        pY[i] = dot(proxyArr[i], kY);
        aY[i] = dot(answerArr[i], kY);
        pLowYSum += pY[i] * weights[i];
        aLowYSum += aY[i] * weights[i];
    }
    const float pLowYSrgb = pLowYSum * invWSum;
    const float aLowYSrgb = aLowYSum * invWSum;

    float epSq = 0.0;
    float eaSq = 0.0;
    float covPA = 0.0;
    [unroll] for (int i = 0; i < 9; ++i)
    {
        const float hp = pY[i] - pLowYSrgb;
        const float ha = aY[i] - aLowYSrgb;
        epSq += weights[i] * (hp * hp);
        eaSq += weights[i] * (ha * ha);
        covPA += weights[i] * (hp * ha);
    }
    const float eP = sqrt(max(epSq * invWSum, 0.0));
    const float eA = sqrt(max(eaSq * invWSum, 0.0));
    const float corr = (eP > 1e-5 && eA > 1e-5) ? clamp((covPA * invWSum) / (eP * eA), -1.0, 1.0) : 1.0;

    // Brightness-normalize sRGB micro-detail std with 1 scalar factor outside the loop (exact 1.0 when a == p).
    const float lumaI = clamp(dot(iLow, kY), 0.10, 4.0);
    const float aScale = rcp(pow(lumaI, 0.4545));
    const float eA_norm = eA * aScale;

    // Texture-gated, phase-aware micro-detail gain G_detail (exact 1.0 when a == p or on flat areas).
    // Thresholds are calibrated for bilateral-weighted sRGB std (sigma=0.055 caps eP around ~0.035).
    const float texConf = smoothstep(0.0045, 0.016, eP);
    const float gRaw = (eA_norm + 0.0085) / (eP + 0.0085);
    float gGainTarget = gRaw > 1.0 ? lerp(1.0, gRaw, saturate(corr * 1.4)) : gRaw;
    if (corr < -0.15)
        gGainTarget = min(gGainTarget, lerp(1.0, 0.65, saturate((-corr - 0.15) * 2.0)));
    const float gDetail = clamp(lerp(1.0, gGainTarget, texConf), 0.60, 1.65);

    // Preserve coherent NR-created detail on smooth/low-texture surfaces (and genuine phase-flipped relighting)
    // without injecting ViT phase-shift dipoles or false-positive injection on hard coplanar/silhouette edges.
    const float3 pHfC = pLin[4] - pLow;
    const float3 aHfC = aLin[4] - aLow;
    const float3 nrNewHf = aHfC - pHfC * iLow;
    const float edgeClear = 1.0 - smoothstep(0.008, 0.035, maxDpSq);
    const float smoothGate = (1.0 - smoothstep(0.012, 0.035, eP)) * edgeClear;
    const float flipGate = (1.0 - smoothstep(-0.35, 0.10, corr)) * smoothstep(0.010, 0.028, eA_norm) * edgeClear * 0.50;
    const float regimeGate = max(smoothGate, flipGate);
    const float coherenceGate = 1.0 - 0.85 * isolated * (1.0 - smoothstep(0.045, 0.11, peak));
    const float aLowY = max(dot(aLow, kY), 0.0);
    const float newMag = length(nrNewHf) / (aLowY + 0.04);
    const float coreGate = smoothstep(0.042, 0.115, newMag);
    const float nrInject = regimeGate * coherenceGate * coreGate * 0.72;
    const float3 nrNewBound = clamp(nrNewHf, -0.5 * (aLow + 0.04), 1.5 * (aLow + 0.04));
    const float3 aEff = max(aLow + nrNewBound * nrInject, 0.0);
    const float3 logI = clamp(log2((aEff + 0.04) / (pLow + 0.04)), -4.3, 3.0);

    const float2 jitter = float2(
        isfinite(gCompareSplit) ? clamp(gCompareSplit, -0.5, 0.5) : 0.0,
        isfinite(gCompareZoom) ? clamp(gCompareZoom, -0.5, 0.5) : 0.0);
    float3 proxyOut = proxyC;
    if (any(abs(jitter) > 1e-5))
    {
        const float2 unjitUv = saturate((float2(id.xy) + 0.5 - jitter) * invSize);
#ifdef ORACLE_DECOMP_PACKED
        const precise float2 unjitPacked = PwPack(unjitUv * gNativeWorkSize.xy);
        const float2 unjitPackedUv = saturate(unjitPacked / gNativeWorkSize.zw);
        proxyOut = SanitizeFinite3(gSource.SampleLevel(gLinear, unjitPackedUv, 0).rgb, proxyC);
#else
        proxyOut = SanitizeFinite3(gSource.SampleLevel(gLinear, unjitUv, 0).rgb, proxyC);
#endif
    }

    gTarget[id.xy] = float4(SanitizeFinite3(logI, 0.0), SanitizeFinite(gDetail, 1.0));
    gKeep[id.xy] = float4(saturate(proxyOut), 1.0);
}
