// Immutable metadata in row zero; optional exact per-column/per-row FP32 UVs in rows one/two.
#define gNativeWorkSize gMotion.Load(int3(0, 0, 0))
#define gBandCenter gMotion.Load(int3(4, 0, 0))
#define gSideNeg gMotion.Load(int3(5, 0, 0))
#define gSidePos gMotion.Load(int3(6, 0, 0))
#define gSideWorkNeg gMotion.Load(int3(7, 0, 0))
#define gSideWorkPos gMotion.Load(int3(8, 0, 0))
#define gSideEdgeSlope gMotion.Load(int3(9, 0, 0))
#define gWorkScale gMotion.Load(int3(10, 0, 0))
#include "../precompile/dlssnr_spatial_warp.hlsli"
float2 SuperiorArithmeticPackedUv(float2 uv)
{
    // Keep the original evaluation order: one ULP can change the guided estimator's tap set.
    // gWidth/gHeight is the frame size the resolve / warp cache runs at; PwPack wants pixels of gNativeWorkSize.xy.
    // The host refuses a packed layout whose native size differs (superior/Pipeline.inl, Begin), so they are equal.
    precise float2 packedPixel = PwPack(uv * float2(gWidth, gHeight));
    return packedPixel / gNativeWorkSize.zw;
}
