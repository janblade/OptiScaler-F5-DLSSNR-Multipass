#pragma once

// Direct3D 12 only (no OptiScaler headers but native/F5LowShaderBytecode.h), so tests/sdr_to_hdr_dx12_gpu.cpp builds it
// alone.
#include "SdrToHdr_Pass.h"

#include <d3d12.h>
#include <dxgiformat.h>

#include <cstdint>

namespace SdrToHdr
{
// One full-screen draw per frame: the proxy back buffer the game drew in (R8G8B8A8 or B8G8R8A8, read raw through a UNORM
// view) -> the real back buffer (R16G16B16A16_FLOAT for scRGB, R10G10B10A2_UNORM for HDR10).
//
// It runs on a command list of its own, on the queue the game presents on, after the game's lists: both buffers rest in
// PRESENT (COMMON) before and after, as swap chain buffers do between frames.
//
// Not thread-safe: the caller guards it (the swap chain's present lock).
class Dx12Pass
{
  public:
    Dx12Pass() = default;
    ~Dx12Pass() { Release(); }
    Dx12Pass(const Dx12Pass&) = delete;
    Dx12Pass& operator=(const Dx12Pass&) = delete;

    // Makes the pipeline for `realFormat` and room for `bufferCount` back buffers (1..16). Returns nullptr when ready,
    // otherwise why not (a fixed string). Calling it again with the same device, format and a count that fits is a no-op.
    const char* Init(ID3D12Device* device, DXGI_FORMAT realFormat, UINT bufferCount);

    // Converts proxy -> real for back buffer `index` and submits it to `queue`. Returns nullptr when submitted.
    const char* Run(ID3D12CommandQueue* queue, UINT index, ID3D12Resource* proxy, ID3D12Resource* real,
                    const PassConstants& constants);

    // Waits (up to 2 s) until everything submitted so far has finished on the GPU.
    void WaitForIdle();

    // Waits for what was submitted, then lets go of everything.
    void Release();

    bool Ready() const { return _pso != nullptr; }

  private:
    static constexpr UINT kMaxBuffers = 16;

    ID3D12Device* _device = nullptr; // not owned
    DXGI_FORMAT _format = DXGI_FORMAT_UNKNOWN;
    UINT _bufferCount = 0;
    ID3D12RootSignature* _rootSignature = nullptr;
    ID3D12PipelineState* _pso = nullptr;
    ID3D12DescriptorHeap* _srvHeap = nullptr; // shader visible, one SRV per back buffer
    ID3D12DescriptorHeap* _rtvHeap = nullptr; // one RTV per back buffer
    UINT _srvSize = 0;
    UINT _rtvSize = 0;
    ID3D12CommandAllocator* _allocators[kMaxBuffers] = {};
    ID3D12GraphicsCommandList* _list = nullptr;
    ID3D12Fence* _fence = nullptr;
    HANDLE _event = nullptr;
    UINT64 _submitted[kMaxBuffers] = {}; // the fence value each back buffer's entry was last submitted with
    UINT64 _fenceValue = 0;
};
} // namespace SdrToHdr
