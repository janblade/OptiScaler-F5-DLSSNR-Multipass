// Not built with the precompiled header: self-contained so the host GPU test can compile it alone.
#include "OpticalFlow_Dx12.h"

#include "../native/F5LowShaderBytecode.h"

#include <algorithm>
#include <cstring>

namespace
{

constexpr uint32_t kDescriptorsPerPass = 13; // eight SRVs, flow/age UAVs and three optional trace UAVs
constexpr uint32_t kPassesPerFrame = 1 + 2 + (OpticalFlowDx12::kLevels - 1) + OpticalFlowDx12::kLevels + 1 + 1 + 1 + 1 + 1;
constexpr uint32_t kFramesInFlight = 8;
constexpr DXGI_FORMAT kLumaFormat = DXGI_FORMAT_R32_FLOAT;
constexpr DXGI_FORMAT kFlowFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; // typed UAV stores of this are required of every device
constexpr DXGI_FORMAT kStateFormat = DXGI_FORMAT_R32_UINT; // typed atomics on this are required of every device
constexpr DXGI_FORMAT kAgeFormat = DXGI_FORMAT_R8_UINT;    // the still age: typed UAV stores of this are required too

// The shaders are compiled ahead of time (motion/OpticalFlow_Hlsl.h -> native/F5LowShaderBytecode.h): a compile here
// ran on the present thread when Optical F5Low started. `lumaMode` is LUMA_MODE's value for a Luma variant.
const F5LowShaderBytecode::Shader* Compile(const char* entry, std::string* error, const char* group = "OpticalFlow",
                                           const char* lumaMode = "")
{
    const auto* shader = F5LowShaderBytecode::Find(group, entry, lumaMode);

    if (shader == nullptr)
        *error = std::string("no bytecode for ") + group + " " + entry;

    return shader;
}

} // namespace

OpticalFlowDx12::~OpticalFlowDx12()
{
    ReleaseTextures();

    for (ID3D12PipelineState** pso : { &_luma, &_lumaSdr, &_lumaScRgb, &_lumaPq, &_down, &_match, &_median, &_smooth,
                                       &_visualise, &_global, &_cameraModel })
        if (*pso != nullptr)
            (*pso)->Release();

    for(unsigned i=1;i<8;++i)if(_matchVariants[i])_matchVariants[i]->Release();
    for(auto* p:_traceVariants)if(p)p->Release();
    for(auto* p:{_medianEvidence,_smoothEvidence,_smoothDiagnostics})if(p)p->Release();

    if (_rootSignature != nullptr)
        _rootSignature->Release();
    if (_heap != nullptr)
        _heap->Release();
}

