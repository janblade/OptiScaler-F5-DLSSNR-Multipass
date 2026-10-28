#include "pch.h"
#include <memory>
#include <functional>
#include <DirectXPackedVector.h>
#include <d3d12sdklayers.h>
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#include "../../OptiScaler/shaders/dlssnr/superior/Constants.h"
namespace FinishedFixture
{
using DlssNr::Superior::DlssNrConstants;
#include "interpass_fixture.h"
}
using FinishedFixture::Runner;
using FinishedFixture::check;
using Microsoft::WRL::ComPtr;
#define LOG_ERROR(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)
namespace Util { void GetDeviceRemovedReason(ID3D12Device*) { throw std::runtime_error("Device removed"); } }
struct ScopedSkipHeapCapture {}; // Resource tracker instrumentation only; no GPU behaviour is replaced.
struct Config
{
    struct Setting { bool value_or_default() { return true; } } UsePrecompiledShaders;
    static Config* Instance() { static Config value; return &value; }
};
#include "../../OptiScaler/shaders/Shader_Dx12Utils.h"
#include "../../OptiScaler/shaders/dlssnr/DlssNr_FinishedRaster.h"
#include "../../OptiScaler/shaders/dlssnr/precompile/dlssnr_finished_color_Shader.h"
#include "../../OptiScaler/shaders/dlssnr/precompile/dlssnr_finished_pq_direct_Shader.h"
static void expect(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }

// Only the owning class shell is a seam. All PQ methods, base shader methods, descriptor
// placement, format support checks, constants and root/heap initialization are production bodies.
class ProductionBase
{
  public:
    std::string _name;
    ID3D12Device* _device;
    bool _init = false;
    ID3D12RootSignature* _rootSignature = nullptr;
    uint32_t _srcCount = 0, _uavCount = 0, _cbvCount = 0, _rtvCount = 0, _samplerCount = 0;
    std::vector<CD3DX12_DESCRIPTOR_RANGE1> _descriptorRanges;
    ProductionBase(std::string name, ID3D12Device* device) : _name(name), _device(device) {}
    ~ProductionBase() { SAFE_RELEASE(_rootSignature); }
    ComPtr<ID3DBlob> CompileShader(const char*, const char*, const char*) { throw std::runtime_error("Unexpected source compilation"); }
    static DXGI_FORMAT TranslateTypelessFormats(DXGI_FORMAT);
    static bool CreateComputeShader(ID3D12Device*, ID3D12RootSignature*, ID3D12PipelineState**, ID3DBlob*, D3D12_SHADER_BYTECODE);
    bool CreateComputePipeline(ID3D12Device*, ID3D12PipelineState**, const void*, size_t, const char*);
    void CreateShaderResourceView(ID3D12Device*, ID3D12Resource*, D3D12_CPU_DESCRIPTOR_HANDLE, DXGI_FORMAT = DXGI_FORMAT_UNKNOWN);
    void CreateUnorderedAccessView(ID3D12Device*, ID3D12Resource*, D3D12_CPU_DESCRIPTOR_HANDLE, uint32_t);
    bool SetupRootSignature(ID3D12Device*, uint32_t, uint32_t, uint32_t, uint32_t = 0, uint32_t = 0, uint32_t = 0,
                            const D3D12_STATIC_SAMPLER_DESC* = nullptr, D3D12_ROOT_SIGNATURE_FLAGS = D3D12_ROOT_SIGNATURE_FLAG_NONE);
    bool InitHeaps(ID3D12Device*, FrameDescriptorHeap*, size_t);
};
#include "FinishedShaderMethods.inl"
#define DLSSNR_NUM_OF_HEAPS 96
class ProductionCompose : public ProductionBase
{
  public:
    static constexpr uint32_t kSrvCount = 6, kUavCount = 2;
    FrameDescriptorHeap _frameHeaps[DLSSNR_NUM_OF_HEAPS];
    ID3D12Resource* _constantBuffers[DLSSNR_NUM_OF_HEAPS] {};
    uint32_t _heapIndex = 0, _numThreadsX = 8, _numThreadsY = 8;
    ID3D12PipelineState* _finishedColorPipelineState = nullptr;
    ID3D12PipelineState* _finishedPqDirectPipelineState = nullptr;
    std::array<uint8_t, 3> _finishedPqDirectSupport {};
    bool _finishedPqDirectPipelineFailed = false;
    std::unique_ptr<DlssNr::FinishedPqRaster> _finishedPqRaster;
    ProductionCompose(std::string, ID3D12Device*);
    ~ProductionCompose()
    {
        for (auto& resource : _constantBuffers) SAFE_RELEASE(resource);
        SAFE_RELEASE(_finishedColorPipelineState);
        SAFE_RELEASE(_finishedPqDirectPipelineState);
    }
    bool FinishedPqDirectReady(DXGI_FORMAT);
    bool RecordFinishedPqRaster(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*, D3D12_RESOURCE_STATES);
    bool DispatchResidualPass(ID3D12GraphicsCommandList*, const DlssNrConstants&, ID3D12Resource*, ID3D12Resource*,
                              ID3D12Resource*, ID3D12Resource*, ID3D12Resource*);
};
#include "FinishedComposeConstructor.inl"
#include "FinishedComposeMethods.inl"
static ProductionCompose* g_compose;
static unsigned uavBarriers;
// Instrument one real API call; forward the unchanged barrier to the real D3D12 list.
static void ObserveUavBarrier(ID3D12GraphicsCommandList* cmd, UINT count, const D3D12_RESOURCE_BARRIER* barriers)
{
    expect(count == 1 && barriers[0].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV, "Wrong UAV ordering barrier");
    ++uavBarriers;
    cmd->ResourceBarrier(count, barriers);
}
static void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &b);
}
#include "FinishedPqEncodeObserved.inl"
#include "FinishedTestImages.inl"

