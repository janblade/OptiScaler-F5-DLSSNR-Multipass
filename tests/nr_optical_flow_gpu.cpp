// GPU test for OptiScaler/motion/OpticalFlow_Dx12.cpp, no game: a synthetic picture (multi-scale value noise), the same picture
// moved by a known amount, and the flow the pass reports for it. The picture is a continuous function, so a shift of half a
// pixel is exact.
//
//   vcvars64, then from the repo root:
//   cl /std:c++20 /EHsc /O2 tests\nr_optical_flow_gpu.cpp OptiScaler\motion\OpticalFlow_Dx12.cpp d3d12.lib dxgi.lib d3dcompiler.lib

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
#include <string>
#include <vector>

#include "../OptiScaler/motion/OpticalFlow_Dx12.h"

using Microsoft::WRL::ComPtr;

namespace
{

uint32_t kWidth = 1280;
uint32_t kHeight = 720;

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

// The scene: continuous in x and y.
float Scene(float x, float y, int channel)
{
    return 0.45f * ValueNoise(x, y, 64, 11 + channel) + 0.25f * ValueNoise(x, y, 24, 23 + channel) +
           0.18f * ValueNoise(x, y, 9, 37 + channel) + 0.12f * ValueNoise(x, y, 4, 51 + channel);
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

    // An RGBA8 texture holding the scene moved by (dx, dy), left in the non-pixel shader resource state.
    // gain darkens it, noise (in 1/255 steps, peak) adds a different grain to every picture (noiseSeed).
    ComPtr<ID3D12Resource> Picture(float dx, float dy, float gain = 1.0f, float noise = 0.0f, int noiseSeed = 0)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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
            {
                uint8_t* px = data + (size_t) y * fp.Footprint.RowPitch + x * 4;
                for (int c = 0; c < 3; ++c)
                {
                    float v = Scene(x - dx, y - dy, c) * 255.0f * gain;
                    if (noise > 0.0f)
                        v += (Hash((int) x * 3 + c, (int) y, 1000 + noiseSeed) * 2.0f - 1.0f) * noise;
                    px[c] = (uint8_t) std::clamp(v + 0.5f, 0.0f, 255.0f);
                }
                px[3] = 255;
            }

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
};

uint16_t* g_unused = nullptr;

float Half(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, mant = h & 1023;
    float v;
    if (exp == 0)
        v = std::ldexp((float) mant, -24);
    else if (exp == 31)
        v = INFINITY;
    else
        v = std::ldexp((float) (mant + 1024), (int) exp - 25);
    return sign ? -v : v;
}

} // namespace

