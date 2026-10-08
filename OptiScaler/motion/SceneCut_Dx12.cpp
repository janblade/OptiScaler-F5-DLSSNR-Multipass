// Not built with the precompiled header: self-contained so the host GPU tests can compile it alone.
#include "SceneCut_Dx12.h"

#include "../native/F5LowShaderBytecode.h"

#include <algorithm>

namespace
{

constexpr uint32_t kDescriptorsPerPass = 4; // the source (t0), and the state or luma, the flag, the distrust (u0..u2)
constexpr uint32_t kPassesPerFrame = 3;     // luma, histograms, divergence
constexpr uint32_t kFramesInFlight = 16;
constexpr uint32_t kHeapSize = kDescriptorsPerPass * kPassesPerFrame * kFramesInFlight;
constexpr DXGI_FORMAT kLumaFormat = DXGI_FORMAT_R32_FLOAT;
constexpr DXGI_FORMAT kStateFormat = DXGI_FORMAT_R32_UINT;     // typed atomics on this are required of every device
constexpr DXGI_FORMAT kDistrustFormat = DXGI_FORMAT_R8_UNORM; // typed UAV stores of this are required too
constexpr uint32_t kStateRows = 19; // nine tiles' counts, nine previous histograms, one row of scratch

} // namespace

SceneCutDx12::~SceneCutDx12() { Release(); }

void SceneCutDx12::Release()
{
    for (Tex* tex : { &_state, &_flag, &_distrust, &_luma })
    {
        if (tex->resource != nullptr)
            tex->resource->Release();
        *tex = Tex {};
    }

    for (Retired& retired : _retired)
    {
        if (retired.resource != nullptr)
            retired.resource->Release();
        retired = Retired {};
    }

    for (ID3D12PipelineState** pso : { &_hist, &_diverge, &_lumaPso[0], &_lumaPso[1], &_lumaPso[2] })
    {
        if (*pso != nullptr)
            (*pso)->Release();
        *pso = nullptr;
    }

    if (_root != nullptr)
        _root->Release();
    if (_heap != nullptr)
        _heap->Release();
    _root = nullptr;
    _heap = nullptr;
    _device = nullptr;
    _prevValid = false;
}

bool SceneCutDx12::Init(ID3D12Device* device)
{
    if (device == nullptr)
    {
        _error = "no device";
        return false;
    }

    Release();
    _device = device;

    // One table (the source, then three UAVs) and the constants.
    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 3;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER params[2] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = sizeof(Constants) / 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc {};
    rootDesc.NumParameters = 2;
    rootDesc.pParameters = params;

    ID3DBlob* serialized = nullptr;
    ID3DBlob* messages = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &messages);

    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                         IID_PPV_ARGS(&_root));

    if (serialized != nullptr)
        serialized->Release();
    if (messages != nullptr)
        messages->Release();

    if (FAILED(hr))
    {
        _error = "creating the scene-cut root signature";
        return false;
    }

    struct Entry
    {
        const char* name;
        const char* lumaMode;
        ID3D12PipelineState** target;
    };

    // The luma in each encoding: the same entry point built with the flow's LUMA_MODE for it (1 SDR, 2 scRGB, 3 PQ).
    for (const Entry& entry : { Entry { "SceneHist", "", &_hist }, Entry { "SceneDiverge", "", &_diverge },
                                Entry { "SceneLuma", "1", &_lumaPso[(int) Encoding::Srgb] },
                                Entry { "SceneLuma", "2", &_lumaPso[(int) Encoding::ScRgb] },
                                Entry { "SceneLuma", "3", &_lumaPso[(int) Encoding::Pq] } })
    {
        const auto* code = F5LowShaderBytecode::Find("SceneCut", entry.name, entry.lumaMode);

        if (code == nullptr)
        {
            _error = std::string("no bytecode for SceneCut ") + entry.name;
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso {};
        pso.pRootSignature = _root;
        pso.CS = { code->GetBufferPointer(), code->GetBufferSize() };

        hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(entry.target));
        code->Release();

        if (FAILED(hr))
        {
            _error = std::string("creating the pipeline ") + entry.name;
            return false;
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap {};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = kHeapSize;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&_heap))))
    {
        _error = "creating the scene-cut descriptor heap";
        return false;
    }

    _descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // A committed resource starts zeroed, which the histogram counts rely on.
    if (!CreateTexture(_state, 256, kStateRows, kStateFormat, L"SceneCut_State") ||
        !CreateTexture(_flag, 2, 1, kStateFormat, L"SceneCut_Flag") ||
        !CreateTexture(_distrust, 1, 1, kDistrustFormat, L"SceneCut_Distrust"))
    {
        _error = "creating the scene-cut textures";
        return false;
    }

    return true;
}

bool SceneCutDx12::CreateTexture(Tex& tex, uint32_t width, uint32_t height, DXGI_FORMAT format, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    tex.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    if (FAILED(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, tex.state, nullptr,
                                                IID_PPV_ARGS(&tex.resource))))
        return false;

    tex.resource->SetName(name);
    tex.width = width;
    tex.height = height;
    return true;
}

void SceneCutDx12::Transition(ID3D12GraphicsCommandList* list, Tex& tex, D3D12_RESOURCE_STATES state)
{
    if (tex.state == state)
        return;

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = tex.resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = tex.state;
    barrier.Transition.StateAfter = state;
    list->ResourceBarrier(1, &barrier);
    tex.state = state;
}

