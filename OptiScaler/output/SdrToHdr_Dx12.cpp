// Not built with the precompiled header: Direct3D only, so tests/sdr_to_hdr_dx12_gpu.cpp compiles it alone.
#include "SdrToHdr_Dx12.h"

#include "../native/F5LowShaderBytecode.h"

namespace SdrToHdr
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

// The non-sRGB view of a proxy: the pass wants the encoded values the game wrote.
DXGI_FORMAT RawViewFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
} // namespace

void Dx12Pass::Release()
{
    // Nothing of ours may still be running on the GPU when it goes.
    if (_fence != nullptr && _fenceValue != 0 && _fence->GetCompletedValue() < _fenceValue && _event != nullptr &&
        SUCCEEDED(_fence->SetEventOnCompletion(_fenceValue, _event)))
        WaitForSingleObject(_event, 2000);

    ReleaseOf(_list);

    for (auto*& allocator : _allocators)
        ReleaseOf(allocator);

    ReleaseOf(_fence);
    ReleaseOf(_srvHeap);
    ReleaseOf(_rtvHeap);
    ReleaseOf(_pso);
    ReleaseOf(_rootSignature);

    if (_event != nullptr)
    {
        CloseHandle(_event);
        _event = nullptr;
    }

    for (auto& value : _submitted)
        value = 0;

    _fenceValue = 0;
    _device = nullptr;
    _format = DXGI_FORMAT_UNKNOWN;
    _bufferCount = 0;
}

const char* Dx12Pass::Init(ID3D12Device* device, DXGI_FORMAT realFormat, UINT bufferCount)
{
    if (device == nullptr || bufferCount == 0 || bufferCount > kMaxBuffers)
        return "the HDR pass was asked for an unsupported number of back buffers";

    if (realFormat != DXGI_FORMAT_R16G16B16A16_FLOAT && realFormat != DXGI_FORMAT_R10G10B10A2_UNORM)
        return "the HDR pass was asked for an unsupported output format";

    if (_pso != nullptr && _device == device && _format == realFormat && bufferCount <= _bufferCount)
        return nullptr;

    Release();
    _device = device;
    _format = realFormat;
    _bufferCount = bufferCount;

    // Root constants (b0, 8 dwords) and one table with the proxy (t0)
    D3D12_DESCRIPTOR_RANGE range {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = sizeof(PassConstants) / 4;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1].DescriptorTable.pDescriptorRanges = &range;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc {};
    rootDesc.NumParameters = 2;
    rootDesc.pParameters = parameters;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                     D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                     D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                     D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ID3DBlob* serialized = nullptr;

    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr)))
    {
        Release();
        return "the HDR pass's root signature could not be serialised";
    }

    const HRESULT rootResult = device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                           serialized->GetBufferSize(), IID_PPV_ARGS(&_rootSignature));
    serialized->Release();

    if (FAILED(rootResult))
    {
        Release();
        return "the HDR pass's root signature could not be made";
    }

    const auto* vs = F5LowShaderBytecode::Find("SdrToHdr", "FullScreenVS");
    const auto* ps = F5LowShaderBytecode::Find("SdrToHdr", "SdrToHdrPS");

    if (vs == nullptr || ps == nullptr)
    {
        Release();
        return "the HDR pass's shaders are missing";
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso {};
    pso.pRootSignature = _rootSignature;
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = realFormat;
    pso.SampleDesc = { 1, 0 };

    if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&_pso))))
    {
        Release();
        return "the HDR pass's pipeline could not be made";
    }

    D3D12_DESCRIPTOR_HEAP_DESC srvHeap {};
    srvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeap.NumDescriptors = bufferCount;
    srvHeap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeap {};
    rtvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeap.NumDescriptors = bufferCount;

    if (FAILED(device->CreateDescriptorHeap(&srvHeap, IID_PPV_ARGS(&_srvHeap))) ||
        FAILED(device->CreateDescriptorHeap(&rtvHeap, IID_PPV_ARGS(&_rtvHeap))))
    {
        Release();
        return "the HDR pass's descriptor heaps could not be made";
    }

    _srvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    _rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (UINT i = 0; i < bufferCount; ++i)
    {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_allocators[i]))))
        {
            Release();
            return "the HDR pass's command allocators could not be made";
        }
    }

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _allocators[0], nullptr,
                                         IID_PPV_ARGS(&_list))) ||
        FAILED(_list->Close()) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence))))
    {
        Release();
        return "the HDR pass's command list or fence could not be made";
    }

    _event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (_event == nullptr)
    {
        Release();
        return "the HDR pass's wait event could not be made";
    }

    return nullptr;
}

