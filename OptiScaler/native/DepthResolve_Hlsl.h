#pragma once

// The depth finders' resolve of a multisampled depth buffer (native/DepthCopyDx11.cpp on D3D11, native/DepthResolveDx12.cpp on
// D3D12): one value per pixel, the nearest of its samples, into a plain R32_FLOAT copy.
// Compiled ahead of time into native/F5LowShaderBytecode.h by tests/nr_shader_bytecode_smoke.cpp (run it with --write
// after changing anything here; without, it checks the embedded bytecode is current).

namespace DepthResolveHlsl
{
// The nearest sample, not an average: an average at an edge blends the surface in front with the one behind into a depth
// that belongs to neither, and the trust mask and detail reuse want edges to follow the surface in front. Nearest is the
// largest value with reversed Z (near is 1) and the smallest with standard Z, hence two entry points rather than a constant:
// the D3D11 pass binds no constant buffer of the game's slot (it only puts back what it touches).
inline constexpr const char* kSource = R"HLSL(
Texture2DMS<float> Src : register(t0);
RWTexture2D<float> Dst : register(u0);

float Nearest(uint2 p, bool reversed)
{
    uint w, h, samples;
    Src.GetDimensions(w, h, samples);
    float d = Src.Load(int2(p), 0);

    for (uint i = 1; i < samples; ++i)
    {
        const float s = Src.Load(int2(p), i);
        d = reversed ? max(d, s) : min(d, s);
    }

    return d;
}

[numthreads(8, 8, 1)]
void NearestReversed(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    Dst.GetDimensions(w, h);

    if (id.x < w && id.y < h)
        Dst[id.xy] = Nearest(id.xy, true);
}

[numthreads(8, 8, 1)]
void NearestStandard(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    Dst.GetDimensions(w, h);

    if (id.x < w && id.y < h)
        Dst[id.xy] = Nearest(id.xy, false);
}
)HLSL";
} // namespace DepthResolveHlsl
