// GPU test for native/SharedFrame.cpp, the transport of every non-D3D12 adapter, no game: a D3D11 device and a D3D12 device on the
// same adapter, with a shared fence ordering the work of the two on the GPU.
//   - a D3D11 picture (an sRGB format) is copied into a shared texture, D3D12 reads exactly its bytes,
//   - D3D12 writes another pattern into the shared texture, D3D11 copies it back into the picture and reads exactly those bytes,
//   - a D3D11 depth buffer (D32_FLOAT, cleared to a value) is copied into a typeless shared texture and D3D12 reads the value,
//   - the fence orders both directions with no CPU wait between the two devices' work.
//
//   vcvars64, then from the repo root:
//   cl /std:c++20 /EHsc /O2 tests\nr_shared_frame_gpu.cpp OptiScaler\native\SharedFrame.cpp d3d11.lib d3d12.lib dxgi.lib

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../OptiScaler/native/SharedFrame.h"

using Microsoft::WRL::ComPtr;

namespace
{

constexpr uint32_t kWidth = 640;
constexpr uint32_t kHeight = 360;

bool Check(const char* what, bool value)
{
    printf("  %-84s %s\n", what, value ? "ok" : "FAIL");
    return value;
}

struct Devices
{
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context11;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> idle;
    HANDLE event = nullptr;
    UINT64 idleValue = 0;

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
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
            {
                wprintf(L"adapter: %s\n", desc.Description);
                break;
            }
        }

        if (!adapter)
            return false;

        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                                     &device11, nullptr, &context11)) ||
            FAILED(context11.As(&context4)) ||
            FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12))))
            return false;

        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
            FAILED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                               IID_PPV_ARGS(&list))) ||
            FAILED(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&idle))))
            return false;

        event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        return true;
    }

    // D3D12: close and run the list, wait for it on the CPU (only to read results; the ordering under test is on the GPU).
    void Submit12()
    {
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(idle.Get(), ++idleValue);
        idle->SetEventOnCompletion(idleValue, event);
        WaitForSingleObject(event, 10000);
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
        device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r));
        return r;
    }

    void Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        list->ResourceBarrier(1, &b);
    }

    // The texture's first plane as bytes, tightly packed (bytesPerPixel each). The queue waits on `waitValue` of `fence` first.
    std::vector<uint8_t> Read12(ID3D12Resource* tex, uint32_t bytesPerPixel, ID3D12Fence* fence, UINT64 waitValue)
    {
        const auto desc = tex->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 total = 0;
        device12->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto readback = Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

        if (fence != nullptr)
            queue->Wait(fence, waitValue);

        Transition(tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);

        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = tex;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        Transition(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        Submit12();

        std::vector<uint8_t> out((size_t) desc.Width * desc.Height * bytesPerPixel);
        uint8_t* data = nullptr;
        readback->Map(0, nullptr, (void**) &data);
        for (uint32_t y = 0; y < desc.Height; ++y)
            memcpy(&out[(size_t) y * desc.Width * bytesPerPixel], data + (size_t) y * fp.Footprint.RowPitch,
                   (size_t) desc.Width * bytesPerPixel);
        readback->Unmap(0, nullptr);
        return out;
    }

    // D3D12 writes `pixels` into the texture (COMMON in and out), then signals `fence` with `value` on the queue.
    void Write12(ID3D12Resource* tex, const std::vector<uint8_t>& pixels, uint32_t bytesPerPixel, ID3D12Fence* fence,
                 UINT64 value)
    {
        const auto desc = tex->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 total = 0;
        device12->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto upload = Buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

        uint8_t* data = nullptr;
        upload->Map(0, nullptr, (void**) &data);
        for (uint32_t y = 0; y < desc.Height; ++y)
            memcpy(data + (size_t) y * fp.Footprint.RowPitch, &pixels[(size_t) y * desc.Width * bytesPerPixel],
                   (size_t) desc.Width * bytesPerPixel);
        upload->Unmap(0, nullptr);

        Transition(tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.pResource = tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        Transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);

        // The shared fence is signalled on the queue behind the copy: D3D11 waits for it on the GPU.
        queue->Signal(fence, value);

        // The upload buffer has to live until the copy ran.
        queue->Signal(idle.Get(), ++idleValue);
        idle->SetEventOnCompletion(idleValue, event);
        WaitForSingleObject(event, 10000);
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
    }
};

