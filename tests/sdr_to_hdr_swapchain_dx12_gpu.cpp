// GPU test for output/SdrToHdr_ProxyDx12.cpp, the proxy back buffers and resize logic of HDR output, on a real windowed
// flip-model swap chain, no game. The real chain is FP16 (scRGB) and the "game" works with proxies in R8G8B8A8_UNORM:
//   - off path: a chain that was never proxied hands out its own buffers (GameBuffer passes through), no proxy exists,
//   - Start: the proxies have the game's format, size and count, and GetBuffer returns them (out of range is INVALID_CALL),
//   - a flat grey drawn into the current proxy comes out of the real buffer converted (scRGB, 2.2, 203 nits),
//   - every back buffer index presents, the pass runs after the swap chain's own Present rotated the index,
//   - Resize to a new size, to a new buffer count, to unproxied (real chain back to R8G8B8A8) and to proxied again,
//   - Resize while the test still holds a proxy: INVALID_CALL, nothing changed, the chain still works after,
//   - a proxy that cannot be made after the real resize: the chain goes back to the game's format (reverted),
//   - SetFullscreenState true / false (reported if WARP or this session refuses it),
//   - the device's reference count is the same at the end, and the debug layer reports nothing.
//
// Run on the hardware adapter or on WARP (argument "warp").
//
//   vcvars64, then from the repo root:
//   cl /std:c++20 /EHsc /O2 tests\sdr_to_hdr_swapchain_dx12_gpu.cpp OptiScaler\output\SdrToHdr_ProxyDx12.cpp
//      OptiScaler\output\SdrToHdr_Dx12.cpp d3d12.lib dxgi.lib user32.lib
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../OptiScaler/output/SdrToHdr_ProxyDx12.h"

#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

namespace
{
int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

constexpr DXGI_FORMAT kGame = DXGI_FORMAT_R8G8B8A8_UNORM;

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
    ComPtr<IDXGIFactory4> factory;

    void Wait()
    {
        queue->Signal(fence.Get(), ++value);
        fence->SetEventOnCompletion(value, event);
        WaitForSingleObject(event, 10000);
    }

    void Submit()
    {
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        Wait();
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
    }
};

bool MakeGpu(Gpu& g, bool warp)
{
    ComPtr<ID3D12Debug> debug;

    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();

    ComPtr<IDXGIAdapter> adapter;

    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&g.factory))))
        return false;

    if (warp && FAILED(g.factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))))
        return false;

    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device))))
        return false;

    g.device.As(&g.info);

    // The clears of the test's own proxies have no optimized clear value: a performance note about the test
    if (g.info != nullptr)
    {
        D3D12_MESSAGE_ID hide[] = { D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE };
        D3D12_INFO_QUEUE_FILTER filter {};
        filter.DenyList.NumIDs = 1;
        filter.DenyList.pIDList = hide;
        g.info->AddStorageFilterEntries(&filter);
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g.queue));
    g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator));
    g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator.Get(), nullptr, IID_PPV_ARGS(&g.list));
    g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
    g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return g.queue != nullptr && g.list != nullptr && g.fence != nullptr;
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
            (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
             message->Severity == D3D12_MESSAGE_SEVERITY_WARNING))
        {
            std::printf("      debug layer: %s\n", message->pDescription);
            ++count;
        }

        free(message);
    }

    g.info->ClearStoredMessages();
    return count;
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

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l) { return DefWindowProcW(window, message, w, l); }

HWND MakeWindow()
{
    WNDCLASSW wc {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SdrToHdrSwapchainTest";
    RegisterClassW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"sdr_to_hdr test", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr, nullptr,
                           wc.hInstance, nullptr);
}

ComPtr<IDXGISwapChain3> MakeChain(Gpu& g, HWND window, DXGI_FORMAT format, UINT count, UINT width, UINT height)
{
    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = count;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain1;
    ComPtr<IDXGISwapChain3> chain;

    if (SUCCEEDED(g.factory->CreateSwapChainForHwnd(g.queue.Get(), window, &desc, nullptr, nullptr, &chain1)))
        chain1.As(&chain);

    return chain;
}

