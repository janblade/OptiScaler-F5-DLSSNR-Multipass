// Not built with the precompiled header: self-contained so the host GPU test can compile it alone.
#include "OpticalFlow_Dx12.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

namespace
{

constexpr uint32_t kDescriptorsPerPass = 5; // four SRVs and one UAV
constexpr uint32_t kPassesPerFrame = 1 + (OpticalFlowDx12::kLevels - 1) + OpticalFlowDx12::kLevels + 1 + 1;
constexpr uint32_t kFramesInFlight = 8;
constexpr DXGI_FORMAT kLumaFormat = DXGI_FORMAT_R32_FLOAT;
constexpr DXGI_FORMAT kFlowFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; // typed UAV stores of this are required of every device

const char* kSource = R"HLSL(
cbuffer P : register(b0)
{
    uint2 size;      // the size of what is written
    uint2 aux;       // the size of what is read (luma, down) or unused
    int radius;
    uint hasPrediction;
    float lambda;
    float scale;
    uint hasHistory;
    uint3 pad;
};

SamplerState Linear : register(s0);
Texture2D<float4> Color : register(t0);
Texture2D<float>  CurLuma : register(t0);
Texture2D<float>  PrevLuma : register(t1);
Texture2D<float4> Prediction : register(t2);
Texture2D<float4> History : register(t3);
Texture2D<float4> FlowIn : register(t0);
RWTexture2D<float>  OutLuma : register(u0);
RWTexture2D<float4> OutFlow : register(u0);

// Luma of the colour, tone-mapped so an HDR picture matches as well as an SDR one, averaged over 2x2 into the first level.
[numthreads(8, 8, 1)]
void Luma(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    float sum = 0.0;
    [unroll] for (int j = 0; j < 2; ++j)
        [unroll] for (int i = 0; i < 2; ++i)
        {
            int2 p = min(int2(id.xy) * 2 + int2(i, j), int2(aux) - 1);
            float3 c = max(Color.Load(int3(p, 0)).rgb, 0.0);
            float l = dot(c, float3(0.299, 0.587, 0.114));
            sum += l / (1.0 + l);
        }

    OutLuma[id.xy] = sum * 0.25;
}

// The next level: a 2x2 average of the one above.
[numthreads(8, 8, 1)]
void Down(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    float sum = 0.0;
    [unroll] for (int j = 0; j < 2; ++j)
        [unroll] for (int i = 0; i < 2; ++i)
            sum += CurLuma.Load(int3(min(int2(id.xy) * 2 + int2(i, j), int2(aux) - 1), 0));

    OutLuma[id.xy] = sum * 0.25;
}

// Sum of absolute differences between the current frame around p and the previous frame around p + d: sixteen samples, two
// pixels apart, over an 8x8 window.
// The previous frame at a fractional pixel position, filtered.
float Sample(float2 position)
{
    return PrevLuma.SampleLevel(Linear, (position + 0.5) / float2(size), 0);
}

float Cost(int2 p, int2 d)
{
    float s = 0.0;
    int2 hi = int2(size) - 1;

    [unroll] for (int j = -3; j <= 3; j += 2)
        [unroll] for (int i = -3; i <= 3; i += 2)
        {
            int2 q = int2(i, j);
            float c = CurLuma.Load(int3(clamp(p + q, 0, hi), 0));
            float r = PrevLuma.Load(int3(clamp(p + q + d, 0, hi), 0));
            s += abs(c - r);
        }

    return s;
}

// Block matching at one level: look around the coarser level's answer (doubled, it is in that level's pixels) for the offset
// into the previous frame with the smallest difference, then a few gradient steps for the part of a pixel.
[numthreads(8, 8, 1)]
void Match(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    int2 p = int2(id.xy);

    // The candidates for where to search: no motion, the coarser level's answer at the four cells nearest this pixel (what a
    // bilinear read would use; doubled, it is in this level's pixels) and the last frame's flow here. The one that matches
    // best is where the search starts, so a steady pan carries over from frame to frame and an edge is not stuck with the
    // answer of a cell that lies across it.
    int2 centre = 0;
    float start = Cost(p, centre);

    if (hasPrediction != 0)
    {
        int2 coarseHi = int2(aux) - 1;
        int2 cp = min(p >> 1, coarseHi);
        int2 step = int2((p.x & 1) != 0 ? 1 : -1, (p.y & 1) != 0 ? 1 : -1);

        [unroll] for (int k = 0; k < 4; ++k)
        {
            int2 cell = cp + int2((k & 1) != 0 ? step.x : 0, (k & 2) != 0 ? step.y : 0);
            int2 d = int2(round(Prediction.Load(int3(clamp(cell, 0, coarseHi), 0)).xy * 2.0));
            float c = Cost(p, d);

            if (c < start)
            {
                start = c;
                centre = d;
            }
        }
    }

    if (hasHistory != 0)
    {
        int2 d = int2(round(History.Load(int3(p, 0)).xy));
        float c = Cost(p, d);

        if (c < start)
        {
            start = c;
            centre = d;
        }
    }

    float best = 1e30;
    int2 bestD = centre;

    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx)
        {
            int2 d = centre + int2(dx, dy);
            float c = Cost(p, d) + lambda * length(float2(dx, dy));

            if (c < best)
            {
                best = c;
                bestD = d;
            }
        }

