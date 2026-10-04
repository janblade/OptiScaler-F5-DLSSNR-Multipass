#pragma once

// The native input producer's motion step, in a live game (Story 2 of the epic): while the game makes no upscaler call, the
// finished picture is run through OpticalFlowDx12 once per presented frame, on a command list of our own on the swap chain's
// queue, and the flow can be looked at in the DLSS-NR menu. Nothing is changed in the game's picture or lists.
//
// Switched on with [DlssNr] NativeMotion (the menu checkbox applies at once). It waits while the game calls an upscaler, the
// same condition the depth finder stands down for (and runs from the start when the finder is not installed).

#include <d3d12.h>
#include <dxgi.h>

namespace NativeMotionDx12
{
// Both close the depth finder's frame (GenericDepthDx12::OnPresent) and then run the step, so the step reads the frame just
// closed and the finder closes once per game frame.
//
// Once per presented frame, from the wrapped swap chain's present, before DLSS-NR's finished-picture pass (which composes
// from what an upscaler call captured this frame) and before the menu is drawn. `queue` is the swap chain's queue. Does
// nothing while OnFGPresent is being called.
void OnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, ID3D12Device* device);

// Once per game frame under frame generation, from FGHooks::FGPresent before frame generation presents and before it takes
// its lock: the picture is the game's own, `queue` the game's. Frame generation's Upscaler input is fed from here.
void OnFGPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, ID3D12Device* device);

// ImGui: the checkbox, a status line and the flow picture. Call inside the menu.
void DrawDebugUi();

} // namespace NativeMotionDx12
