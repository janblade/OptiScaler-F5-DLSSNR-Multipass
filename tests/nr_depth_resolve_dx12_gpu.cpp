// GPU test for native/DepthResolveDx12.cpp, the D3D12 depth finder's resolve of a multisampled depth copy, no game: a
// multisampled depth buffer is cleared to 0.25 and a triangle at 0.75 is drawn over its upper-left half with 4 samples,
// so the pixels along the triangle's diagonal hold both depths. The resolve must give each pixel its nearest sample:
//   - every pixel is one of the two depths (no average between them),
//   - with reversed Z (near is 1) the edge pixels take 0.75, with standard Z they take 0.25,
//   - for D32 (R32_TYPELESS) and D24S8 (R24G8_TYPELESS) buffers,
//   - three runs in a row (the ring) give the same picture, and the target is left in the state it rested in,
//   - the way the depth finder takes it: the buffer copied as it is into a multisampled slot made as GenericDepth_Dx12
//   makes
//     it (typeless, depth-stencil allowed, resting readable), and resolved from there, gives the same picture,
//   - with the debug layer installed, the runtime reports no error or warning.
//
//   vcvars64, then from the repo root:
//   cl /std:c++20 /EHsc /O2 tests\nr_depth_resolve_dx12_gpu.cpp OptiScaler\native\DepthResolveDx12.cpp d3d12.lib
//   dxgi.lib d3dcompiler.lib

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../OptiScaler/native/DepthResolveDx12.h"

using Microsoft::WRL::ComPtr;

namespace
{
int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

constexpr UINT kWidth = 96, kHeight = 64;
constexpr D3D12_RESOURCE_STATES kRest =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;
    HANDLE event = nullptr;
    ComPtr<ID3D12InfoQueue> info;

    void Submit()
    {
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        Wait();
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
    }

    void Wait()
    {
        queue->Signal(fence.Get(), ++value);
        fence->SetEventOnCompletion(value, event);
        WaitForSingleObject(event, 5000);
    }
};

bool MakeGpu(Gpu& g)
{
    ComPtr<ID3D12Debug> debug;

    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device))))
        return false;

    g.device.As(&g.info);

    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g.queue));
    g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator));
    g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator.Get(), nullptr, IID_PPV_ARGS(&g.list));
    g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
    g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return g.queue != nullptr && g.list != nullptr && g.fence != nullptr;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

ComPtr<ID3D12Resource> Texture(Gpu& g, DXGI_FORMAT format, UINT samples, D3D12_RESOURCE_FLAGS flags,
                               D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = samples;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> tex;
    g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear, IID_PPV_ARGS(&tex));
    return tex;
}

// The multisampled buffer with its edge, left in NON_PIXEL_SHADER_RESOURCE (where the depth finder's copies rest).
ComPtr<ID3D12Resource> EdgeDepth(Gpu& g, DXGI_FORMAT typeless, DXGI_FORMAT dsvFormat)
{
    D3D12_CLEAR_VALUE clear {};
    clear.Format = dsvFormat;
    clear.DepthStencil.Depth = 0.25f;
    auto depth =
        Texture(g, typeless, 4, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear);

    if (depth == nullptr)
        return nullptr;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    g.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&dsvHeap));
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv {};
    dsv.Format = dsvFormat;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
    const auto handle = dsvHeap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateDepthStencilView(depth.Get(), &dsv, handle);

    const char* vsSource = "float4 VSMain(uint id : SV_VertexID) : SV_Position {\n"
                           "  float2 p = id == 0 ? float2(-1, 1) : id == 1 ? float2(1, 1) : float2(-1, -1);\n"
                           "  return float4(p, 0.75, 1); }";
    ComPtr<ID3DBlob> vs;
    D3DCompile(vsSource, std::strlen(vsSource), "edge", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vs, nullptr);

    D3D12_ROOT_SIGNATURE_DESC rootDesc {};
    ComPtr<ID3DBlob> serialized;
    D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr);
    ComPtr<ID3D12RootSignature> root;
    g.device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso {};
    pso.pRootSignature = root.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.RasterizerState.MultisampleEnable = TRUE;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.DSVFormat = dsvFormat;
    pso.SampleDesc.Count = 4;
    ComPtr<ID3D12PipelineState> state;

    if (FAILED(g.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&state))))
        return nullptr;

    const D3D12_VIEWPORT viewport { 0, 0, (float) kWidth, (float) kHeight, 0, 1 };
    const D3D12_RECT scissor { 0, 0, (LONG) kWidth, (LONG) kHeight };
    g.list->ClearDepthStencilView(handle, D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0, 0, nullptr);
    g.list->OMSetRenderTargets(0, nullptr, FALSE, &handle);
    g.list->SetGraphicsRootSignature(root.Get());
    g.list->SetPipelineState(state.Get());
    g.list->RSSetViewports(1, &viewport);
    g.list->RSSetScissorRects(1, &scissor);
    g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.list->DrawInstanced(3, 1, 0, 0);
    const auto toRead =
        Transition(depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g.list->ResourceBarrier(1, &toRead);
    g.Submit();
    return depth;
}

