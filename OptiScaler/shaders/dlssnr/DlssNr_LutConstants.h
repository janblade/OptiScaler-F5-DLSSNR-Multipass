#pragma once

// Constants of precompile/dlssnr_lut.hlsl (Story 2 of the LUT-apply epic,
// dlssnr-lut-apply). Its own 256-byte slot in DlssNr_Dx12's dedicated
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
    // Fixed (Review Pass, 2026-10-04): InputEncoding alone cannot say this -- DlssNrColourEncoding's
    // ShaderConversion() collapses scene-linear HDR and tone-mapped sRGB into the same "else" value
    // (DlssNr_Common.h's own comment on ColourIsLinearHdr says so), so the shader was saturating raw,
    // open-ended linear light straight to 0-1 as if it were already display-referred. This carries the
    // real signal (frame.ColourIsLinearHdr) and Trim below carries what to divide it by first.
    uint32_t ColourIsLinearHdr;
    // DlssNr::AutoTrimEffective(cfg) -- the same global default (or known-game override) the rest of
    // DLSS-NR's own automatic exposure uses when nothing more specific has been measured yet. The real
    // per-frame measured white point isn't computed until later in Dispatch(), well after this pass
    // already ran; reusing the existing default is an approximation of that, not a duplicate of it, and
    // good enough for a pass whose own role (Context, "Reviewed 2026-10-04") is already an accepted
    // approximation -- grading the model's input, not reproducing its measured exposure exactly.
    float Trim;
};
static_assert(sizeof(DlssNrLutConstants) == 256);
