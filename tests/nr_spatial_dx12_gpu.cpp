// GPU test for Compress screen edges' D3D12 passes, no game: the production DXIL (precompile/dlssnr_spatial_Shader.h and
// dlssnr_spatial_guides_Shader.h) run through a root signature shaped like DlssNr_Dx12's (six SRVs, two UAVs, one CBV,
// a linear clamp sampler), on the first hardware adapter or WARP. Checks, for the default layout and a lopsided one:
//   - mode 100 (colour pack) then 102 (unpack): the middle band comes back exactly where it started at 100% model
//     resolution, a smooth picture comes back close everywhere (the edges are resampled), and the pack really is smaller;
//   - mode 101 (guides): depth is point-sampled (every packed depth is one of the source's values), motion is carried by
//     its end points (Pack(p + mv) - Pack(p)), equal to the native vector in the 1:1 middle and shorter in the squeezed
//     edges, scaled by the game's motion scale, and read from a subrect of a larger allocation;
//   - the same at 75% model resolution (the packed picture is then resampled, the unpacked one is the 75% grid).
//
//   vcvars64, then from the repo root:
//   cl /std:c++20 /EHsc /O2 /W4 tests\nr_spatial_dx12_gpu.cpp d3d12.lib dxgi.lib

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include "../OptiScaler/shaders/dlssnr/DlssNr_Spatial.h"
#include "../OptiScaler/shaders/dlssnr/precompile/dlssnr_spatial_Shader.h"
#include "../OptiScaler/shaders/dlssnr/precompile/dlssnr_spatial_guides_Shader.h"

using Microsoft::WRL::ComPtr;
namespace Sp = DlssNr::Spatial;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);                                                           \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

