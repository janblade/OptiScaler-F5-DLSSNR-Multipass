#include "pch.h"
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#include "../../OptiScaler/shaders/dlssnr/superior/Constants.h"
namespace FinishedFixture
{
using DlssNr::Superior::DlssNrConstants;
#include "interpass_fixture.h"
} // namespace FinishedFixture
using FinishedFixture::check;
using FinishedFixture::Runner;
using Microsoft::WRL::ComPtr;
#include <DirectXPackedVector.h>
#include <functional>
#include <d3d12sdklayers.h>
#include <iomanip>
#include "../../OptiScaler/shaders/dlssnr/DlssNr_FinishedCopy.h"
#include "../../OptiScaler/shaders/dlssnr/precompile/dlssnr_finished_color_Shader.h"
#include "../../OptiScaler/shaders/dlssnr/precompile/dlssnr_finished_pq_direct_Shader.h"

static void expect(bool value, const char* reason)
{
    if (!value)
        throw std::runtime_error(reason);
}
static void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after)
{
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    cmd->ResourceBarrier(1, &barrier);
}
struct ComposeBridge
{
    bool ready = true;
    bool FinishedPqDirectReady(DXGI_FORMAT) const { return ready; }
    std::function<bool(const ::DlssNrConstants&, ID3D12Resource*)> dispatch;
    bool DispatchResidualPass(ID3D12GraphicsCommandList*, const ::DlssNrConstants& constants, ID3D12Resource*,
                              ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource* output)
    {
        return dispatch(constants, output);
    }
};
static ComposeBridge* g_compose;
// Actual production recording, including direct/fallback states, not a duplicate of the candidate.
#include "FinishedPqEncode.inl"
static D3D12_RESOURCE_DESC texture(UINT w, UINT h, DXGI_FORMAT format,
                                   D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC d {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
    d.Format = format;
    d.Flags = flags;
    return d;
}
static D3D12_TEXTURE_COPY_LOCATION location(ID3D12Resource* resource)
{
    D3D12_TEXTURE_COPY_LOCATION result {};
    result.pResource = resource;
    result.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    return result;
}
struct Image
{
    Runner& r;
    D3D12_RESOURCE_DESC desc;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 bytes = 0;
    ComPtr<ID3D12Resource> gpu, upload, readback;
    Image(Runner& runner, D3D12_RESOURCE_DESC d) : r(runner), desc(d)
    {
        gpu = r.resource(d, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        r.device->GetCopyableFootprints(&d, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        upload = r.buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        readback = r.buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    void fill(const std::function<void(char*, UINT, UINT)>& pixel, UINT stride)
    {
        char* memory;
        check(upload->Map(0, nullptr, reinterpret_cast<void**>(&memory)));
        for (UINT y = 0; y < desc.Height; ++y)
            for (UINT x = 0; x < desc.Width; ++x)
                pixel(memory + y * footprint.Footprint.RowPitch + x * stride, x, y);
        upload->Unmap(0, nullptr);
    }
    void reset() // GPU texture must be in COPY_DEST.
    {
        auto src = location(upload.Get()), dst = location(gpu.Get());
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprint;
        r.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    std::vector<char> read(D3D12_RESOURCE_STATES state)
    {
        r.begin();
        r.barrier(gpu.Get(), state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        auto src = location(gpu.Get()), dst = location(readback.Get());
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        r.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        r.barrier(gpu.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        r.finish();
        char* memory;
        check(readback->Map(0, nullptr, reinterpret_cast<void**>(&memory)));
        std::vector<char> result(memory, memory + bytes);
        readback->Unmap(0, nullptr);
        return result;
    }
};

struct Timing
{
    Runner& r;
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> readback;
    UINT64 frequency = 0;
    std::ofstream csv;
    Timing(Runner& runner, const char* path) : r(runner), csv(path)
    {
        expect(bool(csv), "Cannot open timing CSV");
        csv << "stage,variant,block,quartet,position,ms\n" << std::setprecision(10);
        D3D12_QUERY_HEAP_DESC d {};
        d.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        d.Count = 8;
        check(r.device->CreateQueryHeap(&d, IID_PPV_ARGS(&queries)));
        readback = r.buffer(64, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        check(r.queue->GetTimestampFrequency(&frequency));
    }
    void run(const std::string& name, const std::function<void(bool)>& record)
    {
        // Separate warm-up for both paths; ABBA/BAAB, 640 retained samples per variant.
        for (unsigned block = 0; block < 8; ++block)
            for (unsigned quartet = 0; quartet < 48; ++quartet)
            {
                r.begin();
                for (unsigned p = 0; p < 4; ++p)
                {
                    const bool candidate = ((p == 1 || p == 2) != (block % 2 != 0));
                    r.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, p * 2);
                    for (unsigned repeat = 0; repeat < 4; ++repeat)
                        record(candidate);
                    r.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, p * 2 + 1);
                }
                r.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 8, readback.Get(), 0);
                r.finish();
                UINT64* times;
                check(readback->Map(0, nullptr, reinterpret_cast<void**>(&times)));
                if (quartet >= 8)
                    for (unsigned p = 0; p < 4; ++p)
                        csv << name << ',' << (((p == 1 || p == 2) != (block % 2 != 0)) ? "candidate" : "base") << ','
                            << block << ',' << quartet - 8 << ',' << p << ','
                            << double(times[p * 2 + 1] - times[p * 2]) * 1000.0 / frequency / 4 << '\n';
                readback->Unmap(0, nullptr);
            }
        csv.flush();
        std::cout << "Measured " << name << '\n' << std::flush;
    }
};

static void guideQuality(Runner& r)
{
    // Real formats, independent axes and offsets, DRS shrink/grow and missing extents.
    struct Case
    {
        DXGI_FORMAT format;
        bool depthStencil;
    };
    for (auto test : { Case { DXGI_FORMAT_R16_UNORM, false },
                       { DXGI_FORMAT_R32_FLOAT, false },
                       { DXGI_FORMAT_R16G16_FLOAT, false },
                       { DXGI_FORMAT_R32G32_FLOAT, false },
                       { DXGI_FORMAT_R32_TYPELESS, false },
                       { DXGI_FORMAT_R16_TYPELESS, true },
                       { DXGI_FORMAT_R32_TYPELESS, true } })
    {
        const auto format = test.format;
        const UINT stride = format == DXGI_FORMAT_R16_UNORM || format == DXGI_FORMAT_R16_TYPELESS ? 2
                            : format == DXGI_FORMAT_R32G32_FLOAT                                  ? 8
                                                                                                  : 4;
        Image source(r,
                     texture(67, 41, format,
                             test.depthStencil ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_NONE));
        const auto typed = format == DXGI_FORMAT_R32_TYPELESS   ? DXGI_FORMAT_R32_FLOAT
                           : format == DXGI_FORMAT_R16_TYPELESS ? DXGI_FORMAT_R16_UNORM
                                                                : format;
        Image target(r, texture(67, 41, typed));
        source.fill(
            [&](char* p, UINT x, UINT y)
            {
                for (UINT c = 0; c < stride; ++c)
                    p[c] = char(x * 31 + y * 17 + c);
            },
            stride);
        target.fill([&](char* p, UINT, UINT) { std::memset(p, 0xCD, stride); }, stride);
        r.begin();
        source.reset();
        target.reset();
        r.barrier(source.gpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        r.finish();
        for (const auto region :
             { DlssNr::GuideRegion { 5, 3, 31, 19 }, { 9, 7, 17, 11 }, { 2, 1, 61, 39 }, { 0, 0, 67, 41 } })
        {
            r.begin();
            target.reset();
            DlssNr::CopyFinishedGuide(r.list.Get(), target.gpu.Get(), source.gpu.Get(), source.desc, region);
            r.finish();
            const auto data = target.read(D3D12_RESOURCE_STATE_COPY_DEST);
            for (UINT y = 0; y < 41; ++y)
                for (UINT x = 0; x < 67; ++x)
                    for (UINT c = 0; c < stride; ++c)
                    {
                        const bool inside =
                            test.depthStencil || (x >= region.x && y >= region.y && x < region.x + region.width &&
                                                  y < region.y + region.height);
                        const char wanted = inside ? char(x * 31 + y * 17 + c) : char(0xCD);
                        expect(data[y * target.footprint.Footprint.RowPitch + x * stride + c] == wanted,
                               "Active guide copy changed a texel or copied padding");
                    }
        }
    }
    auto d = texture(67, 41, DXGI_FORMAT_R16_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    expect(!DlssNr::CanCopyFinishedGuideRegion(d, { 5, 3, 31, 19 }), "Partial depth/stencil copy accepted");
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    d.Format = DXGI_FORMAT_R24G8_TYPELESS;
    expect(!DlssNr::CanCopyFinishedGuideRegion(d, { 5, 3, 31, 19 }), "Partial multi-plane copy accepted");
    d.Format = DXGI_FORMAT_R32_FLOAT;
    expect(!DlssNr::CanCopyFinishedGuideRegion(d, { UINT_MAX, 3, 31, 19 }) &&
               !DlssNr::CanCopyFinishedGuideRegion(d, { 5, 3, UINT_MAX, 19 }),
           "Guide copy bounds overflow");
    const auto regions = DlssNr::ResolveGuideRegions({ 2560, 1440 }, { 4096, 2304 }, { 1920, 1080 }, { 3840, 2160 },
                                                     false, 8, 4, 16, 12);
    expect(regions.depth.width == 1920 && regions.depth.x == 8 && regions.motion.width == 3840 &&
               regions.motion.y == 12,
           "Depth and output-resolution MV regions were conflated");
    std::cout << "PASS: exact active guide texels, padding, formats, DRS, offsets, bounds and depth/stencil fallback\n";
}

static ComPtr<ID3D12PipelineState> pipeline(Runner& r, const void* data, size_t size)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC d {};
    d.pRootSignature = r.root.Get();
    d.CS = { data, size };
    ComPtr<ID3D12PipelineState> p;
    const auto result = r.device->CreateComputePipelineState(&d, IID_PPV_ARGS(&p));
    if (FAILED(result))
    {
        ComPtr<ID3D12InfoQueue> messages;
        if (SUCCEEDED(r.device.As(&messages)))
            for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i)
            {
                SIZE_T size = 0;
                messages->GetMessage(i, nullptr, &size);
                std::vector<char> storage(size);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                messages->GetMessage(i, message, &size);
                std::cerr << message->pDescription << '\n';
            }
    }
    check(result);
    return p;
}

static void pq(Runner& r, UINT w, UINT h, DXGI_FORMAT format, Timing* timing, bool uav = true)
{
    std::cout << "Checking PQ format " << format << std::endl;
    const UINT stride = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
    Image linear(r, texture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
    Image picture(r,
                  texture(w, h, format, uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE));
    Image encoded(r, texture(w, h, format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
    linear.fill(
        [](char* p, UINT x, UINT y)
        {
            uint16_t v[4];
            for (UINT c = 0; c < 4; ++c)
                v[c] = DirectX::PackedVector::XMConvertFloatToHalf(float(int((x * 31 + y * 17 + c * 11) % 4096) - 64) /
                                                                   32);
            if (x % 29 == 0)
                v[0] = 0x7E00; // NaN fallback must preserve original RGB/alpha.
            if (x % 31 == 0)
                v[1] = 0x7C00;
            std::memcpy(p, v, 8);
        },
        8);
    picture.fill(
        [&](char* p, UINT x, UINT y)
        {
            if (stride == 8)
            {
                uint16_t v[4];
                for (UINT c = 0; c < 4; ++c)
                    v[c] = DirectX::PackedVector::XMConvertFloatToHalf(float((x + y + c) % 17) / 16);
                if (x % 13 == 0)
                {
                    v[0] = 0x7E55;
                    v[3] = 0x8000;
                }
                if (x % 17 == 0)
                {
                    v[1] = 0x7C00;
                    v[3] = 0x7E23;
                }
                std::memcpy(p, v, 8);
            }
            else
            {
                const uint32_t bits = 0x13579BDFu * (x + 1) + y * 17;
                std::memcpy(p, &bits, 4);
            }
        },
        stride);
    ::DlssNrConstants c {};
    c.Mode = 1;
    c.Width = w;
    c.Height = h;
    static_assert(offsetof(::DlssNrConstants, Width) == 8 && offsetof(::DlssNrConstants, MaxRatio) == 28);
    auto cb = r.buffer(sizeof(c), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* memory;
    check(cb->Map(0, nullptr, &memory));
    std::memcpy(memory, &c, sizeof(c));
    cb->Unmap(0, nullptr);
    const auto old = pipeline(r, dlssnr_finished_color_cso, sizeof(dlssnr_finished_color_cso));
    const auto direct = pipeline(r, dlssnr_finished_pq_direct_cso, sizeof(dlssnr_finished_pq_direct_cso));
    auto cpu = r.heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT table = 0; table < 3; ++table)
    {
        for (UINT i = 0; i < 6; ++i)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC v {};
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            v.Texture2D.MipLevels = 1;
            auto* input = table == 2 || (i == 2 && table == 0) ? picture.gpu.Get() : linear.gpu.Get();
            v.Format = input->GetDesc().Format;
            r.device->CreateShaderResourceView(input, &v, { cpu.ptr + (table * 8 + i) * r.stride });
        }
        for (UINT i = 6; i < 8; ++i)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC v {};
            v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            v.Format = table == 2 ? linear.desc.Format : format;
            r.device->CreateUnorderedAccessView(table == 2          ? linear.gpu.Get()
                                                : table == 1 && uav ? picture.gpu.Get()
                                                                    : encoded.gpu.Get(),
                                                nullptr, &v, { cpu.ptr + (table * 8 + i) * r.stride });
        }
    }
    r.begin();
    linear.reset();
    picture.reset();
    r.barrier(linear.gpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    r.barrier(picture.gpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    r.barrier(encoded.gpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    r.finish();
    const auto record = [&](bool changed)
    {
        if (changed)
        {
            RecordPqEncode(r.list.Get(), linear.gpu.Get(), picture.gpu.Get(), D3D12_RESOURCE_STATE_PRESENT,
                           encoded.gpu.Get(), c);
            return;
        }
        r.barrier(linear.gpu.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        r.barrier(picture.gpu.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        r.list->SetComputeRootSignature(r.root.Get());
        ID3D12DescriptorHeap* heaps[] = { r.heap.Get() };
        r.list->SetDescriptorHeaps(1, heaps);
        r.list->SetPipelineState(old.Get());
        const auto gpu = r.heap->GetGPUDescriptorHandleForHeapStart();
        r.list->SetComputeRootDescriptorTable(0, gpu);
        r.list->SetComputeRootConstantBufferView(1, cb->GetGPUVirtualAddress());
        r.list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        r.barrier(encoded.gpu.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        r.barrier(picture.gpu.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        r.list->CopyResource(picture.gpu.Get(), encoded.gpu.Get());
        r.barrier(picture.gpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        r.barrier(encoded.gpu.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        r.barrier(linear.gpu.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    };
    bool forceFallback = false;
    ComposeBridge bridge;
    bridge.dispatch = [&](const ::DlssNrConstants& constants, ID3D12Resource* output)
    {
        expect(constants.Mode == 1 || constants.Mode == 6, "Unexpected production PQ mode");
        const bool changed = constants.Mode == 6;
        if (changed && forceFallback)
            return false; // Unsupported typed loads or failed PSO.
        expect(output == (changed ? picture.gpu.Get() : encoded.gpu.Get()), "Production PQ output mismatch");
        r.list->SetComputeRootSignature(r.root.Get());
        ID3D12DescriptorHeap* heaps[] = { r.heap.Get() };
        r.list->SetDescriptorHeaps(1, heaps);
        r.list->SetPipelineState(changed ? direct.Get() : old.Get());
        const auto gpu = r.heap->GetGPUDescriptorHandleForHeapStart();
        r.list->SetComputeRootDescriptorTable(0, { gpu.ptr + (changed ? 8 : 0) * r.stride });
        r.list->SetComputeRootConstantBufferView(1, cb->GetGPUVirtualAddress());
        r.list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        return true;
    };
    g_compose = &bridge;
    r.begin();
    record(false);
    r.finish();
    const auto reference = picture.read(D3D12_RESOURCE_STATE_PRESENT);
    r.begin();
    r.barrier(picture.gpu.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    picture.reset();
    r.barrier(picture.gpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    record(true);
    r.finish();
    const auto candidate = picture.read(D3D12_RESOURCE_STATE_PRESENT);
    for (UINT y = 0; y < h; ++y)
        expect(std::memcmp(reference.data() + y * picture.footprint.Footprint.RowPitch,
                           candidate.data() + y * picture.footprint.Footprint.RowPitch, w * stride) == 0,
               "Direct PQ encoder differs from independent committed shader + copy");
    for (const bool ready : { false, true })
    {
        bridge.ready = ready;
        forceFallback = true;
        r.begin();
        r.barrier(picture.gpu.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        picture.reset();
        r.barrier(picture.gpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        record(true);
        r.finish();
        const auto fallback = picture.read(D3D12_RESOURCE_STATE_PRESENT);
        for (UINT y = 0; y < h; ++y)
            expect(std::memcmp(reference.data() + y * picture.footprint.Footprint.RowPitch,
                               fallback.data() + y * picture.footprint.Footprint.RowPitch, w * stride) == 0,
                   "Production unsupported/failed direct encoder fallback changed the picture");
    }
    forceFallback = false;
    std::cout << "PASS: bit-exact PQ format " << format << " at " << w << 'x' << h << ", UAV=" << uav << '\n';
    if (timing)
    {
        timing->run("pq_encode_" + std::to_string(format), record);
        c.Mode = 0;
        auto decodeCb = r.buffer(sizeof(c), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        check(decodeCb->Map(0, nullptr, &memory));
        std::memcpy(memory, &c, sizeof(c));
        decodeCb->Unmap(0, nullptr);
        Image guides(r, texture(2560, 1440, DXGI_FORMAT_R16G16_FLOAT));
        Image savedDepth(r, guides.desc), savedMotion(r, guides.desc);
        guides.fill(
            [](char* p, UINT x, UINT y)
            {
                const UINT v = x * 31 + y;
                std::memcpy(p, &v, 4);
            },
            4);
        r.begin();
        guides.reset();
        r.barrier(guides.gpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        r.finish();
        // Changed overhead: full depth capture, active MV capture, decode PQ and encode/write it back.
        // Does not execute NR/DLSS or model-dependent dispatches; never label it full game timing.
        timing->run(
            "finished_overhead_no_models_4k_drs1080",
            [&](bool changed)
            {
                for (auto* saved : { savedDepth.gpu.Get(), savedMotion.gpu.Get() })
                {
                    r.barrier(guides.gpu.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_COPY_SOURCE);
                    if (changed && saved == savedMotion.gpu.Get())
                        DlssNr::CopyFinishedGuide(r.list.Get(), saved, guides.gpu.Get(), guides.desc,
                                                  { 0, 0, 1920, 1080 });
                    else
                        r.list->CopyResource(saved, guides.gpu.Get());
                    r.barrier(guides.gpu.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    r.barrier(saved, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    r.barrier(saved, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                }
                r.barrier(picture.gpu.Get(), D3D12_RESOURCE_STATE_PRESENT,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                r.list->SetComputeRootSignature(r.root.Get());
                ID3D12DescriptorHeap* heaps[] = { r.heap.Get() };
                r.list->SetDescriptorHeaps(1, heaps);
                r.list->SetPipelineState(old.Get());
                const auto gpu = r.heap->GetGPUDescriptorHandleForHeapStart();
                r.list->SetComputeRootDescriptorTable(0, { gpu.ptr + 16 * r.stride });
                r.list->SetComputeRootConstantBufferView(1, decodeCb->GetGPUVirtualAddress());
                r.list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                r.barrier(picture.gpu.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                          D3D12_RESOURCE_STATE_PRESENT);
                record(changed);
            });
    }
    g_compose = nullptr;
}

int main(int argc, char** argv)
try
{
    const bool hardware = argc > 1 && std::string(argv[1]) == "--hardware";
    ComPtr<ID3D12Debug> debug;
    if (!hardware && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();
    Runner r(hardware, 24);
    guideQuality(r);
    for (auto format : { DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT })
        pq(r, 67, 41, format, nullptr);
    pq(r, 67, 41, DXGI_FORMAT_R10G10B10A2_UNORM, nullptr, false);
    if (argc > 2)
    {
        Timing timing(r, argv[2]);
        pq(r, 3840, 2160, DXGI_FORMAT_R10G10B10A2_UNORM, &timing);
        for (auto active : { DlssNr::GuideRegion { 0, 0, 1920, 1080 },
                             { 8, 4, 1280, 720 },
                             { 0, 0, 2558, 1439 },
                             { 0, 0, 2560, 1440 } })
        {
            Image source(r, texture(2560, 1440, DXGI_FORMAT_R16G16_FLOAT));
            Image dest(r, source.desc);
            source.fill(
                [](char* p, UINT x, UINT y)
                {
                    const UINT v = x * 31 + y;
                    std::memcpy(p, &v, 4);
                },
                4);
            r.begin();
            source.reset();
            r.barrier(source.gpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            r.finish();
            timing.run("mv_copy_" + std::to_string(active.width),
                       [&](bool changed)
                       {
                           r.barrier(source.gpu.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                     D3D12_RESOURCE_STATE_COPY_SOURCE);
                           if (changed)
                               DlssNr::CopyFinishedGuide(r.list.Get(), dest.gpu.Get(), source.gpu.Get(), source.desc,
                                                         active);
                           else
                               r.list->CopyResource(dest.gpu.Get(), source.gpu.Get());
                           r.barrier(source.gpu.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                           // Match inter-frame ordering of capture -> shader reads -> next capture.
                           r.barrier(dest.gpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                           r.barrier(dest.gpu.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                     D3D12_RESOURCE_STATE_COPY_DEST);
                       });
        }
    }
    if (!hardware)
    {
        ComPtr<ID3D12InfoQueue> messages;
        if (SUCCEEDED(r.device.As(&messages)))
            for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i)
            {
                SIZE_T size = 0;
                messages->GetMessage(i, nullptr, &size);
                std::vector<char> storage(size);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                check(messages->GetMessage(i, message, &size));
                if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                    throw std::runtime_error(message->pDescription);
            }
    }
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
