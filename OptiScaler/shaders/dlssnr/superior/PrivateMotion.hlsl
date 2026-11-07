Texture2D<float2> Motion : register(t0);
RWTexture2D<uint2> Result : register(u0);
cbuffer Params : register(b0)
{
    uint mode;
    float unused;
    uint width, height;
    float unused0, unused1;
    uint originX;
    float unused2;
    uint unused3;
    float scaleX, scaleY;
    uint regionWidth, regionHeight, originY;
};
groupshared uint Counts[256];

[numthreads(256, 1, 1)] void SampleMotion(uint lane : SV_GroupIndex)
{
    uint w, h;
    Motion.GetDimensions(w, h);
    bool fits = originX <= w && originY <= h && regionWidth > 0 && regionHeight > 0 && regionWidth <= w - originX &&
                regionHeight <= h - originY;
    if (!fits)
    {
        if (lane == 0)
            Result[uint2(0, 0)] = 0;
        return;
    }
    uint count = 0;
    for (uint i = lane; i < 576; i += 256)
    {
        uint2 cell = uint2(i % 32, i / 32);
        uint2 at = (cell * 2 + 1) * uint2(regionWidth, regionHeight) / uint2(64, 36) + uint2(originX, originY);
        float2 value = Motion.Load(int3(at, 0)) * float2(scaleX, scaleY);
        if (all(isfinite(value)) && all(abs(value) < 1e6))
            count += dot(value, value) > .1 * .1 ? 65537u : 1u;
    }
    Counts[lane] = count;
    GroupMemoryBarrierWithGroupSync();
    for (uint step = 128; step != 0; step >>= 1)
    {
        if (lane < step)
            Counts[lane] += Counts[lane + step];
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0)
        Result[uint2(0, 0)] = uint2(Counts[0] & 65535u, Counts[0] >> 16u);
}