// How many of the target's pixels read `value`, and how many read neither `value` nor `other`.
void Count(Gpu& g, ID3D12Resource* target, float value, float other, int* matching, int* neither)
{
    const D3D12_RESOURCE_DESC desc = target->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 total = 0;
    D3D12_RESOURCE_DESC typed = desc;
    typed.Format = DXGI_FORMAT_R32_FLOAT;
    g.device->GetCopyableFootprints(&typed, 0, 1, 0, &footprint, nullptr, nullptr, &total);

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bufferDesc {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = total;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                      IID_PPV_ARGS(&readback));

    const auto in = Transition(target, kRest, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.list->ResourceBarrier(1, &in);
    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = target;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const auto out = Transition(target, D3D12_RESOURCE_STATE_COPY_SOURCE, kRest);
    g.list->ResourceBarrier(1, &out);
    g.Submit();

    *matching = *neither = 0;
    void* mapped = nullptr;
    D3D12_RANGE range { 0, (SIZE_T) total };

    if (FAILED(readback->Map(0, &range, &mapped)))
    {
        *neither = -1;
        return;
    }

    const float tolerance = 1.0f / 16777215.0f * 4.0f; // D24 is the coarsest here

    for (UINT y = 0; y < kHeight; ++y)
    {
        const float* row =
            (const float*) ((const uint8_t*) mapped + footprint.Offset + (size_t) y * footprint.Footprint.RowPitch);

        for (UINT x = 0; x < kWidth; ++x)
        {
            if (std::fabs(row[x] - value) < tolerance)
                ++*matching;
            else if (std::fabs(row[x] - other) >= tolerance)
                ++*neither;
        }
    }

    D3D12_RANGE none { 0, 0 };
    readback->Unmap(0, &none);
}

int DebugMessages(Gpu& g)
{
    if (g.info == nullptr)
        return 0;

    int count = 0;

    for (UINT64 i = 0; i < g.info->GetNumStoredMessages(); ++i)
    {
        SIZE_T size = 0;
        g.info->GetMessage(i, nullptr, &size);
        auto* message = (D3D12_MESSAGE*) malloc(size);

        if (message != nullptr && SUCCEEDED(g.info->GetMessage(i, message, &size)) &&
            (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
             message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
             message->Severity == D3D12_MESSAGE_SEVERITY_WARNING))
        {
            std::printf("      debug layer: %s\n", message->pDescription);
            ++count;
        }

        free(message);
    }

    return count;
}
} // namespace

