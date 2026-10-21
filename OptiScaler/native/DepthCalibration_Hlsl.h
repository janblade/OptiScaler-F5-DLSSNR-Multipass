#pragma once
namespace DepthCalibrationHlsl {
inline constexpr const char* source=R"HLSL(
Texture2D<float> Source : register(t0);
RWTexture2D<float> Target : register(u0);
cbuffer Settings : register(b0) { float2 scale; float2 offset; uint2 size; uint2 sourceSize; };
[numthreads(8,8,1)] void Align(uint3 id:SV_DispatchThreadID) {
 if(any(id.xy>=size))return;
 float2 uv=(float2(id.xy)+0.5)/float2(size);
 float2 t=uv/scale+offset;
 uint2 p=min(uint2(saturate(t)*float2(sourceSize)),sourceSize-1u);
 Target[id.xy]=Source.Load(int3(p,0));
}
)HLSL";
}