    // The part of a pixel: a few Lucas-Kanade steps. With the previous frame sampled at the matched offset, the remaining
    // difference is explained by the picture's gradient there; solve that for the shift. A match that is already exact has no
    // difference left and is not moved, which a fit through the costs either side cannot promise.
    float2 sub = 0.0;

    [loop] for (int iteration = 0; iteration < 3; ++iteration)
    {
        float a = 0.0, b = 0.0, c = 0.0, e = 0.0, f = 0.0;

        [unroll] for (int j = -3; j <= 3; j += 2)
            [unroll] for (int i = -3; i <= 3; i += 2)
            {
                int2 q = clamp(p + int2(i, j), 0, int2(size) - 1);
                float2 at = float2(q + bestD) + sub;
                float centre = Sample(at);
                float gx = 0.5 * (Sample(at + float2(1, 0)) - Sample(at - float2(1, 0)));
                float gy = 0.5 * (Sample(at + float2(0, 1)) - Sample(at - float2(0, 1)));
                float r = CurLuma.Load(int3(q, 0)) - centre;

                a += gx * gx;
                b += gx * gy;
                c += gy * gy;
                e += gx * r;
                f += gy * r;
            }

        float det = a * c - b * b;

        if (det > 1e-9)
            sub = clamp(sub + float2(c * e - b * f, a * f - b * e) / det, -1.5, 1.5);
    }

    OutFlow[id.xy] = float4(float2(bestD) + sub, 0.0, 1.0);
}

// A 3x3 median of each component, which removes the odd wrong block, scaled to full-resolution pixels.
void Sort(inout float a, inout float b)
{
    float lo = min(a, b);
    b = max(a, b);
    a = lo;
}

float Median9(float v[9])
{
    // a sorting network for nine values, the middle one is the median
    Sort(v[0], v[1]); Sort(v[3], v[4]); Sort(v[6], v[7]);
    Sort(v[1], v[2]); Sort(v[4], v[5]); Sort(v[7], v[8]);
    Sort(v[0], v[1]); Sort(v[3], v[4]); Sort(v[6], v[7]);
    Sort(v[0], v[3]); Sort(v[3], v[6]); Sort(v[0], v[3]);
    Sort(v[1], v[4]); Sort(v[4], v[7]); Sort(v[1], v[4]);
    Sort(v[2], v[5]); Sort(v[5], v[8]); Sort(v[2], v[5]);
    Sort(v[2], v[4]); Sort(v[4], v[6]); Sort(v[2], v[4]);
    return v[4];
}

// Hue for the direction, brightness for the speed (scale is the speed that is full brightness).
[numthreads(8, 8, 1)]
void Visualise(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    float2 f = FlowIn.Load(int3(id.xy, 0)).xy;
    float hue = atan2(f.y, f.x) / 6.2831853 + 0.5;
    float3 rgb = saturate(abs(frac(hue + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
    float level = saturate(length(f) / scale);

    OutFlow[id.xy] = float4(rgb * level, 1.0);
}

[numthreads(8, 8, 1)]
void Median(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y)
        return;

    float xs[9], ys[9];
    int k = 0;
    int2 hi = int2(size) - 1;

    [unroll] for (int j = -1; j <= 1; ++j)
        [unroll] for (int i = -1; i <= 1; ++i)
        {
            float2 f = FlowIn.Load(int3(clamp(int2(id.xy) + int2(i, j), 0, hi), 0)).xy;
            xs[k] = f.x;
            ys[k] = f.y;
            ++k;
        }

    OutFlow[id.xy] = float4(Median9(xs) * scale, Median9(ys) * scale, 0.0, 1.0);
}
)HLSL";

ID3DBlob* Compile(const char* entry, std::string* error)
{
    ID3DBlob* code = nullptr;
    ID3DBlob* messages = nullptr;

    const HRESULT hr = D3DCompile(kSource, strlen(kSource), "OpticalFlow", nullptr, nullptr, entry, "cs_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages);

    if (FAILED(hr))
    {
        *error = std::string("compiling ") + entry + ": " +
                 (messages != nullptr ? (const char*) messages->GetBufferPointer() : "no message");
        if (messages != nullptr)
            messages->Release();
        return nullptr;
    }

    if (messages != nullptr)
        messages->Release();

    return code;
}

} // namespace

OpticalFlowDx12::~OpticalFlowDx12()
{
    ReleaseTextures();

    for (ID3D12PipelineState** pso : { &_luma, &_down, &_match, &_median, &_visualise })
        if (*pso != nullptr)
            (*pso)->Release();

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

    // One table (four SRVs, one UAV) and the root constants.
    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 4;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 4;

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
                                Entry { "Median", &_median }, Entry { "Visualise", &_visualise } })
    {
        ID3DBlob* code = Compile(entry.name, &_error);

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

    release(_flow);
    release(_preview);
}

