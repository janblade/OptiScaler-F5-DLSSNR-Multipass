// Not built with the precompiled header: Direct3D only, so tests/nr_depth_resolve_dx12_gpu.cpp compiles it alone.
#include "DepthResolveDx12.h"

#include "F5LowShaderBytecode.h"

namespace native
{
namespace
{
template <class T> void ReleaseOf(T*& object)
{
    if (object != nullptr)
    {
        object->Release();
        object = nullptr;
    }
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}
} // namespace

void DepthResolveDx12::Release()
{
    // Nothing of ours may still be running on the GPU when it goes.
    if (_fence != nullptr && _fenceValue != 0 && _fence->GetCompletedValue() < _fenceValue && _event != nullptr &&
        SUCCEEDED(_fence->SetEventOnCompletion(_fenceValue, _event)))
        WaitForSingleObject(_event, 2000);

    ReleaseOf(_list);

    for (auto*& allocator : _allocators)
        ReleaseOf(allocator);

    ReleaseOf(_fence);
    ReleaseOf(_heap);

    for (auto*& pso : _pso)
        ReleaseOf(pso);

    ReleaseOf(_rootSignature);

    if (_event != nullptr)
    {
        CloseHandle(_event);
        _event = nullptr;
    }

    for (auto& value : _submitted)
        value = 0;

    _fenceValue = 0;
    _next = 0;
    _device = nullptr;
    _failed = false;
}

const char* DepthResolveDx12::Init(ID3D12Device* device)
{
    Release();
    _device = device;

    // One table: the multisampled source (t0) and the target (u0).
    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER parameter {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 2;
    parameter.DescriptorTable.pDescriptorRanges = ranges;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc {};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &parameter;

    ID3DBlob* serialized = nullptr;

    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr)))
    {
        _failed = true;
        return "the resolve's root signature could not be serialised";
    }

    const HRESULT rootResult = device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                           serialized->GetBufferSize(), IID_PPV_ARGS(&_rootSignature));
    serialized->Release();

    if (FAILED(rootResult))
    {
        _failed = true;
        return "the resolve's root signature could not be made";
    }

    for (int reversed = 0; reversed < 2; ++reversed)
    {
        const auto* code = F5LowShaderBytecode::Find("DepthResolve", reversed ? "NearestReversed" : "NearestStandard");

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso {};
        pso.pRootSignature = _rootSignature;

        if (code != nullptr)
            pso.CS = { code->GetBufferPointer(), code->GetBufferSize() };

        if (code == nullptr || FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&_pso[reversed]))))
        {
            _failed = true;
            return "the resolve's pipeline could not be made";
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = kRing * kMaxJobs * 2;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_heap))))
    {
        _failed = true;
        return "the resolve's descriptor heap could not be made";
    }

    _descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (auto*& allocator : _allocators)
    {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
        {
            _failed = true;
            return "the resolve's command allocators could not be made";
        }
    }

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _allocators[0], nullptr,
                                         IID_PPV_ARGS(&_list))) ||
        FAILED(_list->Close()) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence))))
    {
        _failed = true;
        return "the resolve's command list or fence could not be made";
    }

    _event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (_event == nullptr)
    {
        _failed = true;
        return "the resolve's wait event could not be made";
    }

    return nullptr;
}

const char* DepthResolveDx12::Run(ID3D12CommandQueue* queue, const Job* jobs, int count, bool reversed,
                                  D3D12_RESOURCE_STATES targetState)
{
    if (queue == nullptr || jobs == nullptr || count <= 0)
        return "nothing to resolve";

    if (count > kMaxJobs)
        count = kMaxJobs;

    ID3D12Device* device = nullptr;

    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
        return "the queue gave no device";

    device->Release(); // the queue keeps it alive

    if (device != _device)
    {
        if (const char* failed = Init(device))
            return failed;
    }
    else if (_failed)
    {
        return "the resolve could not be made on this device";
    }

    // The ring entry's last use must be finished before its allocator and descriptors are written again. Three frames
    // back, it nearly always is.
    const int entry = _next;
    _next = (_next + 1) % kRing;

    if (_fence->GetCompletedValue() < _submitted[entry])
    {
        if (FAILED(_fence->SetEventOnCompletion(_submitted[entry], _event)) ||
            WaitForSingleObject(_event, 2000) != WAIT_OBJECT_0)
            return "an earlier resolve did not finish";
    }

    if (FAILED(_allocators[entry]->Reset()) || FAILED(_list->Reset(_allocators[entry], _pso[reversed ? 1 : 0])))
        return "the resolve's command list could not be reset";

    const D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = _heap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = _heap->GetGPUDescriptorHandleForHeapStart();
    const UINT base = (UINT) entry * kMaxJobs * 2;

    D3D12_RESOURCE_BARRIER toWrite[kMaxJobs] {};

    for (int i = 0; i < count; ++i)
        toWrite[i] = Transition(jobs[i].target, targetState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    _list->ResourceBarrier((UINT) count, toWrite);
    _list->SetComputeRootSignature(_rootSignature);
    _list->SetDescriptorHeaps(1, &_heap);

    for (int i = 0; i < count; ++i)
    {
        const Job& job = jobs[i];
        const UINT slot = base + (UINT) i * 2;

        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = job.sourceView;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        device->CreateShaderResourceView(job.source, &srv, { cpuStart.ptr + (SIZE_T) slot * _descriptorSize });

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(job.target, nullptr, &uav,
                                          { cpuStart.ptr + (SIZE_T) (slot + 1) * _descriptorSize });

        _list->SetComputeRootDescriptorTable(0, { gpuStart.ptr + (UINT64) slot * _descriptorSize });

        const D3D12_RESOURCE_DESC desc = job.target->GetDesc();
        _list->Dispatch((UINT) ((desc.Width + 7) / 8), (desc.Height + 7) / 8, 1);
    }

    D3D12_RESOURCE_BARRIER toRest[kMaxJobs] {};

    for (int i = 0; i < count; ++i)
        toRest[i] = Transition(jobs[i].target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, targetState);

    _list->ResourceBarrier((UINT) count, toRest);

    if (FAILED(_list->Close()))
        return "the resolve's command list could not be closed";

    ID3D12CommandList* lists[] = { _list };
    queue->ExecuteCommandLists(1, lists);
    _submitted[entry] = ++_fenceValue;
    queue->Signal(_fence, _fenceValue);
    return nullptr;
}
} // namespace native
