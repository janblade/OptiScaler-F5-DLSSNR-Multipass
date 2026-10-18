#pragma once

// The exposure scan on D3D11 (plan 2026-10-14-exposure-scan-all-apis, story 3): the same candidates as on D3D12
// (DlssNr_ExposureScan.h) -- a small UAV texture or buffer the game makes -- found on the device's creation calls and
// read through staging copies on the immediate context. They join the one list the readout and the recording show.
//
// NR for a D3D11 game runs on D3D12 (the upscaler bridge, IFeature_Dx11wDx12, or the native producer through
// Dx11FrameSource), so that pass writes the frame's bookkeeping; this file only finds and reads.

struct ID3D11Device;
struct ID3D11Resource;
struct IDXGISwapChain;

namespace DlssNr
{
namespace ExposureScanDx11
{
// From the device's capture (hooks/D3D11_Hooks.cpp): detours the device's CreateBuffer and CreateTexture2D while the
// scan is wanted, so the buffers a game makes at load are seen. A scan switched on later installs at the next present
// and sees what is created from then on.
void Install(ID3D11Device* device);

// Once a frame at the present (the same two places the D3D11 depth finder closes its frame): reads back the values
// copied kSlots presents ago without waiting, and copies this frame's, on the presenting device's immediate context.
void OnPresent(IDXGISwapChain* swapChain);

// From the upscaler bridge (upscalers/IFeature_Dx11wDx12.cpp), before NR: the game's own D3D11 exposure texture, which
// NR then only sees as a D3D12 copy; the scan marks this one as the handed exposure.
void NoteHanded(ID3D11Resource* exposure);
} // namespace ExposureScanDx11
} // namespace DlssNr
