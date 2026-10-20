#pragma once

// The SDR-to-HDR pass (output/SdrToHdr_Dx12.cpp): the game's picture, read raw from the proxy back buffer, written as scRGB
// or HDR10 into the real one.
// Compiled ahead of time into native/F5LowShaderBytecode.h by tests/nr_shader_bytecode_smoke.cpp (run it with --write
// after changing anything here; without, it checks the embedded bytecode is current).
//
// The PQ curve and the BT.709 -> BT.2020 matrix are copied from shaders/dlssnr/precompile/dlssnr_pq.hlsli (the bytecode
// tool compiles one string, with no include handler, so the .hlsli cannot be included); the PQ encode here takes nits
// directly instead of that file's scRGB units.

namespace SdrToHdrHlsl
{
// One triangle that covers the target from the vertex number alone (no vertex buffer, no input layout). The pixel shader
// reads the proxy at the pixel's own position: proxy and real buffer are the same size.
//
//   t0  the proxy through a non-sRGB view (R8G8B8A8_UNORM or B8G8R8A8_UNORM): the encoded values the game wrote, whether
//       it drew through an _SRGB view or not (the hardware encodes on write, so memory holds encoded values either way)
//   b0  gMode 0 = scRGB (1.0 = 80 nits), 1 = PQ; gConvert 1 = this conversion, 0 = how Windows maps an SDR app on an
//       HDR desktop (the piecewise sRGB curve at the Windows SDR white level); gDecode 0 = gamma 2.2, 1 = sRGB curve
inline constexpr const char* kSource = R"HLSL(
Texture2D<float4> Src : register(t0);

cbuffer Constants : register(b0)
{
    uint gMode;
    uint gConvert;
    uint gDecode;
    float gGameNits;
    float gPeakNits;
    float gSdrWhiteNits;
    float2 gPad;
};

float4 FullScreenVS(uint id : SV_VertexID) : SV_Position
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float SrgbDecode1(float v)
{
    if (v <= 0.04045)
        return v / 12.92;

    return pow((v + 0.055) / 1.055, 2.4);
}

float3 SrgbDecode(float3 v)
{
    return float3(SrgbDecode1(v.r), SrgbDecode1(v.g), SrgbDecode1(v.b));
}

static const float3x3 kBt709To2020 = {
    0.6274039, 0.3292830, 0.0433131,
    0.0690973, 0.9195404, 0.0113623,
    0.0163914, 0.0880133, 0.8955953 };

// SMPTE ST 2084 inverse EOTF: light as a fraction of 10000 nits -> signal
float3 EncodePQ(float3 fraction)
{
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    const float3 p = pow(saturate(fraction), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

float4 SdrToHdrPS(float4 pos : SV_Position) : SV_Target
{
    const float3 c = saturate(Src.Load(int3((int2) pos.xy, 0)).rgb);

    float3 nits;

    if (gConvert != 0)
    {
        float3 lin;

        if (gDecode == 0)
            lin = pow(c, 2.2);
        else
            lin = SrgbDecode(c);

        nits = lin * gGameNits;
    }
    else
    {
        nits = SrgbDecode(c) * gSdrWhiteNits;
    }

    nits = min(nits, gPeakNits);

    if (gMode == 0)
        return float4(nits / 80.0, 1.0);

    return float4(EncodePQ(mul(kBt709To2020, nits) / 10000.0), 1.0);
}
)HLSL";
} // namespace SdrToHdrHlsl