namespace
{
uint16_t ToHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int exponent = (int) ((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = x & 0x7FFFFFu;
    if (exponent <= 0)
        return (uint16_t) sign;
    if (exponent >= 31)
        return (uint16_t) (sign | 0x7C00u);
    return (uint16_t) (sign | ((uint32_t) exponent << 10) | (mantissa >> 13));
}

float FromHalf(uint16_t h)
{
    const uint32_t sign = (uint32_t) (h & 0x8000u) << 16;
    const uint32_t exponent = (h >> 10) & 0x1Fu;
    const uint32_t mantissa = h & 0x3FFu;
    uint32_t bits;
    if (exponent == 0)
    {
        if (mantissa == 0)
            bits = sign;
        else
        {
            float value = (float) mantissa / 1024.0f * (1.0f / 16384.0f);
            std::memcpy(&bits, &value, 4);
            bits |= sign;
        }
    }
    else
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> colour, guides;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> constants;
    bool warp = false;

    bool Init()
    {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 d {};
            adapter->GetDesc1(&d);
            if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
                break;
            adapter.Reset();
        }
        if (!device)
        {
            warp = true;
            ComPtr<IDXGIAdapter> software;
            if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&software))) ||
                FAILED(D3D12CreateDevice(software.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
                return false;
        }
        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return false;
        list->Close();

        D3D12_DESCRIPTOR_RANGE ranges[3] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 6;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 2;
        ranges[1].OffsetInDescriptorsFromTableStart = 6;
        ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        ranges[2].NumDescriptors = 1;
        ranges[2].OffsetInDescriptorsFromTableStart = 8;
        D3D12_ROOT_PARAMETER param {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 3;
        param.DescriptorTable.pDescriptorRanges = ranges;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC sampler {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rs {};
        rs.NumParameters = 1;
        rs.pParameters = &param;
        rs.NumStaticSamplers = 1;
        rs.pStaticSamplers = &sampler;
        ComPtr<ID3DBlob> blob, error;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
            FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&rootSignature))))
            return false;

        const auto Pipeline = [&](const unsigned char* code, size_t size, ComPtr<ID3D12PipelineState>& pso) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
            pd.pRootSignature = rootSignature.Get();
            pd.CS = { code, size };
            return SUCCEEDED(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));
        };
        if (!Pipeline(dlssnr_spatial_cso, sizeof(dlssnr_spatial_cso), colour) ||
            !Pipeline(dlssnr_spatial_guides_cso, sizeof(dlssnr_spatial_guides_cso), guides))
            return false;

        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 9;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))))
            return false;

        D3D12_HEAP_PROPERTIES upload {};
        upload.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 256;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &bd,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(&constants)));
    }

    void Wait()
    {
        queue->Signal(fence.Get(), ++fenceValue);
        if (fence->GetCompletedValue() < fenceValue)
        {
            HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            fence->SetEventOnCompletion(fenceValue, e);
            WaitForSingleObject(e, INFINITE);
            CloseHandle(e);
        }
    }

    void Begin()
    {
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
    }

    void End()
    {
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        Wait();
    }

    ComPtr<ID3D12Resource> Texture(DXGI_FORMAT format, UINT w, UINT h, bool uav)
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = d.MipLevels = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> r;
        device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&r));
        return r;
    }

    void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        list->ResourceBarrier(1, &b);
    }

    // texels: width * height * texelBytes bytes, tightly packed. Leaves the texture readable by compute shaders.
    ComPtr<ID3D12Resource> Upload(DXGI_FORMAT format, UINT w, UINT h, UINT texelBytes, const void* texels)
    {
        auto tex = Texture(format, w, h, false);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT rows;
        UINT64 rowBytes, total;
        D3D12_RESOURCE_DESC td = tex->GetDesc();
        device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowBytes, &total);
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> staging;
        device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                        IID_PPV_ARGS(&staging));
        uint8_t* mapped = nullptr;
        staging->Map(0, nullptr, (void**) &mapped);
        for (UINT y = 0; y < h; ++y)
            std::memcpy(mapped + fp.Offset + (size_t) y * fp.Footprint.RowPitch,
                        (const uint8_t*) texels + (size_t) y * w * texelBytes, (size_t) w * texelBytes);
        staging->Unmap(0, nullptr);

        Begin();
        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.pResource = tex.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = staging.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        End();
        return tex;
    }

    // Reads a UAV texture that is in UNORDERED_ACCESS; leaves it readable by compute shaders.
    std::vector<uint8_t> Read(ID3D12Resource* tex, UINT texelBytes)
    {
        D3D12_RESOURCE_DESC td = tex->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT rows;
        UINT64 rowBytes, total;
        device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowBytes, &total);
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&readback));
        Begin();
        Barrier(tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
        src.pResource = tex;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        End();
        std::vector<uint8_t> out((size_t) td.Width * td.Height * texelBytes);
        uint8_t* mapped = nullptr;
        readback->Map(0, nullptr, (void**) &mapped);
        for (UINT y = 0; y < td.Height; ++y)
            std::memcpy(&out[(size_t) y * td.Width * texelBytes], mapped + fp.Offset + (size_t) y * fp.Footprint.RowPitch,
                        (size_t) td.Width * texelBytes);
        readback->Unmap(0, nullptr);
        return out;
    }

    // One dispatch. Outputs are created by the caller (UAV, COPY_DEST at rest); they end in UNORDERED_ACCESS.
    void Dispatch(const Sp::Constants& c, ID3D12Resource* in0, ID3D12Resource* in1, ID3D12Resource* in2,
                  ID3D12Resource* out0, ID3D12Resource* out1)
    {
        void* mapped = nullptr;
        constants->Map(0, nullptr, &mapped);
        std::memcpy(mapped, &c, sizeof(c));
        constants->Unmap(0, nullptr);

        const UINT step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
        ID3D12Resource* srvs[6] = { in0, in1 ? in1 : in0, in2 ? in2 : in0, in0, in0, in0 };
        for (int i = 0; i < 6; ++i, cpu.ptr += step)
            device->CreateShaderResourceView(srvs[i], nullptr, cpu);
        ID3D12Resource* uavs[2] = { out0, out1 ? out1 : out0 };
        for (int i = 0; i < 2; ++i, cpu.ptr += step)
            device->CreateUnorderedAccessView(uavs[i], nullptr, nullptr, cpu);
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv {};
        cbv.BufferLocation = constants->GetGPUVirtualAddress();
        cbv.SizeInBytes = 256;
        device->CreateConstantBufferView(&cbv, cpu);

        Begin();
        Barrier(out0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (out1)
            Barrier(out1, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* heaps[] = { heap.Get() };
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(rootSignature.Get());
        list->SetPipelineState(c.mode == 101 ? guides.Get() : colour.Get());
        list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        list->Dispatch((c.width + 7) / 8, (c.height + 7) / 8, 1);
        End();
    }
};

struct Image
{
    UINT w = 0, h = 0;
    std::vector<float> v; // 4 floats per texel
    float* at(UINT x, UINT y) { return &v[((size_t) y * w + x) * 4]; }
    const float* at(UINT x, UINT y) const { return &v[((size_t) y * w + x) * 4]; }
};

// A smooth picture with a ramp on both axes and a gentle wave, so that resampling the edges loses little.
Image SmoothPicture(UINT w, UINT h)
{
    Image img;
    img.w = w;
    img.h = h;
    img.v.resize((size_t) w * h * 4);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x)
        {
            float* p = img.at(x, y);
            p[0] = 0.15f + 0.7f * (float) x / (float) w;
            p[1] = 0.15f + 0.7f * (float) y / (float) h;
            p[2] = 0.5f + 0.25f * std::sin((float) x * 0.05f) * std::cos((float) y * 0.04f);
            p[3] = 1.0f;
        }
    return img;
}