static void productionPq(Runner& r, ProductionCompose& compose, DXGI_FORMAT format, bool raster,
                         D3D12_RESOURCE_STATES state)
{
    const UINT w = 67, h = 41, stride = format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
    Image linear(r, texture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
    Image target(r, texture(w, h, format, raster ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
    Image encoded(r, texture(w, h, format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
    linear.fill([](char* p, UINT x, UINT y) {
        uint16_t v[4];
        for (unsigned c = 0; c < 4; ++c)
            v[c] = DirectX::PackedVector::XMConvertFloatToHalf(float((x * 17 + y * 29 + c * 31) % 401) / 8);
        std::memcpy(p, v, 8);
    }, 8);
    target.fill([&](char* p, UINT x, UINT y) {
        if (stride == 8) { uint16_t v[4] = {0x3800, 0x3400, 0x3C00, uint16_t((x + y) % 2 ? 0x3800 : 0x3C00)}; std::memcpy(p, v, 8); }
        else { uint32_t v = 0x13579BDFu * (x + 1) + y * 17; std::memcpy(p, &v, 4); }
    }, stride);
    DlssNrConstants constants {};
    constants.Mode = 1; constants.Width = w; constants.Height = h;
    // Independent unchanged committed mode-1 CS, on the old fixture root/table (not the class root).
    auto cb = r.buffer(sizeof(constants), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* data;
    check(cb->Map(0, nullptr, &data)); std::memcpy(data, &constants, sizeof(constants)); cb->Unmap(0, nullptr);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
    pd.pRootSignature = r.root.Get(); pd.CS = {dlssnr_finished_color_cso, sizeof(dlssnr_finished_color_cso)};
    ComPtr<ID3D12PipelineState> referencePso;
    check(r.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&referencePso)));
    auto cpu = r.heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < 6; ++i)
    {
        auto* source = i == 2 ? target.gpu.Get() : linear.gpu.Get();
        D3D12_SHADER_RESOURCE_VIEW_DESC d {}; d.Format = source->GetDesc().Format;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; d.Texture2D.MipLevels = 1;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        r.device->CreateShaderResourceView(source, &d, {cpu.ptr + i * r.stride});
    }
    for (UINT i = 6; i < 8; ++i)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d {}; d.Format = format; d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        r.device->CreateUnorderedAccessView(encoded.gpu.Get(), nullptr, &d, {cpu.ptr + i * r.stride});
    }
    r.begin(); linear.reset(); target.reset();
    r.barrier(linear.gpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    r.barrier(target.gpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    r.barrier(encoded.gpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    r.list->SetComputeRootSignature(r.root.Get()); r.list->SetPipelineState(referencePso.Get());
    ID3D12DescriptorHeap* heaps[] = {r.heap.Get()}; r.list->SetDescriptorHeaps(1, heaps);
    r.list->SetComputeRootDescriptorTable(0, r.heap->GetGPUDescriptorHandleForHeapStart());
    r.list->SetComputeRootConstantBufferView(1, cb->GetGPUVirtualAddress());
    r.list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    r.barrier(encoded.gpu.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    r.barrier(target.gpu.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    r.list->CopyResource(target.gpu.Get(), encoded.gpu.Get());
    r.barrier(target.gpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, state);
    r.barrier(encoded.gpu.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    r.barrier(linear.gpu.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    r.finish();
    auto reference = target.read(state);
    g_compose = &compose;
    // More than one full class descriptor/constant ring; different formats, source images and both
    // compute/raster methods share that actual ring. No custom test descriptor placement is used here.
    for (unsigned n = 0; n < 100; ++n)
    {
        r.begin(); Barrier(r.list.Get(), target.gpu.Get(), state, D3D12_RESOURCE_STATE_COPY_DEST);
        target.reset(); Barrier(r.list.Get(), target.gpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, state);
        const auto previous = compose._heapIndex;
        const auto barriersBefore = uavBarriers;
        const bool direct = !raster && compose.FinishedPqDirectReady(format);
        RecordPqEncode(r.list.Get(), linear.gpu.Get(), target.gpu.Get(), state, encoded.gpu.Get(), constants, raster);
        expect(compose._heapIndex == (previous + 1) % DLSSNR_NUM_OF_HEAPS, "Production PQ ring advanced incorrectly");
        expect(uavBarriers == barriersBefore + unsigned(direct && state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
               "Production encoder omitted/duplicated same-state UAV ordering");
        r.finish();
        const auto result = target.read(state);
        for (UINT y = 0; y < h; ++y)
            expect(std::memcmp(reference.data() + y * target.footprint.Footprint.RowPitch,
                               result.data() + y * target.footprint.Footprint.RowPitch, w * stride) == 0,
                   "Production PQ root/descriptors/format translation changed output");
    }
    expect(!raster || compose._finishedPqRaster != nullptr, "Production raster wrapper did not create its PSO");
    g_compose = nullptr;
}
int main(int argc, char** argv) try
{
    const bool hardware = argc > 1 && std::string(argv[1]) == "--hardware";
    ComPtr<ID3D12Debug> debug;
    if (!hardware && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Runner r(hardware, 8);
    ProductionCompose compose("Finished Picture production test", r.device.Get());
    expect(compose._init, "Production descriptor/root setup failed");
    expect(!compose.FinishedPqDirectReady(DXGI_FORMAT_B8G8R8A8_UNORM) &&
           !compose.FinishedPqDirectReady(DXGI_FORMAT_R10G10B10A2_TYPELESS), "Unsupported PQ format passed feature gate");
    for (auto format : {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT})
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support {format};
        check(r.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
        const bool supported = (support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) &&
            (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) && (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
        expect(compose.FinishedPqDirectReady(format) == supported, "Production typed load/store capability gate disagrees with device");
        productionPq(r, compose, format, false, D3D12_RESOURCE_STATE_PRESENT);
        productionPq(r, compose, format, false, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    productionPq(r, compose, DXGI_FORMAT_R10G10B10A2_UNORM, true, D3D12_RESOURCE_STATE_PRESENT);
    // Exercise cached unsupported and failed-PSO paths with the real fallback dispatch/root/table.
    compose._finishedPqDirectSupport[0] = 1;
    productionPq(r, compose, DXGI_FORMAT_R10G10B10A2_UNORM, false, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    SAFE_RELEASE(compose._finishedPqDirectPipelineState); compose._finishedPqDirectPipelineFailed = true;
    compose._finishedPqDirectSupport[0] = 2;
    productionPq(r, compose, DXGI_FORMAT_R10G10B10A2_UNORM, false, D3D12_RESOURCE_STATE_PRESENT);
    ComPtr<ID3D12InfoQueue> messages;
    if (SUCCEEDED(r.device.As(&messages)))
        for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0; messages->GetMessage(i, nullptr, &size); std::vector<char> bytes(size);
            auto* m = reinterpret_cast<D3D12_MESSAGE*>(bytes.data()); check(messages->GetMessage(i, m, &size));
            expect(m->Severity > D3D12_MESSAGE_SEVERITY_ERROR, m->pDescription);
        }
    std::cout << "PASS: production PQ constructor/root/ring/SRV/UAV/CBV/format support/PSOs, same-state UAV and fallbacks\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
