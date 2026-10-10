#pragma once

// Direct3D 12 only (no OptiScaler headers but native/F5LowShaderBytecode.h), so tests/nr_depth_resolve_dx12_gpu.cpp
// builds it alone.
#include <d3d12.h>
#include <dxgiformat.h>

#include <cstdint>

namespace native
{
// The D3D12 depth finder's resolve of a multisampled depth copy (resource_tracking/GenericDepth_Dx12.cpp): each pixel
// gets the nearest of its samples (native/DepthResolve_Hlsl.h), written into a plain single-sample R32 texture the
// producer reads.
//
// It runs on a command list of its own, never in the game's: a game's list cannot be asked what root signature,
// pipeline and descriptor heaps it has bound, so a pass recorded there could not put them back. The game's list only
// copies the buffer as it is (same samples); this runs later on the queue the producer uses, before the producer, so
// the queue's order puts it after the game's lists that made the copies.
//
// Not thread-safe: the caller guards it.
class DepthResolveDx12
{
  public:
    static constexpr int kMaxJobs = 8;

    struct Job
    {
        ID3D12Resource* source = nullptr;             // multisampled, readable through `sourceView`
        DXGI_FORMAT sourceView = DXGI_FORMAT_UNKNOWN; // R32_FLOAT, R32_FLOAT_X8X24_TYPELESS, R24_UNORM_X8_TYPELESS...
        ID3D12Resource* target = nullptr; // single-sample, R32_TYPELESS or R32_FLOAT, unordered access allowed
    };

    DepthResolveDx12() = default;
    ~DepthResolveDx12() { Release(); }
    DepthResolveDx12(const DepthResolveDx12&) = delete;
    DepthResolveDx12& operator=(const DepthResolveDx12&) = delete;

    // Resolves `count` jobs (at most kMaxJobs) and submits them to `queue`. Sources must be in a state that includes
    // NON_PIXEL_SHADER_RESOURCE and are left there; targets rest in `targetState`, before and after. `reversed`: near
    // is 1, so the nearest sample is the largest. Returns nullptr when it was submitted, otherwise why not (a fixed
    // string, the same pointer for the same reason). Made on the queue's device the first time, and again when the
    // device changes.
    const char* Run(ID3D12CommandQueue* queue, const Job* jobs, int count, bool reversed,
                    D3D12_RESOURCE_STATES targetState);

    void Release();

  private:
    static constexpr int kRing = 3;

    const char* Init(ID3D12Device* device);

    ID3D12Device* _device = nullptr; // not owned: everything below is made on it
    ID3D12RootSignature* _rootSignature = nullptr;
    ID3D12PipelineState* _pso[2] = {};     // [reversed]
    ID3D12DescriptorHeap* _heap = nullptr; // kRing * kMaxJobs * 2: an SRV and a UAV per job
    UINT _descriptorSize = 0;
    ID3D12CommandAllocator* _allocators[kRing] = {};
    ID3D12GraphicsCommandList* _list = nullptr;
    ID3D12Fence* _fence = nullptr;
    HANDLE _event = nullptr;
    UINT64 _submitted[kRing] = {}; // the fence value each ring entry was last submitted with
    UINT64 _fenceValue = 0;
    int _next = 0;
    bool _failed = false; // Init failed on _device: not tried again until the device changes
};
} // namespace native
