// Approximate outward feather: max-reduce the post-contrast alpha, then depth-protected bilinear reconstruction.
// Full-resolution original alpha is always retained. No full-resolution neighbour search or weighted dilation.
#include "GhostDepth.hlsli"
cbuffer Params : register(b0)
{
    uint mode;
    float radius1080;
    uint width, height;
    float globalRawWeight, depthTolerance;
    uint inverted;
    float depthPrecision;
    uint layers;
    uint2 reserved;
    uint step, fullHeight;
};
Texture2D<float4> Base : register(t0);
Texture2D<float2> Field : register(t1); // alpha and depth at the original maximum, before spatial enlargement
Texture2D<float4> Proxy : register(t2);
Texture2D<float> Depth : register(t3);
Texture2D<float4> Answer : register(t4);
#if DLSSNR_GHOST_FEATHER_STAGE == 0
RWTexture2D<float2> Output : register(u0);
#else
RWTexture2D<float4> Output : register(u0);
#endif
float Mask(float v) { return isfinite(v) ? saturate(v) : 0; }
float UpwardHalf(float v)
{
    uint bits = f32tof16(v);
    return f16tof32(bits + uint(f16tof32(bits) < v));
}
#if defined(DLSSNR_GHOST_FEATHER_RINGS) && DLSSNR_GHOST_FEATHER_STAGE == 0
groupshared float2 RingTile[256]; // 8x8 output, 3 expansion cells and one boundary-test halo per side
groupshared uint RingBoundary[256];
float2 RingBlock(int2 p)
{
    if (any(p < 0) || p.x >= int(width) || p.y >= int(height))
        return 0;
    uint fw, fh;
    Base.GetDimensions(fw, fh);
    float maximum = 0;
    uint2 chosen = uint2(p) * 4;
    [unroll] for (uint y = 0; y < 4; ++y)[unroll] for (uint x = 0; x < 4; ++x)
    {
        uint2 q = uint2(p) * 4 + uint2(x, y);
        if (q.x >= fw || q.y >= fh)
            continue;
        float a = Mask(Base.Load(int3(q, 0)).a);
        if (a > maximum)
        {
            maximum = a;
            chosen = q;
        }
    }
    float z = maximum > 0 ? GhostNearDepth(Depth.Load(int3(chosen, 0)), inverted) : -1;
    return z > 0 ? float2(maximum, z) : 0;
}
#endif
[numthreads(8, 8, 1)] void GhostOutwardFeather(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID,
                                               uint3 local : SV_GroupThreadID)
{
#if defined(DLSSNR_GHOST_FEATHER_RINGS) && DLSSNR_GHOST_FEATHER_STAGE == 0
    uint n = clamp(layers, 1u, 3u), margin = n + 1, tileWidth = 8 + 2 * margin;
    for (uint k = local.y * 8 + local.x; k < tileWidth * tileWidth; k += 64)
        RingTile[k] =
            RingBlock(int2(group.xy * 8) + int2(int(k % tileWidth) - int(margin), int(k / tileWidth) - int(margin)));
    GroupMemoryBarrierWithGroupSync();
    for (uint k = local.y * 8 + local.x; k < tileWidth * tileWidth; k += 64)
    {
        uint x = k % tileWidth, y = k / tileWidth;
        bool boundary = false;
        if (x > 0 && y > 0 && x + 1 < tileWidth && y + 1 < tileWidth && RingTile[k].x > 0)
        {
            [unroll] for (int dy = -1; dy <= 1; ++dy)[unroll] for (int dx = -1; dx <= 1; ++dx) boundary =
                boundary || RingTile[int(k) + dy * int(tileWidth) + dx].x <= 0;
        }
        RingBoundary[k] = boundary ? 1u : 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (id.x >= width || id.y >= height)
        return;
    float2 best = RingTile[(local.y + margin) * tileWidth + local.x + margin];
    [loop] for (int y = -int(n); y <= int(n); ++y)[loop] for (int x = -int(n); x <= int(n); ++x)
    {
        int k = (int(local.y) + int(margin) + y) * int(tileWidth) + int(local.x) + int(margin) + x;
        if (RingBoundary[k] == 0)
            continue;
        float2 sample = RingTile[k];
        float a = sample.x * (1 - float(max(abs(x), abs(y))) / float(n + 1));
        if (a > best.x)
            best = float2(a, sample.y);
    }
    Output[id.xy] = best;
#else
    if (id.x >= width || id.y >= height)
        return;
    int2 p = int2(id.xy);
    uint blockSize = clamp(step, 1u, 128u);
#if DLSSNR_GHOST_FEATHER_STAGE == 0
    uint fw, fh;
    Base.GetDimensions(fw, fh);
    float maximum = 0;
    uint2 chosen = uint2(p) * blockSize;
    [loop] for (uint y = 0; y < blockSize; ++y)[loop] for (uint x = 0; x < blockSize; ++x)
    {
        uint2 q = id.xy * blockSize + uint2(x, y);
        if (q.x >= fw || q.y >= fh)
            continue;
        float a = Mask(Base.Load(int3(q, 0)).a);
        if (a > maximum)
        {
            maximum = a;
            chosen = q;
        }
    }
    // One depth read for the winning raw sample; carry that depth all the way to final composition.
    float z = maximum > 0 ? GhostNearDepth(Depth.Load(int3(chosen, 0)), inverted) : -1;
    Output[p] = z > 0 ? float2(maximum, z) : 0;
#else
#ifdef DLSSNR_GHOST_FEATHER_INPLACE
    // Every thread owns one output pixel; neighbours are read only from the completed immutable small field.
#ifdef DLSSNR_GHOST_FEATHER_RINGS
    uint ew, eh;
    Field.GetDimensions(ew, eh);
    float maximum = Mask(Field.Load(int3(clamp(p / 4, int2(0, 0), int2(ew, eh) - 1), 0)).x);
#else
    float2 posEarly = (float2(p) + .5) / float(blockSize) - .5;
    int2 cornerEarly = int2(floor(posEarly));
    uint ew, eh;
    Field.GetDimensions(ew, eh);
    float maximum = 0;
    [unroll] for (int y = 0; y < 2; ++y)[unroll] for (int x = 0; x < 2; ++x) maximum =
        max(maximum, Mask(Field.Load(int3(clamp(cornerEarly + int2(x, y), int2(0, 0), int2(ew, eh) - 1), 0)).x));
#endif
    if (maximum <= 0)
        return;
    float4 base = Output[p];
#else
    float4 base = Base.Load(int3(p, 0));
#endif
    float original = Mask(base.a), expanded = original;
    float z = GhostNearDepth(Depth.Load(int3(p, 0)), inverted);
    float tolerance = isfinite(depthTolerance) ? clamp(depthTolerance, .001, .5) : .05;
    float precision = isfinite(depthPrecision) && depthPrecision >= 0 && depthPrecision <= .001
                          ? max(depthPrecision * 2, 1e-6)
                          : 2.0 / 65535;
#ifdef DLSSNR_GHOST_FEATHER_RINGS
    uint fw, fh;
    Field.GetDimensions(fw, fh);
    float2 sample = Field.Load(int3(clamp(p / 4, int2(0, 0), int2(fw, fh) - 1), 0));
    if (z > 0 && sample.y > 0 && z <= sample.y * (1 + tolerance) + precision)
        expanded = max(original, UpwardHalf(Mask(sample.x)));
#else
    [branch] if (isfinite(radius1080) && radius1080 > 0 && blockSize > 1 && z > 0 && original < 1)
    {
        float2 pos = (float2(p) + .5) / float(blockSize) - .5;
        int2 corner = int2(floor(pos));
        float2 f = frac(pos);
        uint fw, fh;
        Field.GetDimensions(fw, fh);
        float sum = 0;
        [unroll] for (int y = 0; y < 2; ++y)[unroll] for (int x = 0; x < 2; ++x)
        {
            int2 q = clamp(corner + int2(x, y), int2(0, 0), int2(fw, fh) - 1);
            float2 sample = Field.Load(int3(q, 0));
            if (sample.y > 0 && z <= sample.y * (1 + tolerance) + precision)
                sum += Mask(sample.x) * (x ? f.x : 1 - f.x) * (y ? f.y : 1 - f.y);
        }
        expanded = max(original, UpwardHalf(saturate(sum)));
    }
#endif
    float g = isfinite(globalRawWeight) ? saturate(globalRawWeight) : 0;
    expanded = max(original, min(expanded, 1 - g));
    // Base RGB already contains g + original of raw edit. Add only the extra rejection fraction.
    [branch] if (expanded > original)
    {
        float3 proxy = Proxy.Load(int3(p, 0)).rgb, answer = Answer.Load(int3(p, 0)).rgb;
        proxy =
            float3(isfinite(proxy.r) ? proxy.r : 0, isfinite(proxy.g) ? proxy.g : 0, isfinite(proxy.b) ? proxy.b : 0);
        answer = float3(isfinite(answer.r) ? answer.r : proxy.r, isfinite(answer.g) ? answer.g : proxy.g,
                        isfinite(answer.b) ? answer.b : proxy.b);
        float3 raw = clamp(answer - proxy, -65504, 65504);
        float weight = saturate((expanded - original) / max(1e-6, 1 - g - original));
        base.rgb = lerp(base.rgb, raw, weight);
    }
#ifdef DLSSNR_GHOST_FEATHER_INPLACE
    if (expanded <= original)
        return;
#endif
    Output[p] = float4(base.rgb, expanded);
#endif
#endif
}