bool OpticalFlowDx12::Init(ID3D12Device* device)
{
    if (device == nullptr)
    {
        _error = "no device";
        return false;
    }

    _device = device;
    wchar_t dred[2] {};
    _debugEvents = GetEnvironmentVariableW(L"F5LOW_DRED", dred, 2) == 1 && dred[0] == L'1';

    // One table (eight SRVs, two UAVs) and the root constants.
    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 8;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 5;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 8;

    D3D12_ROOT_PARAMETER params[2] {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = sizeof(Constants) / 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc {};
    rootDesc.NumParameters = 2;
    rootDesc.pParameters = params;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &sampler;

    ID3DBlob* serialized = nullptr;
    ID3DBlob* messages = nullptr;

    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &messages)))
    {
        _error = "serializing the root signature";
        if (messages != nullptr)
            messages->Release();
        return false;
    }

    const HRESULT rootResult = device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                           serialized->GetBufferSize(), IID_PPV_ARGS(&_rootSignature));
    serialized->Release();

    if (messages != nullptr)
        messages->Release();

    if (FAILED(rootResult))
    {
        _error = "creating the root signature";
        return false;
    }

    struct Entry
    {
        const char* name;
        ID3D12PipelineState** target;
    };

    for (const Entry& entry : { Entry { "Luma", &_luma }, Entry { "Down", &_down }, Entry { "Match", &_match },
                                Entry { "Median", &_median }, Entry { "Smooth", &_smooth },
                                Entry { "Global", &_global }, Entry { "CameraModel", &_cameraModel } })
    {
        const auto* code = Compile(entry.name, &_error);

        if (code == nullptr)
            return false;

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso {};
        pso.pRootSignature = _rootSignature;
        pso.CS = { code->GetBufferPointer(), code->GetBufferSize() };

        const HRESULT hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(entry.target));
        code->Release();

        if (FAILED(hr))
        {
            _error = std::string("creating the pipeline ") + entry.name;
            return false;
        }
    }

    // The luma in the three other ways (see LumaOf): the same entry point built with another LUMA_MODE.
    {
        struct Variant
        {
            const char* mode;
            ID3D12PipelineState** target;
        };

        for (const Variant& variant :
             { Variant { "1", &_lumaSdr }, Variant { "2", &_lumaScRgb }, Variant { "3", &_lumaPq } })
        {
            const auto* code = Compile("Luma", &_error, "OpticalFlow", variant.mode);

            if (code == nullptr)
                return false;

            D3D12_COMPUTE_PIPELINE_STATE_DESC pso {};
            pso.pRootSignature = _rootSignature;
            pso.CS = { code->GetBufferPointer(), code->GetBufferSize() };

            const HRESULT hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(variant.target));
            code->Release();

            if (FAILED(hr))
            {
                _error = "creating the pipeline Luma";
                return false;
            }
        }
    }

    if (!_sceneCut.Init(device))
    {
        _error = _sceneCut.Error();
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap {};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = kDescriptorsPerPass * kPassesPerFrame * kFramesInFlight;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&_heap))))
    {
        _error = "creating the descriptor heap";
        return false;
    }

    _descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // All bytecode is generated offline. Selecting an experiment never compiles
    // HLSL on a present thread, and feature zero uses the upstream-sized shader.
    auto variant=[&](const char* entry,const char* key,ID3D12PipelineState** target){
        const auto* code=Compile(entry,&_error,"OpticalFlow",key);
        if(!code)return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};d.pRootSignature=_rootSignature;
        d.CS={code->GetBufferPointer(),code->GetBufferSize()};
        return SUCCEEDED(device->CreateComputePipelineState(&d,IID_PPV_ARGS(target)));
    };
    for(unsigned i=1;i<8;++i){const auto key=std::to_string(i);if(!variant("Match",key.c_str(),&_matchVariants[i]))return false;}
    if(!variant("Median","4",&_medianEvidence)||!variant("Smooth","4",&_smoothEvidence)||
       !variant("Smooth","32",&_smoothDiagnostics))return false;

    return true;
}

bool OpticalFlowDx12::CreateTexture(Tex& tex, uint32_t width, uint32_t height, DXGI_FORMAT format, const wchar_t* name)
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

void OpticalFlowDx12::ReleaseTextures()
{
    for(auto& tex:_trace){if(tex.resource)tex.resource->Release();tex=Tex{};}
    _traceValid=false;
    auto release = [](Tex& tex)
    {
        if (tex.resource != nullptr)
            tex.resource->Release();
        tex = Tex {};
    };

    for (auto& set : _pyramid)
        for (auto& tex : set)
            release(tex);

    for (auto& set : _levelFlow)
        for (auto& tex : set)
            release(tex);

    release(_flowMedian);
    release(_globalFlow);
    release(_cameraFlow);
    release(_flow);
    release(_preview);
    release(_depthPreview);
    _depthPreviewActive=false;

    for (Tex& age : _age)
        release(age);
}

