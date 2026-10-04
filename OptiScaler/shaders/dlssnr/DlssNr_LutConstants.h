#pragma once

// Constants of precompile/dlssnr_lut.hlsl (Story 2 of the LUT-apply epic,
// memory/plans/2026-10-04-dlssnr-lut-apply.md). Its own 256-byte slot in DlssNr_Dx12's dedicated
// _lutConstantBuffers ring -- the LUT pass has its own root signature and descriptor table, not the shared
// one DlssNrConstants/DlssNrDetailReuseConstants go through, so this does not need to match either's layout.
// Still one ordered list of 4-byte scalars matching the HLSL Params cbuffer field for field, append only,
// both sides changed together (DEVELOPMENT.md rule 5, applied to this pass's own constants).

#include <cstdint>

struct alignas(256) DlssNrLutConstants
{
    uint32_t Width;
    uint32_t Height;
    float Strength;
    float DomainMinR, DomainMinG, DomainMinB;
    float DomainMaxR, DomainMaxG, DomainMaxB;
    uint32_t LutSize;
    uint32_t InputEncoding;
};
static_assert(sizeof(DlssNrLutConstants) == 256);
