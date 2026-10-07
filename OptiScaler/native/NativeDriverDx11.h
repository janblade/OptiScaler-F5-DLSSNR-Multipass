#pragma once

// The native input producer's D3D11 driver: the Story B counterpart of native/NativeDriverDx12.h. Runs native::NativeProducer
// (optical flow, trust mask, DLSS-NR) on a private D3D12 device paired with the game's D3D11 device, through
// native::Dx11FrameSource (the shared-texture transport) and resource_tracking/GenericDepth_Dx11.h (the depth finder).
//
// Switched on with the same [DlssNr] NativeMotion / NativeInput / NativeDebugView keys as the D3D12 driver; a game is one API or
// the other, so only one of the two drivers' OnPresent is ever fed real work.

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>

namespace NativeMotionDx11
{
// Once per presented frame, from the menu's D3D11 present path. Does nothing while the game's swap chain has been
// replaced by with_dx12::Dx11wDx12SC (frame generation with FGInput=Upscaler for a D3D11 game): that path is never
// reached then, and OnFGPresent below is the one actually driving this.
void OnPresent(IDXGISwapChain* swapChain, ID3D11Device* device);

// Once per real game frame, from Dx11wDx12SC::Present (with_dx12/dx11_with_dx12_sc.cpp), before frame generation
// takes the frame: `real` is the hidden D3D11 swap chain carrying the game's own picture (not what is shown). Runs
// the depth finder's frame close (MenuOverlayDx::Present skips its own on this path) and the producer. Returns the
// D3D12-side picture the producer ended up with (native::Dx11FrameSource::ProcessedPicture), or null when the
// feature is off or nothing could be processed this frame -- the caller copies it into the real D3D12 back buffer
// itself; this function does not touch frame generation's swap chain.
ID3D12Resource* OnFGPresent(IDXGISwapChain* real, ID3D11Device* device);

// ImGui: one status line for the mode in effect (waiting/failed, NR or the stabiliser on this picture, or why not).
// Call inside the menu, on the F5Low page (dlssnr/DlssNr_Menu.cpp), while NativeMotion
// is on.
void DrawStatus();

// NR is running on the finished picture (the menu header's "F5Low: NR on the finished picture").
bool NrOnlyRunning();

// ImGui: with NativeDebugView, the flow tuning sliders, the frame count and the trust mask's state. The keys themselves
// are set by the menu's mode selector (dlssnr/DlssNr_NativeMode.h). Call inside the menu's Advanced section.
void DrawAdvancedUi();

} // namespace NativeMotionDx11