int main(int argc, char** argv)
{
    const bool perf = argc > 1 && std::string(argv[1]) == "perf";

    if (perf)
    {
        kWidth = 2560;
        kHeight = 1440;
    }

    Gpu gpu;

    if (!gpu.Init())
    {
        printf("no D3D12 device\n");
        return 2;
    }

    OpticalFlowDx12 flow;

    if (!flow.Init(gpu.device.Get()))
    {
        printf("init failed: %s\n", flow.Error().c_str());
        return 1;
    }

    if (perf)
    {
        // Frames of a pan, in batches that fit the descriptor ring, timed from the CPU around a wait on the GPU.
        std::vector<ComPtr<ID3D12Resource>> frames;
        for (int i = 0; i < 6; ++i)
            frames.push_back(gpu.Picture(i * 7.0f, i * -3.0f));

        flow.Reset();
        for (int warm = 0; warm < 3; ++warm)
        {
            for (auto& frame : frames)
                flow.Dispatch(gpu.list.Get(), frame.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
            gpu.Submit();
        }

        LARGE_INTEGER freq, a, b;
        QueryPerformanceFrequency(&freq);
        const int batches = 20;
        QueryPerformanceCounter(&a);
        for (int batch = 0; batch < batches; ++batch)
        {
            for (auto& frame : frames)
                flow.Dispatch(gpu.list.Get(), frame.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
            gpu.Submit();
        }
        QueryPerformanceCounter(&b);
        printf("%ux%u: %.3f ms per flow (wall, %d frames)\n", kWidth, kHeight,
               1000.0 * (double) (b.QuadPart - a.QuadPart) / freq.QuadPart / (batches * 6), batches * 6);
        return 0;
    }

    struct Case
    {
        float dx, dy;
        double minWithinHalf; // the share of flow samples that must be within half a pixel (0: only reported)
        float gain = 1.0f;    // picture brightness
        float noise = 0.0f;   // grain peak, in 1/255 steps, different on every picture
        int frames = 2;       // pictures in the pan (constant velocity); the last pair is measured
        const char* what = "";
    };

    const Case cases[] = {
        { 0, 0, 0.95 },
        { 3, -2, 0.90 },
        { 2.5f, 1.5f, 0.85 },
        { 11, 7, 0.90 },
        { 21, -14, 0.90 },
        { -37, 29, 0.85 },
        { 60, 0, 0.80 },
        // fast pans: reported until the flow handles them (the baseline of the flow upgrade)
        { 100, -60, 0, 1.0f, 0.0f, 2, "fast pan" },
        { -160, 40, 0, 1.0f, 0.0f, 2, "fast pan" },
        { 220, 0, 0, 1.0f, 0.0f, 2, "fast pan" },
        // the same pan a few pictures in a row: last frame's flow is known
        { 30, -18, 0, 1.0f, 0.0f, 4, "steady pan, 4 pictures" },
        { 90, 20, 0, 1.0f, 0.0f, 4, "steady fast pan, 4 pictures" },
        // dark and grainy: flat areas where block matching has little to hold on to
        { 3, -2, 0, 0.08f, 3.0f, 2, "dark and grainy" },
        { 12, 5, 0, 0.08f, 3.0f, 4, "dark and grainy, 4 pictures" },
    };
    bool ok = true;

    for (const Case& c : cases)
    {
        flow.Reset();

        // pictures 0..frames-1, picture k shifted by k times the velocity; every one but the last is submitted
        std::vector<ComPtr<ID3D12Resource>> pictures;
        for (int k = 0; k < c.frames; ++k)
            pictures.push_back(gpu.Picture(c.dx * k, c.dy * k, c.gain, c.noise, k));

        for (int k = 0; k + 1 < c.frames; ++k)
        {
            flow.Dispatch(gpu.list.Get(), pictures[k].Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
            gpu.Submit();
        }

        flow.Dispatch(gpu.list.Get(), pictures[c.frames - 1].Get(), DXGI_FORMAT_R8G8B8A8_UNORM);

        if (!flow.FlowValid())
        {
            printf("flow not valid after two frames\n");
            return 1;
        }

        // read the flow back
        ID3D12Resource* out = flow.Flow();
        const auto desc = out->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 total = 0;
        gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto readback = gpu.Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = out;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        gpu.list->ResourceBarrier(1, &b);

        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = out;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        gpu.list->ResourceBarrier(1, &b);
        gpu.Submit();

        uint8_t* data = nullptr;
        readback->Map(0, nullptr, (void**) &data);

        // Content that moved by +d was at p - d before: the flow is -d. Away from the border, where the shift brings in
        // content that was not in the previous picture.
        const int margin = (int) std::ceil(std::max(std::fabs(c.dx), std::fabs(c.dy)) / 2.0f) + 24;
        uint64_t n = 0, half = 0, one = 0, wild = 0;
        double sumErr = 0;

        for (uint32_t y = margin; y + margin < desc.Height; ++y)
            for (uint32_t x = margin; x + margin < desc.Width; ++x)
            {
                const uint16_t* px = (const uint16_t*) (data + (size_t) y * fp.Footprint.RowPitch + x * 8);
                const float ex = Half(px[0]) - (-c.dx), ey = Half(px[1]) - (-c.dy);
                const float err = std::sqrt(ex * ex + ey * ey);
                sumErr += err;
                ++n;
                half += err <= 0.5f;
                one += err <= 1.0f;
                wild += err > 3.0f;
            }

        readback->Unmap(0, nullptr);

        const double shareHalf = (double) half / n;
        const bool pass = shareHalf >= c.minWithinHalf;
        ok = ok && (pass || c.minWithinHalf <= 0);
        printf("shift (%6.1f, %6.1f): mean error %7.3f px, within 0.5 px %5.1f%%, within 1 px %5.1f%%, off by over 3 px %5.1f%%   %s  %s\n",
               c.dx, c.dy, sumErr / n, 100.0 * shareHalf, 100.0 * one / n, 100.0 * wild / n,
               c.minWithinHalf > 0 ? (pass ? "ok" : "FAIL") : "(reported)", c.what);
    }

    printf(ok ? "all passed\n" : "FAILED\n");
    return ok ? 0 : 1;
}
