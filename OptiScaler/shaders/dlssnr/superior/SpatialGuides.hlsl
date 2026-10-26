// Superior's continuous motion field. The original Compress shaders remain unchanged.
#define SPATIAL_GUIDES 1
#define CSMain UnusedSpatialMain
#include "../precompile/dlssnr_spatial.hlsl"
#undef CSMain

#include "SurfaceMotion.hlsli"

void SuperiorReconstructedGuides(uint3 id, bool packed)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;
    const float2 nativePixel =
        packed ? PwUnpack(float2(id.xy) + 0.5) : (float2(id.xy) + 0.5) * (gNativeWorkSize.xy / float2(gWidth, gHeight));
    const float2 uv = nativePixel / gNativeWorkSize.xy;
    const int2 depthLo = int2(gDepthRect.xy), motionLo = int2(gMotionRect.xy);
    const int2 dp = clamp(int2(gDepthRect.xy + uv * gDepthRect.zw), depthLo, int2(gDepthRect.xy + gDepthRect.zw) - 1);
    const int2 mp =
        clamp(int2(gMotionRect.xy + uv * gMotionRect.zw), motionLo, int2(gMotionRect.xy + gMotionRect.zw) - 1);
    const float depth = gPadding0.x > 0.5 ? gSource1.Load(int3(dp, 0)).x : 0.0;
    const float2 raw = SuperiorSurfaceMotion(nativePixel, mp, dp, depth);
    float2 motion = raw;
    if (packed)
    {
        const float2 v = raw * gMotionScale;
        motion = all(isfinite(v)) ? PwPack(nativePixel + v) - PwPack(nativePixel) : 0.0;
    }
    gTarget0[id.xy] = isfinite(depth) ? depth : (gUnused > 0.5 ? 0.0 : 1.0);
    gTarget1[id.xy] = all(isfinite(motion)) ? motion : 0.0;
}
[numthreads(8, 8, 1)] void SuperiorSurfaceGuidesMain(uint3 id : SV_DispatchThreadID)
{
    SuperiorReconstructedGuides(id, true);
}[numthreads(8, 8, 1)] void SuperiorUniformSurfaceGuidesMain(uint3 id : SV_DispatchThreadID)
{
    SuperiorReconstructedGuides(id, false);
}
