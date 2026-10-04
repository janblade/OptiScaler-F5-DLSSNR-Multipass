#pragma once

#include "pch.h"

#include "GenericDepth_Select.h"

// Finds the scene's depth buffer in a DirectX 12 game by watching how the game uses its depth-stencil buffers, with no
// ReShade and no help from an upscaler call. The first stage of the native input producer (DLSS-NR in a game that has no
// DLSS/FSR/XeSS call); see GenericDepth_Select.h for the heuristic itself.
//
// Off unless [DlssNr] NativeDepthFinder is set (startup only). When off nothing is hooked. When on, the hooks only observe:
// CreateDepthStencilView and the descriptor copies (which resource a depth descriptor is), OMSetRenderTargets (which depth
// buffer a command list draws into), ClearDepthStencilView and the draw calls (how much). Nothing is changed.
namespace GenericDepthDx12
{
// Hooks the device and a command list once; a no-op when the key is off or it has run already.
void Install(ID3D12Device* device);

// Once per presented frame: closes the frame's counts, picks, and logs. The swap chain gives the picture's size.
void OnPresent(IDXGISwapChain* swapChain);

// The pick as of the last present: valid == false until a buffer qualifies.
GenericDepthSelect::Pick CurrentPick();

bool Installed();
} // namespace GenericDepthDx12