std::vector<uint16_t> ToHalves(const Image& img)
{
    std::vector<uint16_t> out(img.v.size());
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = ToHalf(img.v[i]);
    return out;
}

Image FromHalves(const std::vector<uint8_t>& bytes, UINT w, UINT h)
{
    Image img;
    img.w = w;
    img.h = h;
    img.v.resize((size_t) w * h * 4);
    const uint16_t* half = (const uint16_t*) bytes.data();
    for (size_t i = 0; i < img.v.size(); ++i)
        img.v[i] = FromHalf(half[i]);
    return img;
}

void RunColour(Gpu& gpu, const Sp::Settings& settings, float scale, const char* label)
{
    constexpr UINT W = 320, H = 192;
    const Sp::Layout layout = Sp::Build(settings, W, H, scale);
    std::printf("%s: %ux%u -> model %ux%u, ordinary %ux%u\n", label, W, H, layout.modelW, layout.modelH,
                layout.ordinaryW, layout.ordinaryH);
    CHECK(layout.active);
    if (!layout.active)
        std::printf("  not active, status %d\n", (int) layout.status);
    CHECK(layout.modelW * layout.modelH < layout.ordinaryW * layout.ordinaryH);

    const Image source = SmoothPicture(W, H);
    const auto halves = ToHalves(source);
    auto src = gpu.Upload(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, 8, halves.data());
    auto packed = gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT, layout.modelW, layout.modelH, true);
    const DlssNr::GuideRegions none { { 0, 0, W, H }, { 0, 0, W, H } };
    gpu.Dispatch(Sp::MakeConstants(layout, 100, none, 1, 1), src.Get(), nullptr, nullptr, packed.Get(), nullptr);
    const Image packedImage = FromHalves(gpu.Read(packed.Get(), 8), layout.modelW, layout.modelH);

    // The pack samples the source where the layout says: the middle band, at 100%, is the source moved by whole texels.
    if (layout.ordinaryW == W)
    {
        const float shiftX = pw::Pack(layout.warp.x.bandCenter, layout.warp.x) - layout.warp.x.bandCenter;
        const float shiftY = pw::Pack(layout.warp.y.bandCenter, layout.warp.y) - layout.warp.y.bandCenter;
        int compared = 0, worst = 0;
        for (UINT y = (UINT) (layout.centerBounds.top * H) + 2; y + 2 < (UINT) (layout.centerBounds.bottom * H); y += 3)
            for (UINT x = (UINT) (layout.centerBounds.left * W) + 2; x + 2 < (UINT) (layout.centerBounds.right * W); x += 3)
            {
                const int px = (int) std::lround((float) x + shiftX), py = (int) std::lround((float) y + shiftY);
                ++compared;
                for (int c = 0; c < 3; ++c)
                    if (std::abs(packedImage.at(px, py)[c] - source.at(x, y)[c]) > 2e-3f)
                        ++worst;
            }
        CHECK(compared > 100 && worst == 0);
    }

    // Unpack the packed picture as both the input and the answer.
    auto packedSrv = gpu.Upload(DXGI_FORMAT_R16G16B16A16_FLOAT, layout.modelW, layout.modelH, 8,
                                ToHalves(packedImage).data());
    auto proxy = gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT, layout.ordinaryW, layout.ordinaryH, true);
    auto answer = gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT, layout.ordinaryW, layout.ordinaryH, true);
    gpu.Dispatch(Sp::MakeConstants(layout, 102, none, 1, 1), packedSrv.Get(), packedSrv.Get(), nullptr, proxy.Get(),
                 answer.Get());
    const Image back = FromHalves(gpu.Read(proxy.Get(), 8), layout.ordinaryW, layout.ordinaryH);
    const Image backAnswer = FromHalves(gpu.Read(answer.Get(), 8), layout.ordinaryW, layout.ordinaryH);
    CHECK(std::memcmp(back.v.data(), backAnswer.v.data(), back.v.size() * sizeof(float)) == 0); // same filter, both

    double worstMiddle = 0, worstAnywhere = 0;
    for (UINT y = 0; y < layout.ordinaryH; ++y)
        for (UINT x = 0; x < layout.ordinaryW; ++x)
        {
            // The ordinary grid at 100% is the source; elsewhere compare against the source at the same place.
            const float u = ((float) x + 0.5f) / (float) layout.ordinaryW, v = ((float) y + 0.5f) / (float) layout.ordinaryH;
            const float expect0 = 0.15f + 0.7f * u, expect1 = 0.15f + 0.7f * v;
            const bool middle = u > layout.centerBounds.left + 0.02f && u < layout.centerBounds.right - 0.02f &&
                                v > layout.centerBounds.top + 0.02f && v < layout.centerBounds.bottom - 0.02f;
            const double error = std::max(std::abs(back.at(x, y)[0] - expect0), std::abs(back.at(x, y)[1] - expect1));
            worstAnywhere = std::max(worstAnywhere, error);
            if (middle)
                worstMiddle = std::max(worstMiddle, error);
        }
    std::printf("  unpack error: middle %.5f, anywhere %.5f\n", worstMiddle, worstAnywhere);
    CHECK(worstMiddle < (layout.ordinaryW == W ? 0.004 : 0.02));
    CHECK(worstAnywhere < 0.04);
    if (layout.ordinaryW == W)
    {
        // Exactly back, in the middle, to half precision.
        int differing = 0;
        for (UINT y = (UINT) (layout.centerBounds.top * H) + 2; y + 2 < (UINT) (layout.centerBounds.bottom * H); y += 3)
            for (UINT x = (UINT) (layout.centerBounds.left * W) + 2; x + 2 < (UINT) (layout.centerBounds.right * W); x += 3)
                for (int c = 0; c < 3; ++c)
                    if (std::abs(back.at(x, y)[c] - source.at(x, y)[c]) > 2e-3f)
                        ++differing;
        CHECK(differing == 0);
    }
}

