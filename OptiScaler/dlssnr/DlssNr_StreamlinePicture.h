#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace DlssNr::StreamlinePicture
{
// Streamline's before-hook ABI (sl.api/internal.h). These access the app-facing FG buffers.
using GetIndex = UINT (*)(IDXGISwapChain*, bool&);
using GetBuffer = HRESULT (*)(IDXGISwapChain*, UINT, REFIID, void**, bool&);
inline constexpr GUID renderQueueKey = {
    0xc1eb6f13, 0x550c, 0x46ab, { 0xa8, 0x29, 0xf8, 0x72, 0x12, 0x69, 0x3a, 0xb1 }
};

inline constexpr GUID localRenderQueueKey = {
    0x2eb7837a, 0xa126, 0x4d10, { 0x9e, 0xf5, 0xc2, 0xf1, 0x4b, 0x41, 0x71, 0x7a }
};

inline Microsoft::WRL::ComPtr<ID3D12Resource> Read(IDXGISwapChain* swapchain, GetIndex indexHook, GetBuffer bufferHook)
{
    Microsoft::WRL::ComPtr<ID3D12Resource> picture;
    if (!swapchain || !indexHook || !bufferHook)
        return picture;
    bool handled = false;
    const UINT index = indexHook(swapchain, handled);
    if (!handled)
        return picture;
    handled = false;
    if (FAILED(bufferHook(swapchain, index, IID_PPV_ARGS(&picture), handled)) || !handled)
        picture.Reset();
    return picture; // Never substitute the asynchronous display buffer when the plugin declines.
}

inline Microsoft::WRL::ComPtr<ID3D12CommandQueue> RenderQueue(IDXGISwapChain* swapchain, bool local = false)
{
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    UINT size = sizeof(ID3D12CommandQueue*);
    if (swapchain &&
        FAILED(swapchain->GetPrivateData(local ? localRenderQueueKey : renderQueueKey, &size, queue.GetAddressOf())))
        queue.Reset();
    return queue;
}
using GetFunction = void* (*) (const char*);
// Only supported app-facing hooks own a chain. Native-input/lower presents must not edit it again.
bool Owns(IDXGISwapChain* swapchain);
bool NativeFgRunning();
// Lock-free registration availability for evaluate, which already holds the NR mutex.
bool PauseEvaluation();
void* Wrap(const char* name, GetFunction getFunction, bool local = false);
} // namespace DlssNr::StreamlinePicture