int main()
{
    Gpu g;

    if (!MakeGpu(g))
    {
        std::printf("no D3D12 device\n");
        return 2;
    }

    std::printf("debug layer: %s\n", g.info != nullptr ? "on" : "not installed (its check is skipped)");

    struct Case
    {
        const char* name;
        DXGI_FORMAT typeless, dsv, view;
    };

    const Case cases[] = {
        { "R32_TYPELESS as D32, 4 samples", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT },
        { "R24G8_TYPELESS as D24S8, 4 samples", DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT,
          DXGI_FORMAT_R24_UNORM_X8_TYPELESS },
    };

    native::DepthResolveDx12 resolve;

    for (const auto& c : cases)
    {
        auto depth = EdgeDepth(g, c.typeless, c.dsv);
        auto target =
            Texture(g, DXGI_FORMAT_R32_TYPELESS, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, kRest, nullptr);

        if (depth == nullptr || target == nullptr)
        {
            Check(false, c.name);
            continue;
        }

        const native::DepthResolveDx12::Job job { depth.Get(), c.view, target.Get() };
        int nearReversed = 0, strayReversed = 0, nearStandard = 0, strayStandard = 0;

        const char* failed = resolve.Run(g.queue.Get(), &job, 1, true, kRest);
        g.Wait();

        if (failed == nullptr)
            Count(g, target.Get(), 0.75f, 0.25f, &nearReversed, &strayReversed);
        else
            std::printf("      not run: %s\n", failed);

        const char* failedStandard = resolve.Run(g.queue.Get(), &job, 1, false, kRest);
        g.Wait();

        if (failedStandard == nullptr)
            Count(g, target.Get(), 0.75f, 0.25f, &nearStandard, &strayStandard);

        char what[200];
        std::snprintf(what, sizeof(what), "%s: resolved, every pixel one of the two depths", c.name);
        Check(failed == nullptr && failedStandard == nullptr && strayReversed == 0 && strayStandard == 0, what);
        std::snprintf(what, sizeof(what),
                      "%s: the edge keeps its nearest sample (0.75 on %d pixels reversed, %d standard)", c.name,
                      nearReversed, nearStandard);
        Check(nearStandard > 0 && nearReversed > nearStandard + (int) kHeight / 2, what);

        // Around the ring again: the same picture each time.
        bool same = true;

        for (int run = 0; run < 3; ++run)
        {
            int nearAgain = 0, strayAgain = 0;
            same = same && resolve.Run(g.queue.Get(), &job, 1, true, kRest) == nullptr;
            g.Wait();
            Count(g, target.Get(), 0.75f, 0.25f, &nearAgain, &strayAgain);
            same = same && nearAgain == nearReversed && strayAgain == 0;
        }

        std::snprintf(what, sizeof(what), "%s: three more runs around the ring give the same picture", c.name);
        Check(same, what);

        // The depth finder's way: the game's list copies the buffer as it is into a multisampled slot, the resolve
        // reads that.
        auto slot = Texture(g, c.typeless, 4, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr);
        int nearSlot = -1, straySlot = -1;

        if (slot != nullptr)
        {
            D3D12_RESOURCE_BARRIER in[2] = { Transition(depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                        D3D12_RESOURCE_STATE_COPY_SOURCE),
                                             Transition(slot.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                        D3D12_RESOURCE_STATE_COPY_DEST) };
            g.list->ResourceBarrier(2, in);
            D3D12_TEXTURE_COPY_LOCATION dst {};
            dst.pResource = slot.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION src {};
            src.pResource = depth.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            D3D12_RESOURCE_BARRIER out[2] = { Transition(depth.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                              Transition(slot.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) };
            g.list->ResourceBarrier(2, out);
            g.Submit();

            const native::DepthResolveDx12::Job fromSlot { slot.Get(), c.view, target.Get() };

            if (resolve.Run(g.queue.Get(), &fromSlot, 1, true, kRest) == nullptr)
            {
                g.Wait();
                Count(g, target.Get(), 0.75f, 0.25f, &nearSlot, &straySlot);
            }
        }

        std::snprintf(what, sizeof(what),
                      "%s: copied into a slot as the depth finder does, then resolved: the same picture", c.name);
        Check(nearSlot == nearReversed && straySlot == 0, what);
    }

    resolve.Release();
    Check(DebugMessages(g) == 0, "the debug layer reported nothing");
    std::printf("%s (%d failed)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