void RunGuides(Gpu& gpu, const Sp::Settings& settings, float scale, const char* label)
{
    constexpr UINT W = 320, H = 192;
    // The guides live in larger allocations, and the depth subrect starts off the origin.
    constexpr UINT DW = 352, DH = 208, MW = 336, MH = 200;
    constexpr UINT depthX = 16, depthY = 8, motionX = 4, motionY = 2;
    const Sp::Layout layout = Sp::Build(settings, W, H, scale);
    CHECK(layout.active);

    // Depth: a value no two texels share, so a blend between texels cannot pass for a source value.
    std::vector<float> depth((size_t) DW * DH);
    std::set<float> depthValues;
    for (UINT y = 0; y < DH; ++y)
        for (UINT x = 0; x < DW; ++x)
        {
            const float d = 0.001f + (float) (y * DW + x) * 1e-5f;
            depth[(size_t) y * DW + x] = d;
            if (x >= depthX && y >= depthY && x < depthX + W && y < depthY + H)
                depthValues.insert(d);
        }
    // Motion: a field that changes smoothly (half floats), in the game's own units: native pixels / mvScale.
    constexpr float mvScaleX = 2.0f, mvScaleY = -1.5f;
    const auto Motion = [](UINT x, UINT y) {
        return std::pair<float, float> { 6.0f * std::sin((float) x * 0.03f) + 2.0f, 5.0f * std::cos((float) y * 0.05f) - 1.0f };
    };
    std::vector<uint16_t> motion((size_t) MW * MH * 2);
    for (UINT y = 0; y < MH; ++y)
        for (UINT x = 0; x < MW; ++x)
        {
            const auto m = Motion(x - motionX, y - motionY);
            motion[((size_t) y * MW + x) * 2] = ToHalf(m.first);
            motion[((size_t) y * MW + x) * 2 + 1] = ToHalf(m.second);
        }

    auto depthTex = gpu.Upload(DXGI_FORMAT_R32_FLOAT, DW, DH, 4, depth.data());
    auto motionTex = gpu.Upload(DXGI_FORMAT_R16G16_FLOAT, MW, MH, 4, motion.data());
    auto colourStandIn = gpu.Upload(DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 8, 8, std::vector<uint16_t>(8 * 8 * 4).data());
    auto packedDepth = gpu.Texture(DXGI_FORMAT_R32_FLOAT, layout.modelW, layout.modelH, true);
    auto packedMotion = gpu.Texture(DXGI_FORMAT_R32G32_FLOAT, layout.modelW, layout.modelH, true);
    const DlssNr::GuideRegions regions { { depthX, depthY, W, H }, { motionX, motionY, W, H } };
    gpu.Dispatch(Sp::MakeConstants(layout, 101, regions, mvScaleX, mvScaleY), colourStandIn.Get(), depthTex.Get(),
                 motionTex.Get(), packedDepth.Get(), packedMotion.Get());

    const auto depthBytes = gpu.Read(packedDepth.Get(), 4);
    const auto motionBytes = gpu.Read(packedMotion.Get(), 8);
    const float* packedD = (const float*) depthBytes.data();
    const float* packedM = (const float*) motionBytes.data();

    int notSource = 0, motionWrong = 0, middleSame = 0, edgeShorter = 0, edgeChecked = 0, middleChecked = 0;
    for (UINT y = 0; y < layout.modelH; ++y)
        for (UINT x = 0; x < layout.modelW; ++x)
        {
            if (depthValues.find(packedD[(size_t) y * layout.modelW + x]) == depthValues.end())
                ++notSource;

            // What the end points say, computed on the CPU from the same mapping.
            const float nx = pw::Unpack((float) x + 0.5f, layout.warp.x), ny = pw::Unpack((float) y + 0.5f, layout.warp.y);
            const UINT sx = (UINT) std::clamp((int) (nx / W * W), 0, (int) W - 1);
            const UINT sy = (UINT) std::clamp((int) (ny / H * H), 0, (int) H - 1);
            const auto m = Motion(sx, sy);
            const float mx = m.first * mvScaleX, my = m.second * mvScaleY;
            // The half-float rounding of the source, and one texel of choice in where the sample landed.
            const float ex = pw::Pack(nx + mx, layout.warp.x) - pw::Pack(nx, layout.warp.x);
            const float ey = pw::Pack(ny + my, layout.warp.y) - pw::Pack(ny, layout.warp.y);
            const float gx = packedM[((size_t) y * layout.modelW + x) * 2], gy = packedM[((size_t) y * layout.modelW + x) * 2 + 1];
            if (std::abs(gx - ex) > 0.35f || std::abs(gy - ey) > 0.35f)
                ++motionWrong;

            // Far enough inside the band that the vector's end point is inside it too.
            const bool inMiddle = nx > layout.centerBounds.left * W + 24 && nx < layout.centerBounds.right * W - 24 &&
                                  ny > layout.centerBounds.top * H + 24 && ny < layout.centerBounds.bottom * H - 24;
            if (inMiddle && scale == 1.0f)
            {
                ++middleChecked;
                if (std::abs(gx - mx) < 0.35f && std::abs(gy - my) < 0.35f)
                    ++middleSame;
            }
            else if (!inMiddle && nx < layout.centerBounds.left * W - 4 && std::abs(mx) > 3.0f && scale == 1.0f)
            {
                // The squeezed left edge: a vector pointing outwards (negative x) is shorter than the native one.
                ++edgeChecked;
                if (std::abs(gx) < std::abs(mx) * 0.999f || mx > 0)
                    ++edgeShorter;
            }
        }
    std::printf("%s: model %ux%u, depth not from source %d, motion off %d, middle same %d/%d, edge shorter %d/%d\n", label,
                layout.modelW, layout.modelH, notSource, motionWrong, middleSame, middleChecked, edgeShorter, edgeChecked);
    CHECK(notSource == 0);
    CHECK(motionWrong == 0);
    if (scale == 1.0f)
    {
        CHECK(middleChecked > 500 && middleSame == middleChecked);
        CHECK(edgeChecked > 20);
    }
}
// A picture with sharp one-pixel text-like edges in the periphery (a HUD), smooth elsewhere.
Image TextPicture(UINT w, UINT h)
{
    Image img = SmoothPicture(w, h);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x)
        {
            const bool periphery = x < w / 10 || x >= w - w / 10 || y >= h - h / 8 || y < h / 12;
            if (periphery && ((x + y * 3) % 2 == 0 || (x / 3 + y) % 4 == 0))
            {
                float* p = img.at(x, y);
                p[0] = p[1] = p[2] = (x ^ y) & 1 ? 0.95f : 0.05f;
            }
        }
    return img;
}

