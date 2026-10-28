#pragma once

// The depth alignment for the native producer: the game's raw depth point-sampled into the picture's coordinates
// (native/DepthAlign_Hlsl.h) before the flow, the trust mask and the consumer read it. The scale and the offset are
// the menu's (DlssNr NativeDepthScaleX/Y, NativeDepthOffsetX/Y): the depth's uv = the picture's uv / scale +
// (-offsetX / width, offsetY / height), the offsets in the picture's pixels (the convention common depth add-ons
// use). The producer runs it only when the alignment is not the identity; its pipeline is made on the first frame
// that needs it (from precompiled bytecode). From PR #232 (TechvibeCoder07).

#include "F5LowShaderBytecode.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace native
{

class DepthAlignDx12
{
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

  public:
    static constexpr uint32_t kSlots = 3; // one descriptor pair per producer ring slot

    // True when `transform` (XY scale, XY offset in pixels) moves the depth at all.
    static bool Moves(const float* transform)
    {
        return transform != nullptr &&
               (transform[0] != 1.0f || transform[1] != 1.0f || transform[2] != 0.0f || transform[3] != 0.0f);
    }

    // Records the pass into `list`: `source` (readable by a compute shader, viewed as `view`) to an R32_FLOAT texture
    // of width x height, which it returns in the NON_PIXEL_SHADER_RESOURCE state; null when it could not (the caller
    // then runs without depth). `slot` is the producer's ring slot, whose earlier frame the GPU has finished.
    ID3D12Resource* Run(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* source, DXGI_FORMAT view,
                        uint32_t width, uint32_t height, uint32_t slot, const float* transform)
    {
        if (source == nullptr || width == 0 || height == 0 || slot >= kSlots || !Init(device))
            return nullptr;

        if (_target == nullptr || width != _width || height != _height)
        {
            D3D12_RESOURCE_DESC desc {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = width;
            desc.Height = height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R32_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            D3D12_HEAP_PROPERTIES heap {};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            ComPtr<ID3D12Resource> fresh;

            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&fresh))))
                return nullptr;

            fresh->SetName(L"NativeProducer_AlignedDepth");
            _target = fresh;
            _width = width;
            _height = height;
            _readable = false;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += (SIZE_T) slot * 2 * _increment;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = view;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(source, &srv, cpu);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpu = cpu;
        uavCpu.ptr += _increment;
        device->CreateUnorderedAccessView(_target.Get(), nullptr, &uav, uavCpu);

        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = _target.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        if (_readable)
            list->ResourceBarrier(1, &barrier);

        struct Constants
        {
            float scale[2], offset[2];
            uint32_t size[2], sourceSize[2];
        } constants {};

        for (int i = 0; i < 2; ++i)
            constants.scale[i] = std::isfinite(transform[i]) ? (std::max) (transform[i], 0.01f) : 1.0f;

        constants.offset[0] = std::isfinite(transform[2]) ? -transform[2] / (float) width : 0.0f;
        constants.offset[1] = std::isfinite(transform[3]) ? transform[3] / (float) height : 0.0f;
        constants.size[0] = width;
        constants.size[1] = height;
        const D3D12_RESOURCE_DESC sourceDesc = source->GetDesc();
        constants.sourceSize[0] = (uint32_t) sourceDesc.Width;
        constants.sourceSize[1] = sourceDesc.Height;

        ID3D12DescriptorHeap* heaps[] = { _heap.Get() };
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(_root.Get());
        list->SetPipelineState(_pipeline.Get());
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = _heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += (UINT64) slot * 2 * _increment;
        list->SetComputeRootDescriptorTable(0, gpu);
        list->SetComputeRoot32BitConstants(1, 8, &constants, 0);
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        _readable = true;
        return _target.Get();
    }

  private:
    bool Init(ID3D12Device* device)
    {
        if (_pipeline != nullptr)
            return true;

        if (device == nullptr)
            return false;

        D3D12_DESCRIPTOR_RANGE ranges[2] {};
        ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1 };
        D3D12_ROOT_PARAMETER params[2] {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = { 2, ranges };
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = { 0, 0, 8 };
        D3D12_ROOT_SIGNATURE_DESC rootDesc {};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = params;
        ComPtr<ID3DBlob> blob, error;

        if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
            FAILED(
                device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&_root))))
            return false;

        const auto* code = F5LowShaderBytecode::Find("DepthAlign", "Align");

        if (code == nullptr)
            return false;

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso {};
        pso.pRootSignature = _root.Get();
        pso.CS = { code->GetBufferPointer(), code->GetBufferSize() };

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = kSlots * 2;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_heap))) ||
            FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&_pipeline))))
        {
            _heap.Reset();
            _pipeline.Reset();
            return false;
        }

        _increment = device->GetDescriptorHandleIncrementSize(heapDesc.Type);
        return true;
    }

    ComPtr<ID3D12RootSignature> _root;
    ComPtr<ID3D12PipelineState> _pipeline;
    ComPtr<ID3D12DescriptorHeap> _heap;
    ComPtr<ID3D12Resource> _target;
    uint32_t _increment = 0, _width = 0, _height = 0;
    bool _readable = false;
};

} // namespace native