bool OpticalFlowDx12::EnsureSize(uint32_t width, uint32_t height)
{
    if (width == _width && height == _height && _flow.resource != nullptr)
        return true;

    // The previous work may still be reading these; a size change is rare (a resolution change) and the caller is expected
    // to have waited, as it does before replacing its own targets.
    ReleaseTextures();
    _havePrevious = false;
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

    return CreateTexture(_flow, (width + 1) / 2, (height + 1) / 2, kFlowFormat, L"OpticalFlow_Flow") &&
           CreateTexture(_preview, (width + 1) / 2, (height + 1) / 2, DXGI_FORMAT_R8G8B8A8_UNORM, L"OpticalFlow_Preview");
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
                           ID3D12Resource* src3, DXGI_FORMAT format3)
{
    Transition(list, dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const UINT first = _heapCursor;
    _heapCursor = (_heapCursor + kDescriptorsPerPass) % (kDescriptorsPerPass * kPassesPerFrame * kFramesInFlight);

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = _heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = _heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T) first * _descriptorSize;
    gpu.ptr += (UINT64) first * _descriptorSize;

    ID3D12Resource* sources[4] = { src0, src1 != nullptr ? src1 : src0, src2 != nullptr ? src2 : src0,
                                   src3 != nullptr ? src3 : src0 };
    const DXGI_FORMAT formats[4] = { format0, src1 != nullptr ? format1 : format0, src2 != nullptr ? format2 : format0,
                                     src3 != nullptr ? format3 : format0 };

    for (int i = 0; i < 4; ++i)
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

    ID3D12DescriptorHeap* heaps[] = { _heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(_rootSignature);
    list->SetPipelineState(pso);
    list->SetComputeRootDescriptorTable(0, gpu);
    list->SetComputeRoot32BitConstants(1, sizeof(Constants) / 4, &constants, 0);
    list->Dispatch((dst.width + 7) / 8, (dst.height + 7) / 8, 1);

    // Anything that reads it next reads it as a texture.
    Transition(list, dst, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

bool OpticalFlowDx12::Dispatch(ID3D12GraphicsCommandList* list, ID3D12Resource* color, DXGI_FORMAT colorFormat)
{
    if (_device == nullptr || _match == nullptr || list == nullptr || color == nullptr)
        return false;

    const D3D12_RESOURCE_DESC colorDesc = color->GetDesc();

    if (!EnsureSize((uint32_t) colorDesc.Width, colorDesc.Height))
    {
        _error = "creating the textures";
        return false;
    }

    // Build the current frame's pyramid.
    auto& current = _pyramid[_current];
    auto& previous = _pyramid[1 - _current];

    Constants constants {};
    constants.sizeX = current[0].width;
    constants.sizeY = current[0].height;
    constants.auxX = (uint32_t) colorDesc.Width;
    constants.auxY = colorDesc.Height;
    Pass(list, _luma, color, colorFormat, nullptr, DXGI_FORMAT_UNKNOWN, nullptr, DXGI_FORMAT_UNKNOWN, current[0],
         kLumaFormat, constants);

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
            constants.hasHistory = history ? 1 : 0;

            if (!coarsest)
            {
                constants.auxX = current[level + 1].width;
                constants.auxY = current[level + 1].height;
            }

            Pass(list, _match, current[level].resource, kLumaFormat, previous[level].resource, kLumaFormat,
                 coarsest ? nullptr : levelNow[level + 1].resource, kFlowFormat, levelNow[level], kFlowFormat, constants,
                 history ? levelBefore[level].resource : nullptr, kFlowFormat);
        }

        constants = Constants {};
        constants.sizeX = _flow.width;
        constants.sizeY = _flow.height;
        constants.scale = 2.0f; // the half-resolution level's pixels to full-resolution ones
        Pass(list, _median, levelNow[0].resource, kFlowFormat, nullptr, DXGI_FORMAT_UNKNOWN, nullptr,
             DXGI_FORMAT_UNKNOWN, _flow, kFlowFormat, constants);

        _flowValid = true;
    }

    // The flow can be read by a pixel shader too (the menu preview, a consumer's sampling).
    Transition(list, _flow,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    _current = 1 - _current;
    _havePrevious = true;
    return true;
}

bool OpticalFlowDx12::Visualise(ID3D12GraphicsCommandList* list, float maxSpeed)
{
    if (!_flowValid || _preview.resource == nullptr || list == nullptr)
        return false;

    // The flow rests in the combined read state; the pass wants the non-pixel one it was written to.
    Transition(list, _flow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    Constants constants {};
    constants.sizeX = _preview.width;
    constants.sizeY = _preview.height;
    constants.scale = maxSpeed;
    Pass(list, _visualise, _flow.resource, kFlowFormat, nullptr, DXGI_FORMAT_UNKNOWN, nullptr, DXGI_FORMAT_UNKNOWN,
         _preview, DXGI_FORMAT_R8G8B8A8_UNORM, constants);

    Transition(list, _flow,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(list, _preview, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}
