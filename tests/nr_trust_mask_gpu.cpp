// GPU test for OptiScaler/motion/TrustMask_Dx12.cpp, no game. A scene with a square that moves over a still background (with a
// depth map of its own), run through the real OpticalFlowDx12 and then the trust mask:
//   - the still background far from the square is trusted,
//   - the strip the square uncovers is not (the depth there was the square's),
//   - the inside of the moving square is trusted (it moves as one, at one depth),
//   - a hard cut to another scene distrusts nearly everything and is reported a few frames later,
//   - a scene that does not move at all is trusted everywhere.
//
//   vcvars64, then from the repo root:
//   cl /std:c++20 /EHsc /O2 tests\nr_trust_mask_gpu.cpp OptiScaler\motion\OpticalFlow_Dx12.cpp OptiScaler\motion\TrustMask_Dx12.cpp d3d12.lib dxgi.lib d3dcompiler.lib

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

#include "../OptiScaler/motion/OpticalFlow_Dx12.h"
#include "../OptiScaler/motion/TrustMask_Dx12.h"

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

struct Runner
{
    Gpu& gpu;
    OpticalFlowDx12 flow;
    TrustMaskDx12 trust;
    std::vector<uint8_t> mask;
    bool ok = true;

    explicit Runner(Gpu& g) : gpu(g)
    {
        if (!flow.Init(gpu.device.Get()) || !trust.Init(gpu.device.Get()))
        {
            printf("init failed: %s %s\n", flow.Error().c_str(), trust.Error().c_str());
            ok = false;
        }
    }

    // One frame through both; keeps the mask the pass produced. noDepth: no depth copy this frame (the generic
    // depth finder found nothing qualifying) -- Dispatch must still run, on flow-consistency and luma alone.
    bool lastDispatchRan = false;
    void Frame(const Scene& s, bool split = false, bool noDepth = false)
    {
        auto colour = Colour(gpu, s);
        auto depth = noDepth ? ComPtr<ID3D12Resource>() : Depth(gpu, s, split ? 1 : 0);
        auto depth2 = (split && !noDepth) ? Depth(gpu, s, 2) : ComPtr<ID3D12Resource>();

        flow.Dispatch(gpu.list.Get(), colour.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);

        if (flow.FlowValid())
        {
            TrustMaskDx12::Inputs in;
            in.flow = flow.Flow();
            in.flowWidth = flow.FlowWidth();
            in.flowHeight = flow.FlowHeight();
            in.fullPerFlow = 2.0f;
            in.lumaNow = flow.LumaOfLastFrame();
            in.lumaBefore = flow.LumaOfFrameBefore();

            if (!noDepth)
            {
                in.depths[0] = depth.Get();
                in.depthCount = 1;

                if (split)
                {
                    in.depths[1] = depth2.Get();
                    in.depthCount = 2;
                }
                in.depthFormat = DXGI_FORMAT_R32_FLOAT;
                in.depthWidth = kWidth;
                in.depthHeight = kHeight;
                in.depthReversed = true;
            }
            lastDispatchRan = trust.Dispatch(gpu.list.Get(), in);
        }

        gpu.Submit();

        if (flow.FlowValid())
            mask = gpu.ReadMask(trust.Mask());
    }

    double Mean(int x0, int y0, int x1, int y1) const // in picture pixels
    {
        double sum = 0;
        uint64_t n = 0;
        for (int y = y0 / 2; y < y1 / 2; ++y)
            for (int x = x0 / 2; x < x1 / 2; ++x)
            {
                sum += mask[(size_t) y * (kWidth / 2) + x] / 255.0;
                ++n;
            }
        return sum / n;
    }

    double ShareAbove(int x0, int y0, int x1, int y1, double level) const
    {
        uint64_t above = 0, n = 0;
        for (int y = y0 / 2; y < y1 / 2; ++y)
            for (int x = x0 / 2; x < x1 / 2; ++x)
            {
                above += mask[(size_t) y * (kWidth / 2) + x] / 255.0 >= level;
                ++n;
            }
        return (double) above / n;
    }
};