// Bilinear resample, the reference picture on the ordinary grid when that is not the native one.
Image Resample(const Image& src, UINT w, UINT h)
{
    Image out;
    out.w = w;
    out.h = h;
    out.v.resize((size_t) w * h * 4);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x)
        {
            const float fx = std::clamp(((float) x + 0.5f) * src.w / w - 0.5f, 0.0f, (float) src.w - 1);
            const float fy = std::clamp(((float) y + 0.5f) * src.h / h - 0.5f, 0.0f, (float) src.h - 1);
            const UINT x0 = (UINT) fx, y0 = (UINT) fy, x1 = std::min(x0 + 1, src.w - 1), y1 = std::min(y0 + 1, src.h - 1);
            const float tx = fx - x0, ty = fy - y0;
            for (int c = 0; c < 4; ++c)
            {
                const float a = src.at(x0, y0)[c] * (1 - tx) + src.at(x1, y0)[c] * tx;
                const float b = src.at(x0, y1)[c] * (1 - tx) + src.at(x1, y1)[c] * tx;
                out.at(x, y)[c] = a * (1 - ty) + b * ty;
            }
        }
    return out;
}

// The Replace curves take the unpacked answer as the picture. With a model that changes nothing (answer == packed input) that
// picture must be the one the uncompressed path shows the model, or the edges' round trip lands on screen. Mode 102 alone
// fails that on sharp edges; mode 103, given the reference, does not, and leaves the 1:1 middle bit-identical to 102.
void RunReplace(Gpu& gpu, const Sp::Settings& settings, float scale, const char* label)
{
    constexpr UINT W = 320, H = 192;
    const Sp::Layout layout = Sp::Build(settings, W, H, scale);
    CHECK(layout.active);

    const Image source = TextPicture(W, H);
    const Image reference = layout.ordinaryW == W ? source : Resample(source, layout.ordinaryW, layout.ordinaryH);
    auto src = gpu.Upload(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, 8, ToHalves(source).data());
    auto ref = gpu.Upload(DXGI_FORMAT_R16G16B16A16_FLOAT, layout.ordinaryW, layout.ordinaryH, 8, ToHalves(reference).data());
    auto packed = gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT, layout.modelW, layout.modelH, true);
    const DlssNr::GuideRegions none { { 0, 0, W, H }, { 0, 0, W, H } };
    gpu.Dispatch(Sp::MakeConstants(layout, 100, none, 1, 1), src.Get(), nullptr, nullptr, packed.Get(), nullptr);
    gpu.Read(packed.Get(), 8); // leaves it readable

    auto proxy102 = gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT, layout.ordinaryW, layout.ordinaryH, true);
    auto answer102 = gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT, layout.ordinaryW, layout.ordinaryH, true);
    gpu.Dispatch(Sp::MakeConstants(layout, 102, none, 1, 1), packed.Get(), packed.Get(), nullptr, proxy102.Get(), answer102.Get());
    auto proxy103 = gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT, layout.ordinaryW, layout.ordinaryH, true);
    auto answer103 = gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT, layout.ordinaryW, layout.ordinaryH, true);
    gpu.Dispatch(Sp::MakeConstants(layout, 103, none, 1, 1), packed.Get(), packed.Get(), ref.Get(), proxy103.Get(), answer103.Get());

    const Image a102 = FromHalves(gpu.Read(answer102.Get(), 8), layout.ordinaryW, layout.ordinaryH);
    const Image a103 = FromHalves(gpu.Read(answer103.Get(), 8), layout.ordinaryW, layout.ordinaryH);
    double worst102 = 0, worst103 = 0;
    int centreDiffers = 0, centreChecked = 0;
    for (UINT y = 0; y < layout.ordinaryH; ++y)
        for (UINT x = 0; x < layout.ordinaryW; ++x)
        {
            for (int c = 0; c < 3; ++c)
            {
                worst102 = std::max(worst102, (double) std::abs(a102.at(x, y)[c] - reference.at(x, y)[c]));
                worst103 = std::max(worst103, (double) std::abs(a103.at(x, y)[c] - reference.at(x, y)[c]));
            }
            const float u = ((float) x + 0.5f) / layout.ordinaryW, v = ((float) y + 0.5f) / layout.ordinaryH;
            if (layout.ordinaryW == W && u > layout.centerBounds.left + 0.01f && u < layout.centerBounds.right - 0.01f &&
                v > layout.centerBounds.top + 0.01f && v < layout.centerBounds.bottom - 0.01f)
            {
                ++centreChecked;
                if (std::memcmp(a102.at(x, y), a103.at(x, y), 3 * sizeof(float)) != 0)
                    ++centreDiffers;
            }
        }
    std::printf("%s: model %ux%u ordinary %ux%u; against the uncompressed picture: mode 102 off by %.4f, mode 103 off by "
                "%.5f; middle texels that differ between them %d/%d\n",
                label, layout.modelW, layout.modelH, layout.ordinaryW, layout.ordinaryH, worst102, worst103, centreDiffers,
                centreChecked);
    CHECK(worst102 > 0.1);   // without the correction the sharp edges are lost (this is what the user saw)
    CHECK(worst103 < 0.002); // with it the picture is the uncompressed one, to half precision
    CHECK(centreDiffers == 0);
}
} // namespace

