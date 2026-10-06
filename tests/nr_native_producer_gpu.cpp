// GPU test for native::NativeProducer (OptiScaler/native/NativeProducer.cpp), no game and no model: a fake frame source
// hands it pictures and depth copies through the FrameContract, and a stand-in for DLSS-NR is passed in as a function.
// It checks, through the contract only:
//   - the first frame has no flow, later frames do; the trust mask runs with or without depth,
//   - the stand-in for NR is called with the guides (depth null when there was none) and not called when apply is off,
//   - the picture is left exactly as it was (bit for bit) when NR does nothing, whatever state it came in,
//   - the output's fence point completes, and a hard cut is reported and makes the next NR call a reset,
//   - a cut hint from the adapter restarts the flow.
//
//   vcvars64, then from the repo root:
//   cl /std:c++20 /EHsc /O2 tests\nr_native_producer_gpu.cpp OptiScaler\native\NativeProducer.cpp
//   OptiScaler\motion\OpticalFlow_Dx12.cpp OptiScaler\motion\TrustMask_Dx12.cpp d3d12.lib dxgi.lib d3dcompiler.lib

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
#include <functional>
#include <vector>

#include "../OptiScaler/native/NativeProducer.h"

using Microsoft::WRL::ComPtr;

namespace
{

constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;

float Hash(int x, int y, int seed)
{
    uint32_t h = (uint32_t) x * 374761393u + (uint32_t) y * 668265263u + (uint32_t) seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xFFFFFF) / 16777215.0f;
}

float ValueNoise(float x, float y, float scale, int seed)
{
    const float fx = x / scale, fy = y / scale;
    const int ix = (int) std::floor(fx), iy = (int) std::floor(fy);
    const float tx = fx - ix, ty = fy - iy;
    const float sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
    const float a = Hash(ix, iy, seed), b = Hash(ix + 1, iy, seed), c = Hash(ix, iy + 1, seed),
                d = Hash(ix + 1, iy + 1, seed);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

float Texture(float x, float y, int channel, int seed)
{
    return 0.45f * ValueNoise(x, y, 64, seed + channel) + 0.25f * ValueNoise(x, y, 24, seed + 20 + channel) +
           0.18f * ValueNoise(x, y, 9, seed + 40 + channel) + 0.12f * ValueNoise(x, y, 4, seed + 60 + channel);
}

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 fenceValue = 0;

    bool Init()
    {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;

        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
            {
                wprintf(L"adapter: %s\n", desc.Description);
                break;
            }
        }

        if (!device)
            return false;

        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return false;

        event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        return true;
    }

    void Submit()
    {
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++fenceValue);
        fence->SetEventOnCompletion(fenceValue, event);
        WaitForSingleObject(event, INFINITE);
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
    }

    ComPtr<ID3D12Resource> Buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r));
        return r;
    }

    // A texture filled by fill(x, y, pixel), left in the non-pixel shader resource state.
    ComPtr<ID3D12Resource> Upload(DXGI_FORMAT format, uint32_t bytesPerPixel,
                                  const std::function<void(uint32_t, uint32_t, uint8_t*)>& fill)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> tex;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&tex));

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto upload = Buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

        uint8_t* data = nullptr;
        upload->Map(0, nullptr, (void**) &data);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
                fill(x, y, data + (size_t) y * fp.Footprint.RowPitch + (size_t) x * bytesPerPixel);
        upload->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.pResource = tex.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &b);
        Submit();
        return tex;
    }

    // The mask (R8) as bytes, tightly packed.
    std::vector<uint8_t> ReadMask(ID3D12Resource* tex)
    {
        const auto desc = tex->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto readback = Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);

        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = tex;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        Submit();

        std::vector<uint8_t> out((size_t) desc.Width * desc.Height);
        uint8_t* data = nullptr;
        readback->Map(0, nullptr, (void**) &data);
        for (uint32_t y = 0; y < desc.Height; ++y)
            memcpy(&out[(size_t) y * desc.Width], data + (size_t) y * fp.Footprint.RowPitch, desc.Width);
        readback->Unmap(0, nullptr);
        return out;
    }
};

// A scene: a still background and a square at x, with a depth map where near is 1 (reversed-Z, near plane 0.1).
struct Scene
{
    int seed;
    float squareX;       // the square's left edge; negative: none
    float backgroundZ;
    float squareZ;
    float depthShift = 0;   // the depth map's square is this many pixels off the picture's (a jittered depth buffer)
};

bool InSquare(const Scene& s, uint32_t x, uint32_t y, float shift = 0)
{
    return s.squareX >= 0 && x >= s.squareX + shift && x < s.squareX + shift + 200 && y >= 260 && y < 460;
}

ComPtr<ID3D12Resource> Colour(Gpu& gpu, const Scene& s)
{
    return gpu.Upload(DXGI_FORMAT_R8G8B8A8_UNORM, 4,
                      [&](uint32_t x, uint32_t y, uint8_t* px)
                      {
                          const bool in = InSquare(s, x, y);
                          for (int c = 0; c < 3; ++c)
                          {
                              const float v = in ? Texture(x - s.squareX, (float) y, c, s.seed + 500)
                                                 : Texture((float) x, (float) y, c, s.seed);
                              px[c] = (uint8_t) std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f);
                          }
                          px[3] = 255;
                      });
}

