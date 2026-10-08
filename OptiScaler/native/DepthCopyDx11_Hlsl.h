#pragma once

// The D3D11 depth copy's conversion shader (native/DepthCopyDx11.cpp).
// Compiled ahead of time into native/F5LowShaderBytecode.h by tests/nr_shader_bytecode_smoke.cpp (run it with --write
// after changing anything here; without, it checks the embedded bytecode is current).

namespace DepthCopyDx11Hlsl
{
// Reads the read copy (whatever its depth format family) through a single-channel view and writes the plain R32_FLOAT copy. A
// typeless depth-stencil format (R32G8X24_TYPELESS and the like) can fail to make a cross-API (D3D11<->D3D12) shared NT handle
// outright (CreateTexture2D returns E_INVALIDARG for such a format with D3D11_RESOURCE_MISC_SHARED_NTHANDLE, even though the same
// device shares an ordinary colour texture of that size), where a plain float texture shares without issue.
inline constexpr const char* kConvertSource = R"HLSL(
Texture2D<float> Src : register(t0);
RWTexture2D<float> Dst : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    Dst.GetDimensions(w, h);

    if (id.x >= w || id.y >= h)
        return;

    Dst[id.xy] = Src.Load(int3(id.xy, 0));
}
)HLSL";
} // namespace DepthCopyDx11Hlsl