int main()
{
    Gpu gpu;
    if (!gpu.Init())
    {
        std::puts("SKIP: no D3D12 device could run the spatial shader");
        return 0;
    }
    std::printf("Adapter: %s\n", gpu.warp ? "WARP" : "hardware");

    Sp::Settings defaults;
    defaults.enabled = true;
    Sp::Settings lopsided = defaults;
    lopsided.centerX = 72; lopsided.workX = 86; lopsided.centerY = 78; lopsided.workY = 92;
    lopsided.offsetX = 3; lopsided.offsetY = -2;
    lopsided.shiftX = 1.5f; lopsided.shiftY = -0.5f;

    RunColour(gpu, defaults, 1.0f, "colour default 100%");
    RunColour(gpu, lopsided, 1.0f, "colour lopsided 100%");
    RunColour(gpu, defaults, 0.75f, "colour default 75%");
    RunGuides(gpu, defaults, 1.0f, "guides default 100%");
    RunGuides(gpu, lopsided, 1.0f, "guides lopsided 100%");
    RunGuides(gpu, defaults, 0.75f, "guides default 75%");
    RunReplace(gpu, defaults, 1.0f, "replace default 100%");
    RunReplace(gpu, defaults, 0.8f, "replace default 80%");
    RunReplace(gpu, defaults, 1.5f, "replace default 150%");
    RunReplace(gpu, lopsided, 1.0f, "replace lopsided 100%");

    if (fails == 0)
        std::puts("PASS: NR compress screen edges D3D12 pack / unpack (colour, depth point-sampled, motion end points)");
    return fails == 0 ? 0 : 1;
}