bool Check(const char* what, double value, bool pass)
{
    printf("  %-58s %6.3f   %s\n", what, value, pass ? "ok" : "FAIL");
    return pass;
}

} // namespace

int main()
{
    Gpu gpu;
    if (!gpu.Init())
    {
        printf("no D3D12 device\n");
        return 2;
    }

    bool ok = true;

    // 1. a square moving over a still background
    {
        printf("moving square over a still background\n");
        Runner run(gpu);
        if (!run.ok)
            return 1;

        const Scene base { 7, 0, 20.0f, 5.0f };
        for (int k = 0; k < 8; ++k)
        {
            Scene s = base;
            s.squareX = 100.0f + 16.0f * k;
            run.Frame(s);
        }

        if (getenv("MASK_ROW"))
        {
            const int xs = 100 + 16 * 7;
            printf("mask along y=360, x from %d step 2:", xs - 40);
            for (int px = xs - 40; px < xs + 40; px += 2)
                printf(" %d", run.mask[(size_t) (360 / 2) * (kWidth / 2) + px / 2]);
            printf("\n");
        }

        const int x = 100 + 16 * 7; // the square's left edge in the last frame
        ok &= Check("far background, mean mask", run.Mean(700, 60, 1200, 200), run.Mean(700, 60, 1200, 200) < 0.05);
        ok &= Check("uncovered strip, share of mask >= 0.5",
                    run.ShareAbove(x - 16 + 4, 300, x - 4, 420, 0.5), run.ShareAbove(x - 16 + 4, 300, x - 4, 420, 0.5) > 0.7);
        ok &= Check("inside the square, mean mask", run.Mean(x + 40, 300, x + 160, 420), run.Mean(x + 40, 300, x + 160, 420) < 0.25);

        // 2. a hard cut to another scene, then the same scene held
        printf("hard cut\n");
        const Scene other { 91, -1, 4.0f, 4.0f };
        run.Frame(other);
        ok &= Check("on the cut frame, mean mask", run.Mean(0, 0, kWidth, kHeight), run.Mean(0, 0, kWidth, kHeight) > 0.8);

        for (int k = 0; k < 3; ++k)
            run.Frame(other);

        ok &= Check("scene cut reported a few frames later (share)", run.trust.DistrustedShare(), run.trust.SceneCutSeen());
    }

    // 3. nothing moves
    {
        printf("static scene\n");
        Runner run(gpu);
        if (!run.ok)
            return 1;

        const Scene still { 7, 300, 20.0f, 5.0f };
        for (int k = 0; k < 8; ++k)
            run.Frame(still);

        ok &= Check("whole picture, mean mask", run.Mean(0, 0, kWidth, kHeight), run.Mean(0, 0, kWidth, kHeight) < 0.03);
        ok &= Check("no scene cut", run.trust.DistrustedShare(), !run.trust.SceneCutSeen());
    }

    // 4. nothing moves, but the depth buffer is jittered by a pixel from frame to frame, as a game that anti-aliases does
    {
        printf("static scene, jittered depth\n");
        Runner run(gpu);
        if (!run.ok)
            return 1;

        for (int k = 0; k < 8; ++k)
        {
            Scene still { 7, 300, 20.0f, 5.0f };
            still.depthShift = (k % 2) ? 1.0f : 0.0f;
            run.Frame(still);
        }

        ok &= Check("whole picture, mean mask", run.Mean(0, 0, kWidth, kHeight), run.Mean(0, 0, kWidth, kHeight) < 0.03);
        ok &= Check("along the square's edges, mean mask", run.Mean(280, 300, 320, 420),
                    run.Mean(280, 300, 320, 420) < 0.1);
    }

    // 5. nothing moves, and the depth comes in two partial copies on every other frame (the game's lists split the scene
    //    differently each frame): the nearest surface over the copies is the scene either way
    {
        printf("static scene, depth split over two copies on alternate frames\n");
        Runner run(gpu);
        if (!run.ok)
            return 1;

        for (int k = 0; k < 8; ++k)
        {
            const Scene still { 7, 300, 20.0f, 5.0f };
            run.Frame(still, (k % 2) != 0);
        }

        ok &= Check("whole picture, mean mask", run.Mean(0, 0, kWidth, kHeight), run.Mean(0, 0, kWidth, kHeight) < 0.03);
        ok &= Check("the square, mean mask", run.Mean(320, 300, 480, 420), run.Mean(320, 300, 480, 420) < 0.05);
    }

    // 6. no depth at all (depthCount == 0, as when the scene's generic depth finder qualifies nothing for this
    //    camera angle): Dispatch must still run on flow-consistency and luma alone, and BuildGuides must still
    //    produce a flow-only guide -- a valid GuideMotion() with GuideDepth() left null, not a stale depth guide
    //    from an earlier frame that did have depth.
    {
        printf("no depth at all\n");
        Runner run(gpu);
        if (!run.ok)
            return 1;

        // A couple of frames with real depth first (the first establishes flow history only), so GuideDepth()
        // has something stale it could wrongly keep handing out.
        run.Frame(Scene { 7, 300, 20.0f, 5.0f });
        run.Frame(Scene { 7, 300, 20.0f, 5.0f });
        {
            auto colour = Colour(gpu, Scene { 7, 300, 20.0f, 5.0f });
            auto depth = Depth(gpu, Scene { 7, 300, 20.0f, 5.0f });
            TrustMaskDx12::Inputs in;
            in.flow = run.flow.Flow();
            in.flowWidth = run.flow.FlowWidth();
            in.flowHeight = run.flow.FlowHeight();
            in.fullPerFlow = 2.0f;
            in.lumaNow = run.flow.LumaOfLastFrame();
            in.lumaBefore = run.flow.LumaOfFrameBefore();
            in.depths[0] = depth.Get();
            in.depthCount = 1;
            in.depthFormat = DXGI_FORMAT_R32_FLOAT;
            in.depthWidth = kWidth;
            in.depthHeight = kHeight;
            in.depthReversed = true;
            const bool built = run.trust.BuildGuides(gpu.list.Get(), in, kWidth, kHeight);
            gpu.Submit();
            ok &= Check("with depth: BuildGuides succeeds and reports a depth guide", built ? 1.0 : 0.0,
                        built && run.trust.GuideDepth() != nullptr && run.trust.GuideMotion() != nullptr);
        }

        // Now a run of frames with no depth at all.
        for (int k = 0; k < 8; ++k)
            run.Frame(Scene { 7, 320.0f + 10.0f * k, 20.0f, 5.0f }, false, true);

        ok &= Check("no depth: Dispatch still runs (flow-consistency and luma alone)", run.lastDispatchRan ? 1.0 : 0.0,
                    run.lastDispatchRan);
        ok &= Check("no depth: still trusts a static background reasonably", run.Mean(700, 60, 1200, 200),
                    run.Mean(700, 60, 1200, 200) < 0.25);

        {
            TrustMaskDx12::Inputs in;
            in.flow = run.flow.Flow();
            in.flowWidth = run.flow.FlowWidth();
            in.flowHeight = run.flow.FlowHeight();
            const bool built = run.trust.BuildGuides(gpu.list.Get(), in, kWidth, kHeight);
            gpu.Submit();
            ok &= Check("no depth: BuildGuides still succeeds, with a motion guide", built ? 1.0 : 0.0,
                        built && run.trust.GuideMotion() != nullptr);
            ok &= Check("no depth: GuideDepth() is null, not a stale depth guide from the earlier frame",
                        run.trust.GuideDepth() == nullptr ? 1.0 : 0.0, run.trust.GuideDepth() == nullptr);
        }
    }

    printf(ok ? "all passed\n" : "FAILED\n");
    return ok ? 0 : 1;
}
