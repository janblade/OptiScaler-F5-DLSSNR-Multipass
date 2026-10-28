#pragma once

// The depth alignment (native/DepthAlign_Dx12.h): the game's raw depth point-sampled into the picture's coordinates,
// for a depth buffer that does not line up with the picture (a different size, scale or offset). Point sampling keeps
// the raw values: an edge between two surfaces stays an edge, never a depth between them. From PR #232
// (TechvibeCoder07). Its bytecode is in native/F5LowShaderBytecode.h (group "DepthAlign").

namespace DepthAlignHlsl
{
inline constexpr const char* kSource = R"HLSL(
Texture2D<float> Source : register(t0);
RWTexture2D<float> Target : register(u0);

cbuffer Settings : register(b0)
{
    float2 scale;  // the depth's uv is the picture's uv / scale + offset
    float2 offset;
    uint2 size;       // the picture's
    uint2 sourceSize; // the depth's
};

[numthreads(8, 8, 1)]
void Align(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= size))
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(size);
    float2 at = uv / scale + offset;
    uint2 p = min(uint2(saturate(at) * float2(sourceSize)), sourceSize - 1u);
    Target[id.xy] = Source.Load(int3(p, 0));
}
)HLSL";
} // namespace DepthAlignHlsl
