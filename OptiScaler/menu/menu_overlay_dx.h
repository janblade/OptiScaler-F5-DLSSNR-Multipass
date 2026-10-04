#pragma once

#include "SysUtils.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

namespace MenuOverlayDx
{
ID3D12GraphicsCommandList* MenuCommandList();
// A shader-visible descriptor from the menu's own heap, for showing a texture with ImGui::Image (the GPU handle is the
// ImTextureID). SrvHeap() changes when the menu is torn down and rebuilt: handles from the old one are then dead.
ID3D12DescriptorHeap* SrvHeap();
bool AllocSrv(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
void FreeSrv(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu);
void CleanupRenderTarget(bool clearQueue, HWND hWnd);
void Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
             const DXGI_PRESENT_PARAMETERS* pPresentParameters, IUnknown* pDevice, HWND hWnd, bool isUWP);
void ApplyThemeStyle();
} // namespace MenuOverlayDx
