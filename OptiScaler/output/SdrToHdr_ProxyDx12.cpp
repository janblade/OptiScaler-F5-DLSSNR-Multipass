// Not built with the precompiled header: Direct3D and DXGI only, so tests/sdr_to_hdr_swapchain_dx12_gpu.cpp compiles it
// alone.
#include "SdrToHdr_ProxyDx12.h"

namespace SdrToHdr
{
namespace
{
bool IsSrgb(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

// The two formats a proxy of `format` can be viewed as with relaxed casting
void CastableOf(DXGI_FORMAT format, DXGI_FORMAT (&out)[2])
{
    const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    out[0] = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    out[1] = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}
} // namespace

bool ProxyChain::OwnsResource(const void* resource, bool& relaxedCasting)
{
    std::lock_guard<std::recursive_mutex> lock(_lock);

    for (auto* proxy : _proxies)
    {
        if (proxy == resource)
        {
            relaxedCasting = _relaxed;
            return true;
        }
    }

    return false;
}

void ProxyChain::ReleaseResourcesLocked()
{
    _pass.Release(); // waits for what it submitted

    for (auto*& proxy : _proxies)
    {
        if (proxy != nullptr)
            proxy->Release();
    }

    _proxies.clear();
    _relaxed = false;
    _typedSrgb = false;
}

bool ProxyChain::HeldResourcesLocked()
{
    for (auto* proxy : _proxies)
    {
        proxy->AddRef();

        if (proxy->Release() > 1)
            return true;
    }

    return false;
}

HRESULT ProxyChain::GetBufferLocked(UINT index, REFIID riid, void** surface)
{
    if (index >= _proxies.size())
    {
        *surface = nullptr;
        return DXGI_ERROR_INVALID_CALL;
    }

    return _proxies[index]->QueryInterface(riid, surface);
}

const char* ProxyChain::BuildResourcesLocked(IUnknown* deviceUnknown, IDXGISwapChain3* real, DXGI_FORMAT gameFormat)
{
    ID3D12Device* device = nullptr;

    if (FAILED(deviceUnknown->QueryInterface(IID_PPV_ARGS(&device))) || device == nullptr)
        return "the HDR proxy was given no Direct3D 12 device";

    device->Release(); // the caller's object keeps it alive

    DXGI_SWAP_CHAIN_DESC desc {};

    if (FAILED(real->GetDesc(&desc)) || desc.BufferCount == 0 || desc.BufferCount > 16 || desc.BufferDesc.Width == 0 ||
        desc.BufferDesc.Height == 0)
        return "the HDR proxy could not read the swap chain's size";

    // Relaxed format casting lets the game make _SRGB views of a UNORM proxy (or the reverse) and keeps GetDesc showing its
    // own format, as a swap chain's own buffers do
    ID3D12Device10* device10 = nullptr;
    bool relaxed = false;

    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device10))) && device10 != nullptr)
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options {};
        relaxed = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options, sizeof(options))) &&
                  options.RelaxedFormatCastingSupported;

        if (!relaxed)
        {
            device10->Release();
            device10 = nullptr;
        }
    }

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    if ((desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) != 0)
        flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_RESOURCE_DESC1 desc1 {};
    desc1.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc1.Width = desc.BufferDesc.Width;
    desc1.Height = desc.BufferDesc.Height;
    desc1.DepthOrArraySize = 1;
    desc1.MipLevels = 1;
    desc1.Format = gameFormat;
    desc1.SampleDesc.Count = 1;
    desc1.Flags = flags;

    D3D12_RESOURCE_DESC desc0 {};
    desc0.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc0.Width = desc1.Width;
    desc0.Height = desc1.Height;
    desc0.DepthOrArraySize = 1;
    desc0.MipLevels = 1;
    desc0.Format = gameFormat;
    desc0.SampleDesc.Count = 1;
    desc0.Flags = flags;

    DXGI_FORMAT castable[2] = {};
    CastableOf(gameFormat, castable);

    bool failed = false;

    for (UINT i = 0; i < desc.BufferCount && !failed; ++i)
    {
        ID3D12Resource* proxy = nullptr;
        HRESULT hr;

        if (relaxed)
            hr = device10->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc1, D3D12_BARRIER_LAYOUT_COMMON,
                                                    nullptr, nullptr, 2, castable, IID_PPV_ARGS(&proxy));
        else
            hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc0, D3D12_RESOURCE_STATE_COMMON,
                                                 nullptr, IID_PPV_ARGS(&proxy));

        if (FAILED(hr) || proxy == nullptr)
        {
            failed = true;
            break;
        }

        proxy->SetName(L"HDR output proxy back buffer");
        _proxies.push_back(proxy);
    }

    if (device10 != nullptr)
        device10->Release();

    if (failed)
        return "the HDR proxy back buffers could not be made";

    _relaxed = relaxed;
    _realFormat = desc.BufferDesc.Format;
    _typedSrgb = !relaxed && IsSrgb(gameFormat);

    return _pass.Init(device, desc.BufferDesc.Format, desc.BufferCount);
}

const char* ProxyChain::Present(IUnknown* queueOrContext, IDXGISwapChain3* real, PassConstants constants)
{
    std::lock_guard<std::recursive_mutex> lock(_lock);

    if (_proxies.empty() || !_pass.Ready())
        return "the HDR proxy is not set up";

    ID3D12CommandQueue* queue = nullptr;

    if (queueOrContext == nullptr || FAILED(queueOrContext->QueryInterface(IID_PPV_ARGS(&queue))) || queue == nullptr)
        return "the HDR pass was given no command queue";

    queue->Release(); // the caller's object keeps it alive

    const UINT index = real->GetCurrentBackBufferIndex();

    if (index >= _proxies.size())
        return "the HDR proxy has no buffer for this index";

    ID3D12Resource* realBuffer = nullptr;

    if (FAILED(real->GetBuffer(index, IID_PPV_ARGS(&realBuffer))) || realBuffer == nullptr)
        return "the HDR pass could not get the real back buffer";

    constants.srcLinear = _typedSrgb ? 1u : 0u;
    constants.mode = _realFormat == DXGI_FORMAT_R10G10B10A2_UNORM ? 1u : 0u; // follows the chain, not the live setting
    const char* why = _pass.Run(queue, index, _proxies[index], realBuffer, constants);
    realBuffer->Release(); // the pass holds no reference between frames: a resize must find none
    return why;
}
} // namespace SdrToHdr