bool OpticalFlowDx12::EnsureSize(uint32_t width, uint32_t height)
{
    if (width == _width && height == _height && _flow.resource != nullptr)
        return true;

    // The previous work may still be reading these; a size change is rare (a resolution change) and the caller is expected
    // to have waited, as it does before replacing its own targets.
    ReleaseTextures();
    _havePrevious = false;
    _globalReady = false;
    _sceneCut.Reset();
    _sceneCutRan = false;
    _ageValid = false;
    _flowValid = false;
    _width = width;
    _height = height;

    uint32_t w = (width + 1) / 2;
    uint32_t h = (height + 1) / 2;

    for (int level = 0; level < kLevels; ++level)
    {
        for (int set = 0; set < 2; ++set)
            if (!CreateTexture(_pyramid[set][level], w, h, kLumaFormat, L"OpticalFlow_Luma"))
                return false;

        for (int set = 0; set < 2; ++set)
            if (!CreateTexture(_levelFlow[set][level], w, h, kFlowFormat, L"OpticalFlow_LevelFlow"))
                return false;

        w = (w + 1) / 2;
        h = (h + 1) / 2;
    }

    for (Tex& age : _age)
        if (!CreateTexture(age, (width + 1) / 2, (height + 1) / 2, kAgeFormat, L"OpticalFlow_StillAge"))
            return false;

    return CreateTexture(_flowMedian, (width + 1) / 2, (height + 1) / 2, kFlowFormat, L"OpticalFlow_FlowMedian") &&
           CreateTexture(_globalFlow, 1, 1, kFlowFormat, L"OpticalFlow_Global") &&
           CreateTexture(_cameraFlow, 4, 1, kFlowFormat, L"OpticalFlow_CurrentCameraModel") &&
           CreateTexture(_flow, (width + 1) / 2, (height + 1) / 2, kFlowFormat, L"OpticalFlow_Flow");
}

void OpticalFlowDx12::Transition(ID3D12GraphicsCommandList* list, Tex& tex, D3D12_RESOURCE_STATES state)
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

