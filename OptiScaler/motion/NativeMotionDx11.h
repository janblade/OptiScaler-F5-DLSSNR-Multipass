#pragma once

// The native input producer's D3D11 driver: the Story B counterpart of motion/NativeMotion_Dx12.h. Runs native::NativeProducer
// (optical flow, trust mask, DLSS-NR) on a private D3D12 device paired with the game's D3D11 device, through
// native::Dx11FrameSource (the shared-texture transport) and resource_tracking/GenericDepth_Dx11.h (the depth finder).
//
// Switched on with the same [DlssNr] NativeMotion / NativeInput / NativeDebugView keys as the D3D12 driver; a game is one API or
// the other, so only one of the two drivers' OnPresent is ever fed real work.

#include <d3d11.h>
#include <dxgi.h>

namespace NativeMotionDx11
{
// Once per presented frame, from the menu's D3D11 present path.
void OnPresent(IDXGISwapChain* swapChain, ID3D11Device* device);

// ImGui: the checkbox, a status line and (with NativeDebugView) the flow/trust pictures. Call inside the menu.
void DrawDebugUi();

} // namespace NativeMotionDx11