std::vector<uint8_t> Pattern(uint32_t seed, size_t bytes)
{
    std::vector<uint8_t> v(bytes);
    uint32_t h = seed * 2654435761u + 1;
    for (auto& b : v)
    {
        h = h * 1664525u + 1013904223u;
        b = (uint8_t) (h >> 24);
    }
    return v;
}

// A D3D11 texture's bytes through a staging copy, tightly packed.
std::vector<uint8_t> Read11(Devices& d, ID3D11Texture2D* tex, uint32_t bytesPerPixel)
{
    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    d.device11->CreateTexture2D(&desc, nullptr, &staging);
    d.context11->CopyResource(staging.Get(), tex);

    D3D11_MAPPED_SUBRESOURCE mapped {};
    d.context11->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);

    std::vector<uint8_t> out((size_t) desc.Width * desc.Height * bytesPerPixel);
    for (uint32_t y = 0; y < desc.Height; ++y)
        memcpy(&out[(size_t) y * desc.Width * bytesPerPixel], (const uint8_t*) mapped.pData + (size_t) y * mapped.RowPitch,
               (size_t) desc.Width * bytesPerPixel);

    d.context11->Unmap(staging.Get(), 0);
    return out;
}

} // namespace

int main()
{
    Devices d;

    if (!d.Init())
    {
        printf("no D3D11/D3D12 device\n");
        return 2;
    }

    native::SharedFence fence;

    if (!fence.Create(d.device12.Get(), d.device11.Get()))
    {
        printf("shared fence: %s\n", fence.Error().c_str());
        return 1;
    }

    bool ok = true;

    // ---- the picture, both ways, in an sRGB format ----
    printf("a picture in an sRGB format, D3D11 -> D3D12 -> D3D11\n");
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        const auto pattern = Pattern(1, (size_t) kWidth * kHeight * 4);
        D3D11_SUBRESOURCE_DATA init { pattern.data(), kWidth * 4, 0 };

        ComPtr<ID3D11Texture2D> picture;
        ok &= Check("the game's picture is made", SUCCEEDED(d.device11->CreateTexture2D(&desc, &init, &picture)));

        native::SharedTexture shared;
        const DXGI_FORMAT format = native::SharedPictureFormat(desc.Format);
        ok &= Check("the sRGB format maps to UNORM", format == DXGI_FORMAT_R8G8B8A8_UNORM);
        ok &= Check("a shared texture is made and opened on D3D12",
                    shared.Create(d.device11.Get(), kWidth, kHeight, format, D3D11_BIND_SHADER_RESOURCE) &&
                        shared.Open(d.device12.Get()));

        // D3D11: copy, then signal. D3D12: wait on the GPU, then read.
        d.context11->CopyResource(shared.Tex11(), picture.Get());
        const UINT64 copied = fence.Next();
        d.context4->Signal(fence.Fence11(), copied);
        d.context11->Flush();

        const auto seen = d.Read12(shared.Res12(), 4, fence.Fence12(), copied);
        ok &= Check("D3D12 reads exactly the game's bytes", seen == pattern);

        // D3D12: write another pattern, signal. D3D11: wait on the GPU, copy back, read.
        const auto other = Pattern(2, (size_t) kWidth * kHeight * 4);
        const UINT64 written = fence.Next();
        d.Write12(shared.Res12(), other, 4, fence.Fence12(), written);

        d.context4->Wait(fence.Fence11(), written);
        d.context11->CopyResource(picture.Get(), shared.Tex11());
        const auto back = Read11(d, picture.Get(), 4);
        ok &= Check("D3D11 copies back exactly D3D12's bytes", back == other);

        ok &= Check("Matches() sees the same size and a different one", shared.Matches(kWidth, kHeight, format) &&
                                                                         !shared.Matches(kWidth + 1, kHeight, format));
    }

    // ---- depth ----
    printf("a D32_FLOAT depth buffer into a typeless shared texture\n");
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_D32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

        ComPtr<ID3D11Texture2D> depth;
        ComPtr<ID3D11DepthStencilView> dsv;
        ok &= Check("the game's depth buffer is made",
                    SUCCEEDED(d.device11->CreateTexture2D(&desc, nullptr, &depth)) &&
                        SUCCEEDED(d.device11->CreateDepthStencilView(depth.Get(), nullptr, &dsv)));

        d.context11->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.25f, 0);

        DXGI_FORMAT typeless, view;
        ok &= Check("the depth formats map", native::SharedDepthFormats(desc.Format, &typeless, &view) &&
                                                  typeless == DXGI_FORMAT_R32_TYPELESS && view == DXGI_FORMAT_R32_FLOAT);

        native::SharedTexture shared;
        ok &= Check("a shared depth copy is made and opened on D3D12",
                    shared.Create(d.device11.Get(), kWidth, kHeight, typeless, D3D11_BIND_SHADER_RESOURCE) &&
                        shared.Open(d.device12.Get()));

        d.context11->CopyResource(shared.Tex11(), depth.Get());
        const UINT64 copied = fence.Next();
        d.context4->Signal(fence.Fence11(), copied);
        d.context11->Flush();

        const auto bytes = d.Read12(shared.Res12(), 4, fence.Fence12(), copied);
        bool all = true;
        for (size_t i = 0; i < bytes.size() / 4 && all; ++i)
        {
            float v;
            memcpy(&v, &bytes[i * 4], 4);
            all = v == 0.25f;
        }

        ok &= Check("D3D12 reads the cleared depth value everywhere", all);
    }

    // ---- many frames: the fence keeps ordering ----
    printf("60 frames back to back, no CPU wait between the devices\n");
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        ComPtr<ID3D11Texture2D> picture;
        d.device11->CreateTexture2D(&desc, nullptr, &picture);

        native::SharedTexture shared;
        shared.Create(d.device11.Get(), kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        shared.Open(d.device12.Get());

        int wrong = 0;

        for (int frame = 0; frame < 60; ++frame)
        {
            const auto in = Pattern(100 + frame, (size_t) kWidth * kHeight * 4);
            d.context11->UpdateSubresource(picture.Get(), 0, nullptr, in.data(), kWidth * 4, 0);
            d.context11->CopyResource(shared.Tex11(), picture.Get());
            const UINT64 copied = fence.Next();
            d.context4->Signal(fence.Fence11(), copied);
            d.context11->Flush();

            // D3D12 reads it and writes its processed version (each byte inverted) behind the wait.
            auto seen = d.Read12(shared.Res12(), 4, fence.Fence12(), copied);

            if (seen != in)
                ++wrong;

            for (auto& b : seen)
                b = (uint8_t) ~b;

            const UINT64 written = fence.Next();
            d.Write12(shared.Res12(), seen, 4, fence.Fence12(), written);
            d.context4->Wait(fence.Fence11(), written);
            d.context11->CopyResource(picture.Get(), shared.Tex11());

            const auto back = Read11(d, picture.Get(), 4);

            for (size_t i = 0; i < back.size(); ++i)
                if (back[i] != (uint8_t) ~in[i])
                {
                    ++wrong;
                    break;
                }
        }

        ok &= Check("60 frames in and out, every byte as expected", wrong == 0);
    }

    printf(ok ? "all passed\n" : "FAILED\n");
    return ok ? 0 : 1;
}
