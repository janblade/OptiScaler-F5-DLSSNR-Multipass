// One max dilation on the quarter-work grid, then normalized-mask bilinear reconstruction.
// Fade is applied before spatial filtering so the fringe stays soft for long-lived timers.
#ifndef DLSSNR_COARSE_RADIUS
#define DLSSNR_COARSE_RADIUS 1
#endif
cbuffer Params : register(b0)
{
    uint mode;
    float frames;
    uint width, height;
    float fade;
};
Texture2D<uint2> State : register(t0);
RWTexture2D<float> Field : register(u0);
#ifdef DLSSNR_GHOST_CONTRAST
Texture2D<float4> Reference : register(t1);
RWTexture2D<float4> ExpandedReference : register(u1);
#endif
[numthreads(8, 8, 1)] void CoarseGhostDilate(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    uint w, h;
    State.GetDimensions(w, h);
    if (w == 0 || h == 0)
    {
        Field[id.xy] = 0;
        return;
    }
    uint age = 0;
#ifdef DLSSNR_GHOST_CONTRAST
    float4 chosenReference = 0;
#endif
    [unroll] for (int y = -DLSSNR_COARSE_RADIUS; y <= DLSSNR_COARSE_RADIUS; ++y)
    {
        [unroll] for (int x = -DLSSNR_COARSE_RADIUS; x <= DLSSNR_COARSE_RADIUS; ++x)
        {
            int2 q = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(w, h) - 1);
            uint candidate = State.Load(int3(q, 0)).x & 127u;
#ifdef DLSSNR_GHOST_CONTRAST
            [branch] if (candidate > age)
            {
                float4 reference = Reference.Load(int3(q, 0));
                if (all(isfinite(reference)) && reference.a > 0)
                {
                    age = candidate;
                    chosenReference = reference;
                }
            }
#else
            age = max(age, candidate);
#endif
        }
    }
    float f = isfinite(fade) ? clamp(fade, 0, 120) : 3;
    float mask = f > 0 ? saturate(float(age) / f) : age > 0 ? 1 : 0;
    // Store an exactly representable upward-rounded binary16, avoiding a smaller mask from truncation.
    uint bits = f32tof16(mask);
    if (f16tof32(bits) < mask)
        ++bits;
    Field[id.xy] = f16tof32(bits);
#ifdef DLSSNR_GHOST_CONTRAST
    ExpandedReference[id.xy] = chosenReference;
#endif
}