ComPtr<ID3D12Resource> Depth(Gpu& gpu, const Scene& s, int mode = 0)
{
    return gpu.Upload(DXGI_FORMAT_R32_FLOAT, 4,
                      [&](uint32_t x, uint32_t y, uint8_t* px)
                      {
                          const bool square = InSquare(s, x, y, s.depthShift);
                          float d = 0.1f / (square ? s.squareZ : s.backgroundZ);

                          // A copy holding only part of the scene has nothing (far, 0 when reversed) elsewhere.
                          if ((mode == 1 && !square) || (mode == 2 && square))
                              d = 0.0f;

                          memcpy(px, &d, 4);
                      });
}


void Change(Gpu& gpu, ID3D12Resource* tex, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    gpu.list->ResourceBarrier(1, &b);
}

// An RGBA8 texture in the given state as bytes, tightly packed.
std::vector<uint8_t> ReadRgba(Gpu& gpu, ID3D12Resource* tex, D3D12_RESOURCE_STATES state)
{
    const auto desc = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 total = 0;
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto readback = gpu.Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    Change(gpu, tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    Change(gpu, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    gpu.Submit();

    std::vector<uint8_t> out((size_t) desc.Width * desc.Height * 4);
    uint8_t* data = nullptr;
    readback->Map(0, nullptr, (void**) &data);
    for (uint32_t y = 0; y < desc.Height; ++y)
        memcpy(&out[(size_t) y * desc.Width * 4], data + (size_t) y * fp.Footprint.RowPitch, (size_t) desc.Width * 4);
    readback->Unmap(0, nullptr);
    return out;
}

bool Check(const char* what, bool value)
{
    printf("  %-80s %s\n", what, value ? "ok" : "FAIL");
    return value;
}

// What the stand-in for DLSS-NR saw.
struct NrCalls
{
    int count = 0;
    bool reversed = false;
    bool reset = false;
    native::ColorSpace space = native::ColorSpace::Srgb;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool guidesSeen = false;       // depth and motion both present
    bool motionOnlyGuides = false; // motion present, depth null (no depth this frame)
};

} // namespace

int main()
{
    Gpu gpu;

    if (!gpu.Init())
    {
        printf("no D3D12 device\n");
        return 2;
    }

    native::NativeProducer producer;

    if (!producer.Init(gpu.device.Get()))
    {
        printf("init failed: %s\n", producer.Error().c_str());
        return 1;
    }

    NrCalls nr;
    const native::NativeProducer::ApplyFn applyNr = [&](ID3D12GraphicsCommandList*, const native::NativeFrame& f)
    {
        ++nr.count;
        nr.reversed = f.depthReversed;
        nr.reset = f.reset;
        nr.space = f.space;
        nr.state = f.pictureState;
        nr.format = f.colorFormat;
        nr.guidesSeen = f.depth != nullptr && f.motion != nullptr;
        nr.motionOnlyGuides = f.depth == nullptr && f.motion != nullptr;
        return true;
    };

    bool ok = true;
    int extraCopies = 0; // more copies of the depth than the one: a game that splits the scene over several lists

    // One frame through the contract: a picture of the scene (in the state given), with or without depth.
    auto frame = [&](const Scene& s, D3D12_RESOURCE_STATES state, bool withDepth, bool applyOn, bool cutHint,
                     native::FrameOutput& output, std::vector<uint8_t>* before, std::vector<uint8_t>* after)
    {
        auto picture = Colour(gpu, s);
        auto depth = Depth(gpu, s);

        if (state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        {
            Change(gpu, picture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, state);
            gpu.Submit();
        }

        if (before)
            *before = ReadRgba(gpu, picture.Get(), state);

        native::FrameInput input;
        input.api = native::Api::D3D12;
        input.picture = picture.Get();
        input.pictureFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        input.pictureState = state;
        input.colorSpace = native::ColorSpace::Srgb;
        input.width = kWidth;
        input.height = kHeight;
        input.cutHint = cutHint;

        if (withDepth)
        {
            input.depth[0] = depth.Get();
            input.depthCount = 1 + extraCopies;
            for (int i = 1; i < input.depthCount; ++i)
                input.depth[i] = depth.Get();
            input.depthView = DXGI_FORMAT_R32_FLOAT;
            input.depthWidth = kWidth;
            input.depthHeight = kHeight;
            input.depthReversed = true;
        }

        native::NativeProducer::Options options;
        options.apply = applyOn;

        const auto result = producer.Run(gpu.queue.Get(), input, options, applyNr, output);

        // Wait for the producer's work the way an adapter would before presenting.
        if (output.done.fence != nullptr)
        {
            output.done.fence->SetEventOnCompletion(output.done.value, gpu.event);
            WaitForSingleObject(gpu.event, 5000);
        }

        if (after)
            *after = ReadRgba(gpu, picture.Get(), state);

        return result;
    };

    const D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES kPresent = D3D12_RESOURCE_STATE_PRESENT;
    native::FrameOutput output;

    printf("frames through the contract, picture in a read state\n");
    {
        auto r = frame(Scene { 7, 300, 20.0f, 5.0f }, kRead, true, true, false, output, nullptr, nullptr);
        ok &= Check("first frame: submitted, no flow yet, NR not called", r.submitted && !r.flowValid && nr.count == 0);

        std::vector<uint8_t> before, after;

        for (int k = 0; k < 6; ++k)
            r = frame(Scene { 7, 300.0f + 12.0f * (k + 1), 20.0f, 5.0f }, kRead, true, true, false, output, &before, &after);

        ok &= Check("later frame: flow valid and the trust mask ran", r.flowValid && r.trustRan);
        ok &= Check("NR called on it with the guides", r.nativeRan && nr.count >= 1 && nr.guidesSeen);
        ok &= Check("... with reversed-Z, sRGB and the picture's state", nr.reversed && nr.space == native::ColorSpace::Srgb &&
                                                                     nr.state == kRead);
        ok &= Check("... and the picture's typed format", nr.format == DXGI_FORMAT_R8G8B8A8_UNORM);
        ok &= Check("the output reports it was processed", output.processed && output.done.fence != nullptr);
        ok &= Check("the picture is bit-identical after (NR did nothing)", before == after);
    }

    printf("picture in the present state (a back buffer)\n");
    {
        std::vector<uint8_t> before, after;
        auto r = frame(Scene { 7, 560, 20.0f, 5.0f }, kPresent, true, true, false, output, &before, &after);
        ok &= Check("flow valid, NR called with the PRESENT state", r.flowValid && r.nativeRan && nr.state == kPresent);
        ok &= Check("the picture is bit-identical after", before == after);
    }

    printf("no depth, or NR off\n");
    {
        // No depth this frame (the generic depth finder found nothing qualifying, say): the whole frame used to be
        // dropped (trustRan/nativeRan both false). It now still runs on motion alone -- a flow-only guide -- and NR
        // is still called, just without a depth guide.
        const int calls = nr.count;
        auto r = frame(Scene { 7, 572, 20.0f, 5.0f }, kRead, false, true, false, output, nullptr, nullptr);
        ok &= Check("no depth: flow runs, the mask still runs, NR still called with motion only",
                    r.flowValid && r.trustRan && r.nativeRan && nr.count == calls + 1);
        ok &= Check("... with a motion guide and no depth guide", nr.motionOnlyGuides);

        r = frame(Scene { 7, 584, 20.0f, 5.0f }, kRead, true, false, false, output, nullptr, nullptr);
        ok &= Check("NR off: mask runs, NR not called",
                    r.flowValid && r.trustRan && !r.nativeRan && nr.count == calls + 1);
    }

    printf("depth for the flow\n");
    {
        // The flow matches with depth only when it is one copy of the whole scene: one of several copies holds part of it.
        frame(Scene { 7, 300, 20.0f, 5.0f }, kRead, true, true, false, output, nullptr, nullptr);
        ok &= Check("one depth copy: the flow matches with it", producer.Flow()->UsedDepth());

        extraCopies = 1;
        frame(Scene { 7, 312, 20.0f, 5.0f }, kRead, true, true, false, output, nullptr, nullptr);
        extraCopies = 0;
        ok &= Check("two copies: the flow matches without depth", !producer.Flow()->UsedDepth());

        frame(Scene { 7, 324, 20.0f, 5.0f }, kRead, false, true, false, output, nullptr, nullptr);
        ok &= Check("no depth: the flow matches without depth", !producer.Flow()->UsedDepth());
    }

    printf("a hard cut\n");
    {
        // Settle on a still scene, then change to another one entirely.
        for (int k = 0; k < 4; ++k)
            frame(Scene { 7, 300, 20.0f, 5.0f }, kRead, true, true, false, output, nullptr, nullptr);

        bool cut = false;
        bool resetSeen = false;

        for (int k = 0; k < 8 && !cut; ++k)
        {
            const auto r = frame(Scene { 91, -1, 4.0f, 4.0f }, kRead, true, true, false, output, nullptr, nullptr);
            cut = r.sceneCut;
            resetSeen = resetSeen || nr.reset;
        }

        ok &= Check("the cut is reported within a few frames", cut);

        // The frame after the report starts over.
        const auto r = frame(Scene { 91, -1, 4.0f, 4.0f }, kRead, true, true, false, output, nullptr, nullptr);
        ok &= Check("the frame after it has no flow yet (the histories were reset)", !r.flowValid);
    }

    printf("a cut hint from the adapter\n");
    {
        for (int k = 0; k < 3; ++k)
            frame(Scene { 7, 300.0f + 5.0f * k, 20.0f, 5.0f }, kRead, true, true, false, output, nullptr, nullptr);

        const auto r = frame(Scene { 7, 330, 20.0f, 5.0f }, kRead, true, true, true, output, nullptr, nullptr);
        ok &= Check("the flow restarts: no flow on that frame", !r.flowValid);
    }

    printf(ok ? "all passed\n" : "FAILED\n");
    return ok ? 0 : 1;
}
