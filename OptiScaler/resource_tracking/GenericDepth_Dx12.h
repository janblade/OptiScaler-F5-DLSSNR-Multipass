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

// Copies of the picked depth buffer from the frame that was just presented, for the producer's later steps. A game that spreads
// its scene over several command lists, in a different split each frame, leaves each copy holding only part of the scene; the
// frame's copies are all given (copies[]), and the nearest surface over all of them is the scene whatever the split was.
// resource is the copy of the stretch that drew the most. It is made only while the debug overlay or [DlssNr] NativeMotion is on, and the copy is
// recorded into the game's own lists. The texture rests in NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE; it is
// overwritten by the next frame's copies, which run later on the same queue than anything recorded at present.
struct Snapshot
{
    static constexpr int kMaxCopies = 8;

    bool valid = false;
    ID3D12Resource* resource = nullptr;            // the copy of the stretch that drew the most (the menu's preview)
    ID3D12Resource* copies[kMaxCopies] = {};       // every copy of the frame, in no particular order
    int copyCount = 0;
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

// The key was on at this start but Install() gave up (the log says why).
bool InstallFailed();

// Called when the game creates an upscaler feature (every D3D12 input goes through FeatureProvider_Dx12::GetFeature): a game
// with an upscaler of its own needs no depth finder, so it stands down for good.
void NoteUpscalerCall();

// The game has called an upscaler within the last couple of seconds, whether or not the finder is installed
// (native::GameUpscalerCalledRecently, or the installed finder's own present-counted window).
bool GameCallsUpscaler();

// The warm-up is over and no upscaler call was seen: the finder is allowed to pick and report.
bool Armed();

// ImGui: the menu's one depth line (off, needs a restart, could not start, stood down, watching, picked). No checkbox.
// Call inside the menu, on the F5Low page (dlssnr/DlssNr_Menu.cpp).
void DrawStatus();

// ImGui: the "Use the game's depth" and "Show the picked depth here" checkboxes, and with [DlssNr] NativeDepthOverlay
// the picked depth buffer as a grayscale image. [DlssNr] NativeDepthFinder is also turned on and off by the menu's mode
// selector (dlssnr/DlssNr_NativeMode.h); this is for setting it on its own. Call inside the menu's Advanced section.
void DrawAdvancedUi();
} // namespace GenericDepthDx12
