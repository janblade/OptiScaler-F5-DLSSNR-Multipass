#pragma once

// The pass's constants, shared by the Direct3D 12 and Direct3D 11 passes (no Direct3D here).
#include <cstdint>

namespace SdrToHdr
{
// The layout of b0 in output/SdrToHdr_Hlsl.h (8 dwords): root constants on D3D12, a 32 byte constant buffer on D3D11.
struct PassConstants
{
    uint32_t mode = 0;    // 0 = scRGB, 1 = PQ
    uint32_t convert = 1; // 1 = this conversion; 0 = the Windows baseline (piecewise sRGB at sdrWhiteNits)
    uint32_t decode = 0;  // 0 = gamma 2.2, 1 = sRGB curve (convert = 1)
    float gameNits = 203.0f;
    float peakNits = 1000.0f; // a clamp; the caller resolves 0 (SdrToHdr::ResolvePeakNits)
    float sdrWhiteNits = 80.0f;
    uint32_t srcLinear = 0; // 1: the proxy is a typed _SRGB resource, read through an _SRGB view (see SdrToHdr_Hlsl.h)
    float pad = 0.0f;
};
static_assert(sizeof(PassConstants) == 32, "b0 is 8 dwords");
} // namespace SdrToHdr