void OpticalFlowDx12::Pass(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, ID3D12Resource* src0,
                           DXGI_FORMAT format0, ID3D12Resource* src1, DXGI_FORMAT format1, ID3D12Resource* src2,
                           DXGI_FORMAT format2, Tex& dst, DXGI_FORMAT dstFormat, const Constants& constants,
                           ID3D12Resource* src3, DXGI_FORMAT format3, ID3D12Resource* src4, DXGI_FORMAT format4,
                           ID3D12Resource* src5, DXGI_FORMAT format5, ID3D12Resource* src6, DXGI_FORMAT format6,
                           DXGI_FORMAT auxUavFormat)
{
    Transition(list, dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (_debugEvents)
    {
        const char* label = pso == _visualise ? "F5Low Visualise" : pso == _match ? "F5Low Match" :
                        pso == _smooth ? "F5Low Smooth" : pso == _median ? "F5Low Median" :
                        pso == _cameraModel ? "F5Low CameraModel" : pso == _global ? "F5Low Global" : pso == _down ? "F5Low Down" : "F5Low Luma";
        // DRED records the label beside its dispatch breadcrumbs when explicitly enabled.
        list->BeginEvent(1, label, (UINT) std::strlen(label) + 1);
    }
    StampBegin(list);

    const UINT first = _heapCursor;
    _heapCursor = (_heapCursor + kDescriptorsPerPass) % (kDescriptorsPerPass * kPassesPerFrame * kFramesInFlight);

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = _heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T) first * _descriptorSize;
    gpu.ptr += (UINT64) first * _descriptorSize;

    ID3D12Resource* sources[8] = { src0,
                                   src1 != nullptr ? src1 : src0,
                                   src2 != nullptr ? src2 : src0,
                                   src3 != nullptr ? src3 : src0,
                                   src4 != nullptr ? src4 : src0,
                                   src5 != nullptr ? src5 : src0,
                                   src6 != nullptr ? src6 : src0,
                                   _passAgeIn != nullptr ? _passAgeIn : src0 };
    const DXGI_FORMAT formats[8] = { format0,
                                     src1 != nullptr ? format1 : format0,
                                     src2 != nullptr ? format2 : format0,
                                     src3 != nullptr ? format3 : format0,
                                     src4 != nullptr ? format4 : format0,
                                     src5 != nullptr ? format5 : format0,
                                     src6 != nullptr ? format6 : format0,
                                     _passAgeIn != nullptr ? kAgeFormat : format0 };

    for (int i = 0; i < 8; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = formats[i];
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        _device->CreateShaderResourceView(sources[i], &srv, cpu);
        cpu.ptr += _descriptorSize;
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.Format = dstFormat;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    _device->CreateUnorderedAccessView(dst.resource, nullptr, &uav, cpu);
    cpu.ptr += _descriptorSize;

    // The second UAV is the still age the finest match writes; every other pass gets a view of nothing.
    D3D12_UNORDERED_ACCESS_VIEW_DESC ageUav {};
    ageUav.Format = auxUavFormat;
    ageUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    _device->CreateUnorderedAccessView(_passAgeOut, nullptr, &ageUav, cpu);
    if (_passTrace) {
        for (auto& tex:_trace) {
            cpu.ptr += _descriptorSize;
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            _device->CreateUnorderedAccessView(tex.resource,nullptr,&view,cpu);
        }
    }

    ID3D12DescriptorHeap* heaps[] = { _heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(_rootSignature);
    const uint32_t features=(_settings.referenceMode?0u:
        (_settings.reuseCandidateCost?1u:0u)|(_settings.refinementAcceptance?2u:0u)|
        (_settings.evidenceFilter?4u:0u))|(_settings.evidenceDiagnostics?32u:0u);
    ID3D12PipelineState* selected=pso;
    if(pso==_match){
        const unsigned key=(features&7u)|((features&32u)?4u:0u);
        selected=_passTrace?_traceVariants[key]:(key?_matchVariants[key]:_match);
    }
    if(pso==_median && (features&(4u|32u)))selected=_medianEvidence;
    if(pso==_smooth)selected=(features&4u)?_smoothEvidence:(features&32u)?_smoothDiagnostics:_smooth;
    list->SetPipelineState(selected);
    list->SetComputeRootDescriptorTable(0, gpu);
    Constants actual=constants;
    actual.motionFlags=features;
    // Display-only depth calibration uses these otherwise matching-only slots.
    // Never overwrite the visualizer's multiplier / linearization far plane.
    if(pso!=_visualise){actual.photoTolerance=1e-6f;actual.evidenceNoise=1.0f/255.0f;}
    list->SetComputeRoot32BitConstants(1, sizeof(Constants) / 4, &actual, 0);
    const uint32_t groupSize=pso==_smooth?16u:8u;
    list->Dispatch((dst.width + groupSize-1) / groupSize, (dst.height + groupSize-1) / groupSize, 1);

    // Anything that reads it next reads it as a texture.
    Transition(list, dst, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    StampEnd(list, pso);
    if (_debugEvents) list->EndEvent();
}

// The test's timing: a timestamp before the first pass of a frame, then one after each (the barrier at the end of a
// pass is in it).
void OpticalFlowDx12::StampBegin(ID3D12GraphicsCommandList* list)
{
    if (_timeHeap != nullptr && _timeCount == 0 && _timeCapacity >= 2 && _timeCapacity <= 64)
    {
        list->EndQuery(_timeHeap, D3D12_QUERY_TYPE_TIMESTAMP, _timeCount);
        _timeNames[_timeCount++] = "start";
    }
}

void OpticalFlowDx12::StampEnd(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, const char* name)
{
    if (_timeHeap == nullptr || _timeCount == 0 || _timeCount >= _timeCapacity || _timeCount >= 64)
        return;

    list->EndQuery(_timeHeap, D3D12_QUERY_TYPE_TIMESTAMP, _timeCount);
    _timeNames[_timeCount++] = name != nullptr ? name
                               : pso == _luma || pso == _lumaSdr || pso == _lumaScRgb || pso == _lumaPq ? "luma"
                               : pso == _down                                                         ? "down"
                               : pso == _match                                                        ? "match"
                               : pso == _median                                                       ? "median"
                               : pso == _smooth                                                       ? "smooth"
                               : pso == _cameraModel                                                  ? "camera_model"
                               : pso == _global                                                       ? "global"


                                                                                                      : "other";
}

bool OpticalFlowDx12::Dispatch(ID3D12GraphicsCommandList* list, ID3D12Resource* color, DXGI_FORMAT colorFormat,
                               ID3D12Resource* depth, DXGI_FORMAT depthFormat, bool depthReversed, Encoding encoding)
{
    if (_device == nullptr || _match == nullptr || list == nullptr || color == nullptr)
        return false;

    const D3D12_RESOURCE_DESC colorDesc = color->GetDesc();

    if (!EnsureSize((uint32_t) colorDesc.Width, colorDesc.Height))
    {
        _error = "creating the textures";
        return false;
    }

    _traceValid=false;
    if (_settings.trace) {
        for (auto& tex:_trace) if (!tex.resource && !CreateTexture(tex,(uint32_t(colorDesc.Width)+1)/2,
                (colorDesc.Height+1)/2,DXGI_FORMAT_R32G32B32A32_FLOAT,L"OpticalFlow_OptionalTrace")) return false;
        const unsigned key=(_settings.referenceMode?0u:(_settings.reuseCandidateCost?1u:0u)|
            (_settings.refinementAcceptance?2u:0u)|(_settings.evidenceFilter?4u:0u))|(_settings.evidenceDiagnostics?4u:0u);
        if (!_traceVariants[key]) {
            const auto mode=std::to_string(64u+key);const auto* code=Compile("Match",&_error,"OpticalFlow",mode.c_str());
            if(!code)return false;
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=_rootSignature;
            desc.CS={code->GetBufferPointer(),code->GetBufferSize()};
            if(FAILED(_device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&_traceVariants[key]))))return false;
        }
    }

    // Build the current frame's pyramid.
    auto& current = _pyramid[_current];
    auto& previous = _pyramid[1 - _current];

    Constants constants {};
    constants.sizeX = current[0].width;
    constants.sizeY = current[0].height;
    constants.auxX = (uint32_t) colorDesc.Width;
    constants.auxY = colorDesc.Height;
    constants.whiteNits = (std::max)(_settings.hdrWhiteNits, 1.0f);
    Pass(list,
         !_settings.perceptualLuma     ? _luma
         : encoding == Encoding::ScRgb ? _lumaScRgb
         : encoding == Encoding::Pq    ? _lumaPq
                                       : _lumaSdr,
         color, colorFormat, nullptr, DXGI_FORMAT_UNKNOWN, nullptr, DXGI_FORMAT_UNKNOWN, current[0], kLumaFormat,
         constants);

    // Is this frame a hard cut from the last one? Decided here, on the GPU, so the match and the trust mask can act on
    // it in this very frame.
    _sceneCutRan = false;

    if (_settings.sceneCutDetector)
    {
        StampBegin(list);
        _sceneCutRan = _sceneCut.Detect(list, current[0].resource, _settings.sceneCutThreshold);
        StampEnd(list, nullptr, "scenecut");
    }
    else
        _sceneCut.Reset();

    for (int level = 1; level < kLevels; ++level)
    {
        constants.sizeX = current[level].width;
        constants.sizeY = current[level].height;
        constants.auxX = current[level - 1].width;
        constants.auxY = current[level - 1].height;
        Pass(list, _down, current[level - 1].resource, kLumaFormat, nullptr, DXGI_FORMAT_UNKNOWN, nullptr,
             DXGI_FORMAT_UNKNOWN, current[level], kLumaFormat, constants);
    }

    const bool history = _flowValid; // the last frame made a flow: its levels are candidates
    _flowValid = false;
    auto& levelNow = _levelFlow[_current];
    auto& levelBefore = _levelFlow[1 - _current];

    // Depth-aware matching, when there is depth.
    const bool depthMatching = _settings.depthMatching && depth != nullptr && depthFormat != DXGI_FORMAT_UNKNOWN;
    _usedDepth = depthMatching;
    const D3D12_RESOURCE_DESC depthDesc = depthMatching ? depth->GetDesc() : D3D12_RESOURCE_DESC {};

    bool wroteAge = false; // the finest match wrote this frame's ages

    if (_havePrevious)
    {
        for (int level = kLevels - 1; level >= 0; --level)
        {
            const bool coarsest = level == kLevels - 1;

            constants = Constants {};
            constants.sizeX = current[level].width;
            constants.sizeY = current[level].height;
            constants.radius = coarsest ? _settings.coarseRadius : _settings.radius;
            constants.hasPrediction = coarsest ? 0 : 1;
            constants.lambda = _settings.lambda;
            constants.hasHistory = (history && _settings.useHistory) ? 1 : 0;
            constants.coarseCells = (uint32_t) std::clamp(_settings.coarseCells, 1, 9);
            constants.knee = _settings.confidenceKnee;
            constants.depthMatching = depthMatching ? 1 : 0;
            constants.depthX = depthMatching ? (uint32_t) depthDesc.Width : 1;
            constants.depthY = depthMatching ? depthDesc.Height : 1;
            constants.reversed = depthReversed ? 1 : 0;
            constants.hasGlobal = _globalReady ? 1 : 0;
            constants.inverseRefinement = _settings.inverseRefinement ? 1 : 0;
            constants.sceneCutEnabled = _sceneCutRan ? 1 : 0;
            constants.zeroMargin = level == 0 ? _settings.zeroMargin : 0.0f;
            constants.zeroReach = (uint32_t) std::clamp(_settings.zeroReach, 0, 64);
            constants.scale = 1.0f / (float) (2 << level); // full-resolution pixels in this level's
            constants.stillFrames = level == 0 ? (uint32_t) std::clamp(_settings.stillFrames, 0, 255) : 0;
            constants.hasAge = _ageValid ? 1 : 0;
            constants.stillEpsilon = _settings.stillEpsilon;
            constants.stillMargin = _settings.stillMargin;

            // The brightness weights, on the finest level (with depth on top of the depth weights).
            constants.lookWeights = level == 0 && _settings.lookWeights ? 1 : 0;
            constants.lookRange = (std::max)(_settings.lookRange, 1e-4f);
            constants.lookDistance = (std::max)(_settings.lookDistance, 0.5f);

            if (!coarsest)
            {
                constants.auxX = current[level + 1].width;
                constants.auxY = current[level + 1].height;
            }

            // The finest level reads the last frame's ages and writes this frame's (no pass of its own).
            const bool keepAge = constants.stillFrames != 0;

            if (keepAge)
            {
                _passAgeIn = _age[1 - _current].resource;
                _passAgeOut = _age[_current].resource;
                Transition(list, _age[_current], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }

            _passTrace=_settings.trace && level==std::clamp(_settings.traceLevel,0,kLevels-1);
            if(_passTrace)for(auto& tex:_trace)Transition(list,tex,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Pass(list, _match, current[level].resource, kLumaFormat, previous[level].resource, kLumaFormat,
                 coarsest ? nullptr : levelNow[level + 1].resource, kFlowFormat, levelNow[level], kFlowFormat,
                 constants, (history && _settings.useHistory) ? levelBefore[level].resource : nullptr, kFlowFormat,
                 depthMatching ? depth : nullptr, depthFormat, _globalReady ? _globalFlow.resource : nullptr,
                 kFlowFormat, _sceneCutRan ? _sceneCut.Flag() : nullptr, kStateFormat);

            if(_passTrace){for(auto& tex:_trace)Transition(list,tex,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);_traceValid=true;}
            _passTrace=false;
            if (keepAge)
            {
                Transition(list, _age[_current], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                _passAgeIn = _passAgeOut = nullptr;
                wroteAge = true;
            }
        }

        constants = Constants {};
        constants.sizeX = _flow.width;
        constants.sizeY = _flow.height;
        constants.scale = 2.0f; // the half-resolution level's pixels to full-resolution ones
        Pass(list, _median, levelNow[0].resource, kFlowFormat, nullptr, DXGI_FORMAT_UNKNOWN, nullptr,
             DXGI_FORMAT_UNKNOWN, _flowMedian, kFlowFormat, constants);

        const bool currentCameraReady=_settings.cameraPrior && _settings.globalCandidate;
        if(currentCameraReady) {
            Constants model {};
            model.sizeX=4;model.sizeY=1;
            model.auxX=_flowMedian.width;model.auxY=_flowMedian.height;
            model.scale=1.0f; // Median already converted XY to full-resolution pixels.
            model.hasGlobal=_globalReady?1u:0u;
            model.depthMatching=depthMatching?1u:0u;
            model.depthX=depthMatching?uint32_t(depthDesc.Width):1u;
            model.depthY=depthMatching?depthDesc.Height:1u;
            model.reversed=depthReversed?1u:0u;
            Pass(list,_cameraModel,_flowMedian.resource,kFlowFormat,nullptr,DXGI_FORMAT_UNKNOWN,
                 nullptr,DXGI_FORMAT_UNKNOWN,_cameraFlow,kFlowFormat,model,nullptr,DXGI_FORMAT_UNKNOWN,
                 depthMatching?depth:nullptr,depthFormat,_globalReady?_globalFlow.resource:nullptr,kFlowFormat);
        }
        constants = Constants {};
        constants.sizeX = _flow.width;
        constants.sizeY = _flow.height;
        constants.radius = std::clamp(_settings.smoothRadius, 0, 4); // the shader's tile holds at most four pixels around a group
        constants.depthMatching = depthMatching ? 1u : 0u;
        constants.depthX = depthMatching ? (uint32_t)depthDesc.Width : 1u;
        constants.depthY = depthMatching ? depthDesc.Height : 1u;
        constants.reversed = depthReversed ? 1u : 0u;
        constants.hasGlobal=currentCameraReady?2u:0u;
        constants.lookWeights=_settings.lookWeights?1u:0u;
        constants.lookRange=(std::max)(_settings.lookRange,1e-4f);
        constants.lookDistance=(std::max)(_settings.lookDistance,0.5f);
        constants.stillFrames=uint32_t(std::clamp(_settings.stillFrames,0,255));
        constants.stillMargin=_settings.stillMargin;
        constants.hasAge=wroteAge?1u:0u;
        _passAgeIn=wroteAge?_age[_current].resource:nullptr;
        Pass(list, _smooth, _flowMedian.resource, kFlowFormat, current[0].resource, kLumaFormat, previous[0].resource,
             kLumaFormat, _flow, kFlowFormat, constants, nullptr, DXGI_FORMAT_UNKNOWN,
             depthMatching ? depth : nullptr, depthFormat,currentCameraReady?_cameraFlow.resource:nullptr,kFlowFormat);
        _passAgeIn=nullptr;

        if (_settings.globalCandidate)
        {
            // What the whole frame did, for the next one to try everywhere (see Global).
            constants = Constants {};
            constants.sizeX = 1;
            constants.sizeY = 1;
            constants.auxX = _flow.width;
            constants.auxY = _flow.height;
            constants.depthMatching = depthMatching ? 1 : 0;
            constants.depthX = depthMatching ? (uint32_t) depthDesc.Width : 1;
            constants.depthY = depthMatching ? depthDesc.Height : 1;
            constants.reversed = depthReversed ? 1 : 0;
            Pass(list, _global, _flow.resource, kFlowFormat, nullptr, DXGI_FORMAT_UNKNOWN, nullptr, DXGI_FORMAT_UNKNOWN,
                 _globalFlow, kFlowFormat, constants, nullptr, DXGI_FORMAT_UNKNOWN, depthMatching ? depth : nullptr,
                 depthFormat);
        }

        _globalReady = _settings.globalCandidate;
        _flowValid = true;
    }

    // The flow can be read by a pixel shader too (the menu preview, a consumer's sampling).
    Transition(list, _flow,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // The ages this frame wrote are next frame's last ones, if the match wrote them (taken from what ran, not from the
    // settings, which the menu can change meanwhile).
    _ageValid = wroteAge;

    _current = 1 - _current;
    _havePrevious = true;
    return true;
}

bool OpticalFlowDx12::Visualise(ID3D12GraphicsCommandList* list, float maxSpeed, int view,
                               ID3D12Resource* color, DXGI_FORMAT colorFormat,
                                ID3D12Resource* depth, DXGI_FORMAT depthFormat, bool depthReversed,
                                ID3D12Resource* guide, ID3D12Resource* historyDistrust, const float* debugDepthTransform)
{
    if (!_flowValid || list == nullptr)
        return false;

    // Diagnostics must not create a pipeline or allocate their image while disabled. This also
    // avoids compiling a newly selected visualizer during the game's initial swapchain setup.
    if (_visualise == nullptr)
    {
        const auto* code = Compile("Visualise", &_error);
        if (!code) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso {};
        pso.pRootSignature = _rootSignature;
        pso.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        if (FAILED(_device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&_visualise))))
        {
            _error = "creating the diagnostic visualizer pipeline";
            return false;
        }
    }
    Tex& preview=view==6?_depthPreview:_preview;
    if (preview.resource == nullptr &&
        !CreateTexture(preview,view==6?_width:_flow.width,view==6?_height:_flow.height,
                       DXGI_FORMAT_R8G8B8A8_UNORM,L"OpticalFlow_Preview"))
        return false;
    _depthPreviewActive=view==6;

    // The flow rests in the combined read state; the pass wants the non-pixel one it was written to.
    Tex& shownFlow = view == 2 ? _levelFlow[1-_current][0] : view == 3 ? _flowMedian : _flow;
    Transition(list, shownFlow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    Constants constants {};
    constants.sizeX = preview.width;
    constants.sizeY = preview.height;
    constants.scale = maxSpeed;
    constants.radius = std::clamp(view, 0, 18);
    constants.inverseRefinement=_debugSettings.depthLinearize?1u:0u;
    constants.photoTolerance=_debugSettings.depthMultiplier;
    constants.evidenceNoise=(std::max)(_debugSettings.depthFarPlane,1.001f);
    constants.auxX = _width;
    constants.auxY = _height;
    constants.knee = std::clamp(_debugSettings.spacing, 16.0f, 128.0f);
    constants.lambda = std::clamp(_debugSettings.gain, 0.25f, 8.0f);
    constants.zeroMargin = std::clamp(_debugSettings.minimumMotion, 0.05f, 4.0f);
    constants.lookRange = std::clamp(_debugSettings.thickness, 0.5f, 3.0f);
    constants.lookDistance = std::clamp(_debugSettings.gridOpacity, 0.0f, 0.5f);
    constants.whiteNits = std::clamp(_debugSettings.opacity, 0.0f, 1.0f);
    if(view==6){
        constants.zeroMargin=debugDepthTransform?(std::max)(debugDepthTransform[0],.01f):1.f;
        constants.lookRange=debugDepthTransform?(std::max)(debugDepthTransform[1],.01f):1.f;
        constants.lookDistance=debugDepthTransform?-debugDepthTransform[2]/float(_width):0.f;
        constants.whiteNits=debugDepthTransform?debugDepthTransform[3]/float(_height):0.f;
    }
    const bool background = (view == 7 || view == 18) && _debugSettings.background && color && colorFormat != DXGI_FORMAT_UNKNOWN;
    constants.hasPrediction = background ? 1 : 0;
    constants.depthMatching=depth && depthFormat!=DXGI_FORMAT_UNKNOWN ? 1u:0u;
    if(constants.depthMatching){auto d=depth->GetDesc();constants.depthX=(uint32_t)d.Width;constants.depthY=d.Height;}
    constants.reversed=depthReversed?1u:0u;
    const bool maskView=view==5 || view==17 || view==18;
    ID3D12Resource* debugGuide=maskView ? historyDistrust : guide;
    const DXGI_FORMAT debugGuideFormat=maskView ? DXGI_FORMAT_R8_UNORM : kFlowFormat;
    constants.hasHistory=debugGuide?1u:0u;
    constants.hasGlobal=((!_settings.referenceMode && _settings.evidenceFilter) || _settings.evidenceDiagnostics)?1u:0u;
    const bool showCameraSupport=(view==18 || view==5) && _settings.cameraPrior && _settings.globalCandidate;
    if(showCameraSupport)constants.hasGlobal=2u;
    constants.stillFrames=uint32_t(std::clamp(_settings.stillFrames,0,255));
    constants.hasAge=view==12 && _ageValid?1u:0u;
    if(view==12 && _ageValid)_passAgeIn=_age[1-_current].resource;
    const bool showTrace=view>=11 && view<=16 && view!=12 && _traceValid;
    if(showTrace) {
        constants.coarseCells=uint32_t(std::clamp(_settings.traceLevel,0,kLevels-1));
        constants.hasAge=1;
    }
    Pass(list, _visualise, shownFlow.resource, kFlowFormat, showTrace?_trace[0].resource:background ? color : nullptr,
         showTrace?DXGI_FORMAT_R32G32B32A32_FLOAT:background ? colorFormat : DXGI_FORMAT_UNKNOWN,
         showTrace?_trace[1].resource:_pyramid[1-_current][0].resource,showTrace?DXGI_FORMAT_R32G32B32A32_FLOAT:kLumaFormat,preview,DXGI_FORMAT_R8G8B8A8_UNORM,constants,
         showTrace?_trace[2].resource:depth,showTrace?DXGI_FORMAT_R32G32B32A32_FLOAT:depthFormat,_pyramid[_current][0].resource,kLumaFormat,
         debugGuide,debugGuide?debugGuideFormat:DXGI_FORMAT_UNKNOWN,
         showCameraSupport?_cameraFlow.resource:_levelFlow[1-_current][0].resource,kFlowFormat);

    _passAgeIn=nullptr;
    Transition(list, shownFlow,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(list, preview, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}