const char* Dx12Pass::Run(ID3D12CommandQueue* queue, UINT index, ID3D12Resource* proxy, ID3D12Resource* real,
                          const PassConstants& constants)
{
    if (_pso == nullptr)
        return "the HDR pass is not ready";

    if (queue == nullptr || proxy == nullptr || real == nullptr || index >= _bufferCount)
        return "the HDR pass was given nothing to convert";

    const D3D12_RESOURCE_DESC proxyDesc = proxy->GetDesc();
    const D3D12_RESOURCE_DESC realDesc = real->GetDesc();
    const DXGI_FORMAT view = RawViewFormat(proxyDesc.Format);

    if (view == DXGI_FORMAT_UNKNOWN)
        return "the HDR pass cannot read this back buffer format";

    if (realDesc.Format != _format || proxyDesc.Width != realDesc.Width || proxyDesc.Height != realDesc.Height)
        return "the HDR pass's back buffers differ in format or size";

    // This entry's last use must be finished before its allocator and descriptors are written again.
    if (_fence->GetCompletedValue() < _submitted[index])
    {
        if (FAILED(_fence->SetEventOnCompletion(_submitted[index], _event)) ||
            WaitForSingleObject(_event, 2000) != WAIT_OBJECT_0)
            return "an earlier HDR pass did not finish";
    }

    if (FAILED(_allocators[index]->Reset()) || FAILED(_list->Reset(_allocators[index], _pso)))
        return "the HDR pass's command list could not be reset";

    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = view;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = _srvHeap->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += (SIZE_T) index * _srvSize;
    _device->CreateShaderResourceView(proxy, &srv, srvCpu);

    D3D12_RENDER_TARGET_VIEW_DESC rtv {};
    rtv.Format = _format;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu = _rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvCpu.ptr += (SIZE_T) index * _rtvSize;
    _device->CreateRenderTargetView(real, &rtv, rtvCpu);

    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = _srvHeap->GetGPUDescriptorHandleForHeapStart();
    srvGpu.ptr += (UINT64) index * _srvSize;

    const D3D12_RESOURCE_BARRIER toRead[] = {
        Transition(proxy, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        Transition(real, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    _list->ResourceBarrier(2, toRead);

    _list->SetGraphicsRootSignature(_rootSignature);
    _list->SetDescriptorHeaps(1, &_srvHeap);
    _list->SetGraphicsRoot32BitConstants(0, sizeof(PassConstants) / 4, &constants, 0);
    _list->SetGraphicsRootDescriptorTable(1, srvGpu);
    _list->OMSetRenderTargets(1, &rtvCpu, FALSE, nullptr);

    D3D12_VIEWPORT viewport {};
    viewport.Width = (FLOAT) realDesc.Width;
    viewport.Height = (FLOAT) realDesc.Height;
    viewport.MaxDepth = 1.0f;
    _list->RSSetViewports(1, &viewport);

    const D3D12_RECT scissor { 0, 0, (LONG) realDesc.Width, (LONG) realDesc.Height };
    _list->RSSetScissorRects(1, &scissor);

    _list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    _list->DrawInstanced(3, 1, 0, 0);

    const D3D12_RESOURCE_BARRIER toRest[] = {
        Transition(proxy, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT),
        Transition(real, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT),
    };
    _list->ResourceBarrier(2, toRest);

    if (FAILED(_list->Close()))
        return "the HDR pass's command list could not be closed";

    ID3D12CommandList* lists[] = { _list };
    queue->ExecuteCommandLists(1, lists);
    _submitted[index] = ++_fenceValue;
    queue->Signal(_fence, _fenceValue);
    return nullptr;
}
} // namespace SdrToHdr