// Draws a flat colour into `texture` (resting in PRESENT) through a UNORM render target view: the "game"
void Paint(Gpu& g, ID3D12Resource* texture, float value)
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> heap;
    g.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap));
    D3D12_RENDER_TARGET_VIEW_DESC rtv {};
    rtv.Format = kGame;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    const auto handle = heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(texture, &rtv, handle);
    const auto in = Transition(texture, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g.list->ResourceBarrier(1, &in);
    const float color[4] = { value, value, value, 1.0f };
    g.list->ClearRenderTargetView(handle, color, 0, nullptr);
    const auto out = Transition(texture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    g.list->ResourceBarrier(1, &out);
    g.Submit();
}

float HalfToFloat(uint16_t h)
{
    const int sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
    float value = exponent == 0 ? std::ldexp((float) mantissa, -24) : std::ldexp((float) (mantissa + 1024), exponent - 25);
    return sign != 0 ? -value : value;
}

// The red channel of the first pixel of a real FP16 back buffer (resting in PRESENT)
float ReadFirstPixel(Gpu& g, ID3D12Resource* texture)
{
    D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
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
    const auto in = Transition(texture, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.list->ResourceBarrier(1, &in);
    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = texture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const auto out = Transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    g.list->ResourceBarrier(1, &out);
    g.Submit();
    uint8_t* mapped = nullptr;
    readback->Map(0, nullptr, (void**) &mapped);
    uint16_t half = 0;
    std::memcpy(&half, mapped + footprint.Offset, 2);
    readback->Unmap(0, nullptr);
    return HalfToFloat(half);
}

// The red code (10 bits) of the first pixel of a real R10G10B10A2 back buffer (resting in PRESENT)
uint32_t ReadFirstPixel10(Gpu& g, ID3D12Resource* texture)
{
    D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
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
    const auto in = Transition(texture, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.list->ResourceBarrier(1, &in);
    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = texture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const auto out = Transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    g.list->ResourceBarrier(1, &out);
    g.Submit();
    uint8_t* mapped = nullptr;
    readback->Map(0, nullptr, (void**) &mapped);
    uint32_t texel = 0;
    std::memcpy(&texel, mapped + footprint.Offset, 4);
    readback->Unmap(0, nullptr);
    return texel & 1023u;
}

ULONG RefCount(IUnknown* object)
{
    object->AddRef();
    return object->Release();
}

// What a flat SDR grey becomes (gamma 2.2, game white 203 nits, scRGB)
// (the proxy holds 8 bits: the grey is the nearest byte)
float Expected(float grey) { return std::pow(std::round(grey * 255.0f) / 255.0f, 2.2f) * 203.0f / 80.0f; }

SdrToHdr::PassConstants Constants()
{
    SdrToHdr::PassConstants k;
    k.mode = 0;
    k.gameNits = 203.0f;
    k.peakNits = 1000.0f;
    return k;
}

bool Near(float a, float b) { return std::fabs(a - b) <= 2.5e-3f; }

// Paints the current proxy, runs the pass, reads the real buffer, presents: true when the real buffer holds the conversion
bool Frame(Gpu& g, SdrToHdr::ProxyChain& chain, IDXGISwapChain3* real, float grey)
{
    grey = std::round(grey * 255.0f) / 255.0f; // exactly a byte, so the clear and the reference agree
    const UINT index = real->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> proxy, realBuffer;

    if (FAILED(chain.GetBuffer(index, IID_PPV_ARGS(&proxy))) || FAILED(real->GetBuffer(index, IID_PPV_ARGS(&realBuffer))))
        return false;

    Paint(g, proxy.Get(), grey);
    const char* why = chain.Present(g.queue.Get(), real, Constants());
    g.Wait();
    const float got = ReadFirstPixel(g, realBuffer.Get());
    proxy.Reset();
    realBuffer.Reset();
    real->Present(0, 0);
    g.Wait();
    return why == nullptr && Near(got, Expected(grey));
}
} // namespace

int main(int argc, char** argv)
{
    const bool warp = argc > 1 && std::strcmp(argv[1], "warp") == 0;
    Gpu g;

    if (!MakeGpu(g, warp))
    {
        std::printf("no D3D12 device\n");
        return 2;
    }

    std::printf("%s, debug layer: %s\n", warp ? "WARP" : "hardware adapter",
                g.info != nullptr ? "on" : "not installed (its check is skipped)");

    HWND window = MakeWindow();
    const ULONG before = RefCount(g.device.Get());

    {
        // ---- the off path: a chain that is not proxied ----
        {
            auto plain = MakeChain(g, window, kGame, 3, 64, 32);
            Check(plain != nullptr, "an R8G8B8A8 flip chain is made");

            if (plain == nullptr)
                return 2;

            SdrToHdr::ProxyChain none;
            ComPtr<ID3D12Resource> own, viaGame;
            plain->GetBuffer(0, IID_PPV_ARGS(&own));
            const HRESULT hr = SdrToHdr::GameBuffer(plain.Get(), 0, IID_PPV_ARGS(&viaGame));
            Check(!none.Active() && !SdrToHdr::AnyProxied(), "off path: no proxy chain is active and none is registered");
            Check(SUCCEEDED(hr) && viaGame.Get() == own.Get(), "off path: GameBuffer hands out the swap chain's own buffer");
            Check(none.GetBuffer(0, IID_PPV_ARGS(&viaGame)) == DXGI_ERROR_INVALID_CALL, "an inactive chain hands out nothing");
            Check(none.Present(g.queue.Get(), plain.Get(), Constants()) != nullptr, "an inactive chain does not present");
            own.Reset();
            viaGame.Reset();
        }

        // ---- a proxied chain: the real one is FP16 ----
        auto real = MakeChain(g, window, DXGI_FORMAT_R16G16B16A16_FLOAT, 3, 64, 32);
        Check(real != nullptr, "an FP16 flip chain is made");

        if (real == nullptr)
            return 2;

        SdrToHdr::ProxyChain chain;
        const char* started = chain.Start(g.device.Get(), real.Get(), kGame, false);
        Check(started == nullptr, "Start: colour space set, proxies and pass made");

        if (started != nullptr)
        {
            std::printf("      %s\n", started);
            return 2;
        }

        Check(chain.Active() && SdrToHdr::AnyProxied(), "the chain is active and registered");
        Check(chain.GameFormat() == kGame, "the chain remembers the game's format");

        {
            ComPtr<ID3D12Resource> proxy, viaGame, realBuffer;
            chain.GetBuffer(1, IID_PPV_ARGS(&proxy));
            SdrToHdr::GameBuffer(real.Get(), 1, IID_PPV_ARGS(&viaGame));
            real->GetBuffer(1, IID_PPV_ARGS(&realBuffer));
            const auto d = proxy->GetDesc();
            Check(d.Format == kGame && d.Width == 64 && d.Height == 32, "proxy: the game's format and the chain's size");
            Check(viaGame.Get() == proxy.Get() && realBuffer.Get() != proxy.Get() &&
                      realBuffer->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
                  "GameBuffer gives the proxy, the chain's own buffer stays FP16");
            void* none = reinterpret_cast<void*>(1);
            Check(chain.GetBuffer(3, IID_PPV_ARGS(reinterpret_cast<ID3D12Resource**>(&none))) == DXGI_ERROR_INVALID_CALL,
                  "GetBuffer past the buffer count is INVALID_CALL");
        }

        // every index, through the chain's own rotation
        bool all = true;

        for (int i = 0; i < 6; ++i)
            all = Frame(g, chain, real.Get(), 0.25f + 0.1f * (float) i) && all;

        Check(all, "six frames over three buffers: the real buffer holds the converted grey every time");

        SdrToHdr::ResizeRequest request;
        request.rawFormat = request.resolvedFormat = kGame;
        request.wantProxy = true;
        request.gameColourSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

        // resize: new size
        request.width = 96;
        request.height = 48;
        auto result = chain.Resize(g.device.Get(), real.Get(), request);
        Check(SUCCEEDED(result.hr) && result.proxied && !result.refused, "Resize to 96x48 stays proxied");
        {
            ComPtr<ID3D12Resource> proxy;
            chain.GetBuffer(0, IID_PPV_ARGS(&proxy));
            Check(proxy->GetDesc().Width == 96 && proxy->GetDesc().Height == 48, "proxies have the new size");
        }
        Check(Frame(g, chain, real.Get(), 0.5f), "a frame after the resize converts");

        // resize: buffer count
        request.count = 4;
        result = chain.Resize(g.device.Get(), real.Get(), request);
        Check(SUCCEEDED(result.hr) && result.proxied, "Resize to four buffers stays proxied");
        {
            ComPtr<ID3D12Resource> proxy;
            Check(SUCCEEDED(chain.GetBuffer(3, IID_PPV_ARGS(&proxy))), "the fourth proxy exists");
            proxy.Reset();
            Check(chain.GetBuffer(4, IID_PPV_ARGS(&proxy)) == DXGI_ERROR_INVALID_CALL, "and a fifth does not");
        }
        bool four = true;

        for (int i = 0; i < 5; ++i)
            four = Frame(g, chain, real.Get(), 0.3f + 0.1f * (float) i) && four;

        Check(four, "five frames over four buffers convert");

        // the game still holds a proxy
        ComPtr<ID3D12Resource> held;
        chain.GetBuffer(2, IID_PPV_ARGS(&held));
        request.count = 0;
        request.width = 80;
        request.height = 40;
        result = chain.Resize(g.device.Get(), real.Get(), request);
        Check(result.hr == DXGI_ERROR_INVALID_CALL && result.refused && chain.Active(),
              "Resize while a proxy is held: INVALID_CALL, nothing changed");
        DXGI_SWAP_CHAIN_DESC realDesc {};
        real->GetDesc(&realDesc);
        Check(realDesc.BufferDesc.Width == 96 && realDesc.BufferDesc.Height == 48 && realDesc.BufferCount == 4,
              "the real chain was not resized");
        Check(Frame(g, chain, real.Get(), 0.5f), "and the chain still converts");
        held.Reset();
        result = chain.Resize(g.device.Get(), real.Get(), request);
        Check(SUCCEEDED(result.hr) && result.proxied, "Resize after the proxy is let go works");

        // to unproxied, and back
        request.wantProxy = false;
        request.rawFormat = DXGI_FORMAT_UNKNOWN; // the game said "keep": the resolved format is used
        result = chain.Resize(g.device.Get(), real.Get(), request);
        real->GetDesc(&realDesc);
        Check(SUCCEEDED(result.hr) && !result.proxied && !chain.Active() && !SdrToHdr::AnyProxied(),
              "Resize to unproxied: no proxies, nothing registered");
        Check(realDesc.BufferDesc.Format == kGame, "the real chain is back to the game's format");
        {
            ComPtr<ID3D12Resource> viaGame, own;
            real->GetBuffer(0, IID_PPV_ARGS(&own));
            SdrToHdr::GameBuffer(real.Get(), 0, IID_PPV_ARGS(&viaGame));
            Check(viaGame.Get() == own.Get(), "GameBuffer passes through again");
        }
        request.wantProxy = true;
        request.rawFormat = kGame;
        result = chain.Resize(g.device.Get(), real.Get(), request);
        real->GetDesc(&realDesc);
        Check(SUCCEEDED(result.hr) && result.proxied && realDesc.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
              "Resize to proxied again: the real chain is FP16");
        Check(Frame(g, chain, real.Get(), 0.6f), "a frame converts again");

        // a proxy that cannot be made after the real resize: an unsupported game format
        request.resolvedFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
        result = chain.Resize(g.device.Get(), real.Get(), request);
        real->GetDesc(&realDesc);
        Check(result.reverted && !result.proxied && !chain.Active() && realDesc.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM,
              "a proxy that cannot be made: the chain is put back to the game's format, unproxied");
        request.resolvedFormat = request.rawFormat = kGame;
        result = chain.Resize(g.device.Get(), real.Get(), request);
        Check(SUCCEEDED(result.hr) && result.proxied, "and the next resize makes the proxy again");

        // full screen
        ShowWindow(window, SW_SHOWNOACTIVATE);
        const HRESULT full = real->SetFullscreenState(TRUE, nullptr);
        std::printf("SetFullscreenState(TRUE) -> %08lX%s\n", (unsigned long) full,
                    FAILED(full) ? " (refused by WARP or this session: the full screen cases are skipped)" : "");

        if (SUCCEEDED(full))
        {
            request.width = request.height = 0;
            result = chain.Resize(g.device.Get(), real.Get(), request);
            Check(SUCCEEDED(result.hr) && result.proxied, "full screen: Resize stays proxied");
            Check(Frame(g, chain, real.Get(), 0.5f), "full screen: a frame converts");
            const HRESULT windowed = real->SetFullscreenState(FALSE, nullptr);
            Check(SUCCEEDED(windowed), "SetFullscreenState(FALSE)");
            result = chain.Resize(g.device.Get(), real.Get(), request);
            Check(SUCCEEDED(result.hr) && result.proxied, "windowed again: Resize stays proxied");
            Check(Frame(g, chain, real.Get(), 0.5f), "windowed again: a frame converts");
        }

        chain.Release();
        Check(!chain.Active() && !SdrToHdr::AnyProxied(), "Release: inactive, nothing registered");

        real.Reset(); // one swap chain per window

        // HDR10: the pass writes PQ because the real chain is 10 bit, whatever the caller's constants say
        {
            auto pq = MakeChain(g, window, DXGI_FORMAT_R10G10B10A2_UNORM, 2, 64, 32);
            SdrToHdr::ProxyChain pqChain;
            const char* why10 = pq == nullptr ? "the 10 bit chain could not be made" : pqChain.Start(g.device.Get(), pq.Get(), kGame, true);
            const bool started10 = why10 == nullptr;
            Check(started10, "HDR10: Start on a 10 bit chain");

            if (!started10)
                std::printf("      %s (WARP may not take the PQ colour space)\n", why10);

            if (started10)
            {
                const UINT index = pq->GetCurrentBackBufferIndex();
                ComPtr<ID3D12Resource> proxy, realBuffer;
                pqChain.GetBuffer(index, IID_PPV_ARGS(&proxy));
                pq->GetBuffer(index, IID_PPV_ARGS(&realBuffer));
                Paint(g, proxy.Get(), 128.0f / 255.0f);
                const char* why = pqChain.Present(g.queue.Get(), pq.Get(), Constants()); // Constants() says scRGB
                g.Wait();
                const uint32_t code = ReadFirstPixel10(g, realBuffer.Get());
                const double nits = std::pow(128.0 / 255.0, 2.2) * 203.0;
                const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0, c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0,
                             c3 = 2392.0 / 128.0;
                const double p = std::pow(nits / 10000.0, m1);
                const double expected = std::pow((c1 + c2 * p) / (1.0 + c3 * p), m2) * 1023.0;
                char what[128];
                std::snprintf(what, sizeof(what), "HDR10: the red code is %u, PQ of the grey is %.1f", code, expected);
                Check(why == nullptr && std::fabs((double) code - expected) <= 1.0, what);
                proxy.Reset();
                realBuffer.Reset();
                pqChain.Release();
            }
        }
        g.Wait();
    }

    g.Wait();
    DestroyWindow(window);
    g.Wait();
    const ULONG after = RefCount(g.device.Get());
    char what[160];
    std::snprintf(what, sizeof(what), "device reference count after everything is released: %lu before, %lu after", before,
                  after);
    Check(after == before, what);

    const int messages = DebugMessages(g);
    char debug[96];
    std::snprintf(debug, sizeof(debug), "the debug layer reported %d errors or warnings", messages);
    Check(messages == 0, debug);

    std::printf("%s (%d failed)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
