// Intrinsic 4K-Oracle Stage B2 (Intermediate Resolution between 4K and 2K, e.g. 2880x1620 at 4K / 2560x1440 at 3413p):
// Plane-aware depth + albedo 2x2 Joint Bilateral Upsample of outIntrinsic (workW x workH -> midW x midH).
// Inputs:
//   t0 (gSource):            oracleProxy    (workW x workH, RGBA16F)
//   t1 (gUnusedOracleModel): outIntrinsic   (workW x workH, RGBA16F = logI.rgb + G_detail)
//   t2 (gOriginal):          nativeProxy    (nativeW x nativeH, already tone-mapped [0,1] sRGB proxy for SDR & HDR)
//   t3 (gMotion):            oracleDepth    (workW x workH, R32F pre-linearized GhostNearDepth from Stage B1)
//   t5 (gUnusedOracleAux):   nativeDepth    (depthRect in gExposureSourceWidth / gExposureSourceHeight)
// Outputs:
//   u0 (gTarget):            oracleMid      (midW x midH, RGBA16F = logI.rgb + G_detail)
//   u1 (gKeep):              oracleMidProxy (midW x midH, R11G11B10_FLOAT = nativeProxy sampled at mid grid)
#define gModel gUnusedOracleModel
#define gAux2 gUnusedOracleAux
#include "precompile/dlssnr.hlsl"
#undef gModel
#undef gAux2
#include "GhostDepth.hlsli"

[numthreads(8, 8, 1)]
void OracleJbuMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    uint srcW, srcH;
    gSource.GetDimensions(srcW, srcH);
    if (srcW == 0u || srcH == 0u)
    {
        gTarget[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
        gKeep[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    const float2 midInv = rcp(float2(max(gWidth, 1u), max(gHeight, 1u)));
    const float2 suv = saturate((float2(id.xy) + 0.5) * midInv);

    // nativeProxy (g_nr.colorCopy) is already in the exact [0, 1] sRGB proxy domain matching oracleProxy (both SDR & HDR).
    const float3 nativeGuide = saturate(SanitizeFinite3(gOriginal.SampleLevel(gLinear, suv, 0).rgb, 0.0));

    const bool haveDepth = (gGuideWidth & 1u) != 0u;
    const uint depthInv = (gGuideWidth & 2u) != 0u ? 1u : 0u;
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
        zC = GhostNearDepth(gUnusedOracleAux.Load(int3(dcC, 0)).x, depthInv);
        if (zC > kSkyEps)
        {
            const int2 dcL = depthOrigin + clamp(dcLocal + int2(-1, 0), int2(0, 0), depthHi);
            const int2 dcR = depthOrigin + clamp(dcLocal + int2( 1, 0), int2(0, 0), depthHi);
            const int2 dcU = depthOrigin + clamp(dcLocal + int2(0, -1), int2(0, 0), depthHi);
            const int2 dcD = depthOrigin + clamp(dcLocal + int2(0,  1), int2(0, 0), depthHi);
            const float zL = GhostNearDepth(gUnusedOracleAux.Load(int3(dcL, 0)).x, depthInv);
            const float zR = GhostNearDepth(gUnusedOracleAux.Load(int3(dcR, 0)).x, depthInv);
            const float zU = GhostNearDepth(gUnusedOracleAux.Load(int3(dcU, 0)).x, depthInv);
            const float zD = GhostNearDepth(gUnusedOracleAux.Load(int3(dcD, 0)).x, depthInv);
            const float maxSlope = 0.08 * zC + 1e-6;
            const bool gL = zL > kSkyEps, gR = zR > kSkyEps, gU = zU > kSkyEps, gD = zD > kSkyEps;
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
            const float3 proxyWork = saturate(SanitizeFinite3(gSource.Load(int3(p, 0)).rgb, nativeGuide));
            const float4 rawIntr = gUnusedOracleModel.Load(int3(p, 0));
            const float4 intrWork = float4(SanitizeFinite3(rawIntr.rgb, 0.0), SanitizeFinite(rawIntr.a, 1.0));

            const float3 dCol = proxyWork - nativeGuide;
            const float wColor = exp2(-dot(dCol, dCol) * invColorSig2);

            float wPlane = 1.0;
            if (useDepth && zC > 0.0)
            {
                const float2 tapUv = (float2(p) + 0.5) * workInv;
                const int2 tapDc = clamp(int2(saturate(tapUv) * depthSize), int2(0, 0), depthHi) + depthOrigin;
                // Read pre-linearized work-res GhostNearDepth from Stage B1 (gMotion slot t3): L2-resident & bit-exact!
                const float zWork = gMotion.Load(int3(p, 0)).x;
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

    const float4 intrMid = weighted / max(weightSum, 1e-5);
    gTarget[id.xy] = float4(clamp(SanitizeFinite3(intrMid.rgb, 0.0), -4.3, 3.0),
                            clamp(SanitizeFinite(intrMid.a, 1.0), 0.60, 1.65));
    gKeep[id.xy] = float4(nativeGuide, 1.0);
}
