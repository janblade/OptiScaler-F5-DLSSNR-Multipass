// Minification-only surface correction, after the unchanged Depth-aware pass.
#define SPATIAL_GUIDES 1
#define CSMain UnusedSpatialMain
#include "../precompile/dlssnr_spatial.hlsl"
#undef CSMain
#include "SurfaceMotion.hlsli"

// Exact pixel edges, not a frame-wide compression ratio or a center derivative.
// MV and depth remain tied to the same point-sampled query surface. In particular,
// averaging all vectors in this footprint would mix foreground and background.
bool MotionMinifiedBounds(float2 workPixel, bool packed, out int2 first, out int2 last)
{
    const float2 scale = gMotionRect.zw / gNativeWorkSize.xy;
    const float2 a =
        (packed ? PwUnpack(workPixel - 0.5) : (workPixel - 0.5) * (gNativeWorkSize.xy / float2(gWidth, gHeight))) *
        scale;
    const float2 b =
        (packed ? PwUnpack(workPixel + 0.5) : (workPixel + 0.5) * (gNativeWorkSize.xy / float2(gWidth, gHeight))) *
        scale;
    const int2 lo = int2(gMotionRect.xy), hi = int2(gMotionRect.xy + gMotionRect.zw) - 1;
    first = clamp(int2(floor(a)) + lo, lo, hi);
    last = clamp(int2(ceil(b)) - 1 + lo, first, hi);
    // Rounding the nonlinear map must not turn an effectively 1:1 region into a minifier.
    return any(b - a > 1.0001);
}

bool MotionMinifiedSurface(float2 workPixel, bool packed, DLSSNR_DEPTH_QUERY_TYPE queryDepthPixel, float queryDepth,
                           float2 gradient, out float2 selected)
{
    int2 first, last;
    selected = 0.0;
    if (!MotionMinifiedBounds(workPixel, packed, first, last))
        return false;

    // Search the complete covered lattice up to 8 texels per axis. Above that,
    // bounded stratification spans the entire footprint instead of clipping a
    // fixed neighborhood around its center (no unbounded per-pixel GPU loop).
    const int2 extent = last - first + 1;
    const int2 taps = min(extent, 8);
    bool found = false;
    float bestDistance = 3.402823466e+38;
    [loop] for (int y = 0; y < taps.y; ++y)
    {
        [loop] for (int x = 0; x < taps.x; ++x)
        {
            const int2 p = first + min(int2((float2(x, y) + 0.5) * float2(extent) / float2(taps)), extent - 1);
            const int2 dp = MotionDepthPixel(p);
            const float z = gSource1.Load(int3(dp, 0)).x;
            // De-trend sloping device depth before the format-aware residual test.
            const float predicted = z + dot(gradient, float2(queryDepthPixel - dp));
            if (!isfinite(z) ||
                abs(predicted - queryDepth) > MotionDepthTolerance(predicted, queryDepth, float2(queryDepthPixel - dp)))
                continue;
            const float2 v = gSource2.Load(int3(p, 0)).xy;
            if (!all(isfinite(v)))
                continue;
            const float2 delta = float2(dp - queryDepthPixel) / (gDepthRect.zw / gMotionRect.zw);
            const float distance = dot(delta, delta);
            if (!found || distance < bestDistance)
            {
                found = true;
                bestDistance = distance;
                selected = v;
            }
        }
    }
    return found;
}