// One pass: `source` at t0; u0 is `target` (the luma pass) or the state; u1 the flag, u2 the distrust.
void SceneCutDx12::Pass(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, ID3D12Resource* source,
                        DXGI_FORMAT format, Tex* target, uint32_t groupsX, uint32_t groupsY, const Constants& constants)
{
    // The state stays a UAV for good (the passes only write it); the flag and the distrust are UAVs while the passes run
    // and textures after.
    Transition(list, target != nullptr ? *target : _state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list, _flag, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list, _distrust, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const UINT first = _heapCursor;
    _heapCursor = (_heapCursor + kDescriptorsPerPass) % kHeapSize;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = _heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T) first * _descriptorSize;
    gpu.ptr += (UINT64) first * _descriptorSize;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    _device->CreateShaderResourceView(source, &srv, cpu);
    cpu.ptr += _descriptorSize;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = target != nullptr ? kLumaFormat : kStateFormat;
    _device->CreateUnorderedAccessView(target != nullptr ? target->resource : _state.resource, nullptr, &uav, cpu);
    cpu.ptr += _descriptorSize;
    uav.Format = kStateFormat;
    _device->CreateUnorderedAccessView(_flag.resource, nullptr, &uav, cpu);
    cpu.ptr += _descriptorSize;
    uav.Format = kDistrustFormat;
    _device->CreateUnorderedAccessView(_distrust.resource, nullptr, &uav, cpu);

    ID3D12DescriptorHeap* heaps[] = { _heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(_root);
    list->SetPipelineState(pso);
    list->SetComputeRootDescriptorTable(0, gpu);
    list->SetComputeRoot32BitConstants(1, sizeof(Constants) / 4, &constants, 0);
    list->Dispatch(groupsX, groupsY, 1);

    // The next pass reads what this one wrote.
    if (target != nullptr)
    {
        Transition(list, *target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        return;
    }

    D3D12_RESOURCE_BARRIER barriers[3] {};
    for (int i = 0; i < 3; ++i)
        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = _state.resource;
    barriers[1].UAV.pResource = _flag.resource;
    barriers[2].UAV.pResource = _distrust.resource;
    list->ResourceBarrier(3, barriers);
}

bool SceneCutDx12::Detect(ID3D12GraphicsCommandList* list, ID3D12Resource* luma, float threshold)
{
    if (_device == nullptr || _diverge == nullptr || list == nullptr || luma == nullptr)
        return false;

    const D3D12_RESOURCE_DESC desc = luma->GetDesc();

    if (desc.Width < 3 || desc.Height < 3)
        return false;

    Constants constants {};
    constants.sizeX = (uint32_t) desc.Width;
    constants.sizeY = desc.Height;
    constants.hasPrevious = _prevValid ? 1 : 0;
    constants.threshold = threshold;

    // Eight groups for each of the nine tiles count; one group per tile then compares.
    Pass(list, _hist, luma, kLumaFormat, nullptr, 8, 9, constants);
    Pass(list, _diverge, luma, kLumaFormat, nullptr, 9, 1, constants);

    Transition(list, _flag, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(list, _distrust, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    _prevValid = true;
    return true;
}

bool SceneCutDx12::DetectColor(ID3D12GraphicsCommandList* list, ID3D12Resource* color, DXGI_FORMAT colorFormat,
                               Encoding encoding, float whiteNits, float threshold)
{
    if (_device == nullptr || list == nullptr || color == nullptr)
        return false;

    const D3D12_RESOURCE_DESC desc = color->GetDesc();
    const uint32_t width = (uint32_t) desc.Width;
    const uint32_t height = desc.Height;
    const uint32_t step = (std::max)(1u, (width + kColorLumaWidth - 1) / kColorLumaWidth);
    const uint32_t lumaWidth = (width + step - 1) / step;
    const uint32_t lumaHeight = (height + step - 1) / step;

    if (lumaWidth < 3 || lumaHeight < 3)
        return false;

    // The luma textures a size change replaced, released once that many more frames were recorded: work in flight on a
    // game's queue may still read them, and nothing here knows when it ends.
    for (Retired& retired : _retired)
        if (retired.resource != nullptr && --retired.frames == 0)
        {
            retired.resource->Release();
            retired.resource = nullptr;
        }

    // A new size: new luma, and its histograms are not comparable with the last ones.
    if (_luma.resource == nullptr || _luma.width != lumaWidth || _luma.height != lumaHeight)
    {
        if (_luma.resource != nullptr)
        {
            Retired* slot = &_retired[0];
            for (Retired& retired : _retired)
                if (retired.resource == nullptr || retired.frames < slot->frames)
                    slot = &retired;
            if (slot->resource != nullptr) // every slot taken (sizes changing every frame): the oldest goes now
                slot->resource->Release();
            *slot = Retired { _luma.resource, kFramesInFlight };
        }

        _luma = Tex {};
        _prevValid = false;

        if (!CreateTexture(_luma, lumaWidth, lumaHeight, kLumaFormat, L"SceneCut_Luma"))
        {
            _error = "creating the scene-cut luma";
            return false;
        }
    }

    Constants constants {};
    constants.sizeX = lumaWidth;
    constants.sizeY = lumaHeight;
    constants.colorX = width;
    constants.colorY = height;
    constants.step = step;
    constants.whiteNits = (std::max)(whiteNits, 1.0f);
    Pass(list, _lumaPso[(int) encoding], color, colorFormat, &_luma, (lumaWidth + 7) / 8, (lumaHeight + 7) / 8,
         constants);

    return Detect(list, _luma.resource, threshold);
}
