#pragma once

#include "pch.h"

#include "GenericDepth_Select.h"

// Finds the scene's depth buffer in a Direct3D 11 game by watching how the game uses its depth-stencil views, with no ReShade
// and no help from an upscaler call: the D3D11 adapter of the native input producer (see docs/NATIVE-INPUT-ADAPTERS.md), built on
// the same native::DepthFinderCore as the D3D12 one (resource_tracking/GenericDepth_Dx12.h).
//
// D3D11 makes this simpler than D3D12 in two ways this file relies on: a depth-stencil view answers GetResource() directly (no
// descriptor tracking is needed), and almost every game draws through one context (the immediate context; deferred contexts are
// not watched yet, so a game that draws its scene only through one is not seen -- a per-game limitation, recorded as such).
//
// Off unless [DlssNr] NativeDepthFinder is set (checked at the device's first use, since OptiScaler attaches to a D3D11 game
// later than it does a D3D12 one). When on, the hooks observe ClearDepthStencilView, OMSetRenderTargets(AndUnorderedAccessViews),
// RSSetViewports and the draw calls on the immediate context, and copy the picked buffer into the finder's own textures there
// (native/DepthCopyDx11.h: the game's compute bindings it uses for that are put back; nothing else of the game's is changed).
// The hooks patch the functions, which every context of the kind shares: only the watched context's calls are counted.
namespace GenericDepthDx11
{
// Hooks the immediate context once a D3D11 device is known; a no-op when the key is off or it has run already.
void Install(ID3D11Device* device);

// Once per presented frame: closes the frame's counts, picks, and logs. The swap chain gives the picture's size, and its
// device the context watched: a device the game makes again is followed once it presents (a D3D12 swap chain, as under
// Dx11wDx12, says nothing, and the context Install found is kept).
void OnPresent(IDXGISwapChain* swapChain);

GenericDepthSelect::Pick CurrentPick();

// Copies of the picked depth buffer from the frame just presented, for the producer's adapter. One context means one copy (no
// split-scene case as D3D12 has across many command lists), so this always has at most one entry when it has any.
struct Snapshot
{
    bool valid = false;
    // A plain (non-shared) D3D11 copy; the adapter shares it across to D3D12 itself. Holds a reference, so the copy outlives
    // a replacement of it made while the adapter is still using this one.
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    DXGI_FORMAT typelessFormat = DXGI_FORMAT_UNKNOWN; // the copy's own (typeless) format, for making a shared copy of it
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN; // a typed format that reads its depth
    uint32_t width = 0;
    uint32_t height = 0;
    bool reversed = false; // near is 1.0
};

Snapshot BestSnapshot();

bool Installed();

// The key was on at this start but Install() gave up (the log says why).
bool InstallFailed();

// Called when the game creates an upscaler feature of its own: a game with one needs no depth finder, so it stands down for
// good (the same rule and wake-up as the D3D12 finder).
void NoteUpscalerCall();
bool GameCallsUpscaler();
bool Armed();

// ImGui: the menu's one depth line (off, needs a restart, could not start, stood down, watching, picked), no checkbox.
// Call inside the menu, on the Optical F5Low page (dlssnr/DlssNr_Menu.cpp).
void DrawStatus();

// ImGui: the "Use the game's depth" checkbox and its tooltip. [DlssNr] NativeDepthFinder is also turned on and off by
// the menu's mode selector (dlssnr/DlssNr_NativeMode.h); this is for setting it on its own. Call inside the menu's
// Advanced section.
void DrawAdvancedUi();
} // namespace GenericDepthDx11
