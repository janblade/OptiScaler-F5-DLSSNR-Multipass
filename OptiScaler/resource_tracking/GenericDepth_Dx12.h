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

// A copy of the picked depth buffer as the frame that was just presented left it, for the producer's later steps. Of the
// stretches the game drew into the buffer, the one that drew the most, so it is the same stretch every frame whatever order
// the game's command lists ran in. It is made only while the debug overlay or [DlssNr] NativeMotion is on, and the copy is
// recorded into the game's own lists. The texture rests in NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE; it is
// overwritten by the next frame's copies, which run later on the same queue than anything recorded at present.
struct Snapshot
{
    bool valid = false;
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN; // a typed format that reads its depth: R32_FLOAT, R16_UNORM, ...
    uint32_t width = 0;
    uint32_t height = 0;
    bool reversed = false;                        // near is 1.0
    uint64_t frame = 0;
};

Snapshot BestSnapshot();

// Records the copy the menu's preview shows onto the menu's own list (call after it is reset, before it is submitted). Does
// nothing unless the overlay is on.
void RecordPreviewCopy(ID3D12GraphicsCommandList* list);

bool Installed();

// Called when the game creates an upscaler feature (every D3D12 input goes through FeatureProvider_Dx12::GetFeature): a game
// with an upscaler of its own needs no depth finder, so it stands down for good.
void NoteUpscalerCall();

// The game has called an upscaler within the last couple of seconds (valid when the finder is installed; false otherwise).
bool GameCallsUpscaler();

// The warm-up is over and no upscaler call was seen: the finder is allowed to pick and report.
bool Armed();

// ImGui: status of the finder, and with [DlssNr] NativeDepthOverlay the picked depth buffer as a grayscale image. Draws
// nothing when the finder is not installed. Call inside the menu.
void DrawDebugUi();
} // namespace GenericDepthDx12
