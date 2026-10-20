// Not built with the precompiled header: Direct3D and DXGI only, so tests/sdr_to_hdr_swapchain_dx12_gpu.cpp compiles it
// alone.
#include "SdrToHdr_ProxyDx12.h"

#include <atomic>

namespace SdrToHdr
{
namespace
{
// The chains that have a proxy, by the IUnknown of the real chain: what GameBuffer looks through.
std::mutex g_registryLock;
std::vector<std::pair<IUnknown*, ProxyChain*>> g_registry;
std::atomic<int> g_registered { 0 };
std::atomic<bool> g_warnedSrgbView { false };

IUnknown* IdentityOf(IUnknown* object)
{
    IUnknown* identity = nullptr;

    if (object == nullptr || FAILED(object->QueryInterface(IID_PPV_ARGS(&identity))))
        return nullptr;

    identity->Release(); // only the pointer is kept as a key: the chain owns itself
    return identity;
}

void Register(IUnknown* identity, ProxyChain* chain)
{
    std::lock_guard lock(g_registryLock);
    g_registry.emplace_back(identity, chain);
    g_registered = (int) g_registry.size();
}

void Unregister(ProxyChain* chain)
{
    std::lock_guard lock(g_registryLock);

    for (auto it = g_registry.begin(); it != g_registry.end(); ++it)
    {
        if (it->second == chain)
        {
            g_registry.erase(it);
            break;
        }
    }

    g_registered = (int) g_registry.size();
}

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

DXGI_COLOR_SPACE_TYPE HdrColourSpace(bool hdr10)
{
    return hdr10 ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
}

// SetColorSpace1, after asking the chain whether it can present that space
HRESULT SetColourSpace(IDXGISwapChain3* real, DXGI_COLOR_SPACE_TYPE space)
{
    UINT support = 0;
    HRESULT hr = real->CheckColorSpaceSupport(space, &support);

    if (FAILED(hr))
        return hr;

    if ((support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0)
        return DXGI_ERROR_UNSUPPORTED;

    return real->SetColorSpace1(space);
}
} // namespace

bool AnyProxied() { return g_registered.load(std::memory_order_relaxed) > 0; }

HRESULT GameBuffer(IDXGISwapChain* realChain, UINT index, REFIID riid, void** surface)
{
    if (realChain != nullptr && AnyProxied())
    {
        IUnknown* identity = IdentityOf(realChain);
        ProxyChain* chain = nullptr;

        {
            std::lock_guard lock(g_registryLock);

            for (auto& entry : g_registry)
            {
                if (entry.first == identity)
                {
                    chain = entry.second;
                    break;
                }
            }
        }

        // (a chain is unregistered by its own Release, which the owner runs before it frees the chain)
        if (chain != nullptr && chain->Active())
            return chain->GetBuffer(index, riid, surface);
    }

    return realChain->GetBuffer(index, riid, surface);
}

bool ShouldWarnSrgbViewOnTypedProxy(const ID3D12Resource* resource, DXGI_FORMAT viewFormat)
{
    if (!AnyProxied() || !IsSrgb(viewFormat) || g_warnedSrgbView.load(std::memory_order_relaxed))
        return false;

    std::vector<ProxyChain*> chains;

    {
        std::lock_guard lock(g_registryLock);

        for (auto& entry : g_registry)
            chains.push_back(entry.second);
    }

    for (auto* chain : chains)
    {
        bool relaxed = true;

        if (chain->OwnsResource(resource, relaxed) && !relaxed)
            return !g_warnedSrgbView.exchange(true);
    }

    return false;
}

bool ProxyChain::Active() const
{
    std::lock_guard<std::recursive_mutex> lock(_lock);
    return !_proxies.empty() && _pass.Ready();
}

DXGI_FORMAT ProxyChain::GameFormat() const
{
    std::lock_guard<std::recursive_mutex> lock(_lock);
    return _gameFormat;
}

bool ProxyChain::OwnsResource(const ID3D12Resource* resource, bool& relaxedCasting)
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

void ProxyChain::ReleaseLocked()
{
    if (_identity != nullptr)
    {
        Unregister(this);
        _identity = nullptr;
    }

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

void ProxyChain::Release()
{
    std::lock_guard<std::recursive_mutex> lock(_lock);
    ReleaseLocked();
    _gameFormat = DXGI_FORMAT_UNKNOWN;
}

const char* ProxyChain::BuildLocked(ID3D12Device* device, IDXGISwapChain3* real, DXGI_FORMAT gameFormat)
{
    ReleaseLocked();
    _gameFormat = gameFormat;

    if (device == nullptr || real == nullptr)
        return "the HDR proxy was given no device or swap chain";

    if (!IsSupportedGameFormat(gameFormat))
        return "the HDR proxy cannot stand in for this back buffer format";

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
    {
        ReleaseLocked();
        return "the HDR proxy back buffers could not be made";
    }

    _relaxed = relaxed;
    _realFormat = desc.BufferDesc.Format;
    _typedSrgb = !relaxed && IsSrgb(gameFormat);

    if (const char* why = _pass.Init(device, desc.BufferDesc.Format, desc.BufferCount))
    {
        ReleaseLocked();
        return why;
    }

    _identity = IdentityOf(real);

    if (_identity != nullptr)
        Register(_identity, this);

    return nullptr;
}

const char* ProxyChain::Start(ID3D12Device* device, IDXGISwapChain3* real, DXGI_FORMAT gameFormat, bool hdr10)
{
    std::lock_guard<std::recursive_mutex> lock(_lock);

    if (real == nullptr)
        return "the HDR proxy was given no swap chain";

    if (FAILED(SetColourSpace(real, HdrColourSpace(hdr10))))
        return "the swap chain does not take the HDR colour space";

    return BuildLocked(device, real, gameFormat);
}

HRESULT ProxyChain::Revert(IDXGISwapChain3* real, DXGI_FORMAT gameFormat, UINT flags,
                           DXGI_COLOR_SPACE_TYPE gameColourSpace)
{
    std::lock_guard<std::recursive_mutex> lock(_lock);
    ReleaseLocked();

    if (real == nullptr)
        return E_POINTER;

    // Count, size and buffer flags stay as they are; only the format goes back
    const HRESULT hr = real->ResizeBuffers(0, 0, 0, gameFormat, flags);

    if (SUCCEEDED(hr))
        real->SetColorSpace1(gameColourSpace);

    return hr;
}

bool ProxyChain::HeldLocked()
{
    for (auto* proxy : _proxies)
    {
        proxy->AddRef();

        if (proxy->Release() > 1)
            return true;
    }

    return false;
}

bool ProxyChain::HeldByGame()
{
    std::lock_guard<std::recursive_mutex> lock(_lock);
    return HeldLocked();
}

ResizeResult ProxyChain::Resize(ID3D12Device* device, IDXGISwapChain3* real, const ResizeRequest& request)
{
    std::lock_guard<std::recursive_mutex> lock(_lock);
    ResizeResult result;
    const bool wasProxied = !_proxies.empty() && _pass.Ready();

    if (real == nullptr)
    {
        result.hr = E_POINTER;
        result.proxied = wasProxied;
        return result;
    }

    if (wasProxied)
    {
        // DXGI refuses a resize while the game holds a back buffer; the proxies are the game's back buffers
        _pass.WaitForIdle();

        if (HeldLocked())
        {
            result.hr = DXGI_ERROR_INVALID_CALL;
            result.proxied = true;
            result.refused = true;
            result.note = "the game still holds a back buffer";
            return result;
        }
    }

    ReleaseLocked();

    const DXGI_FORMAT realFormat = request.wantProxy ? RealFormat(request.hdr10)
                                   : wasProxied     ? request.resolvedFormat
                                                    : request.rawFormat;

    HRESULT hr = request.useV1 ? real->ResizeBuffers1(request.count, request.width, request.height, realFormat,
                                                      request.flags, request.nodeMask, request.queues)
                               : real->ResizeBuffers(request.count, request.width, request.height, realFormat,
                                                     request.flags);
    result.hr = hr;

    if (FAILED(hr))
    {
        // The chain is as it was: give the game its back buffers again
        if (wasProxied)
        {
            const char* why = BuildLocked(device, real, request.resolvedFormat);

            if (why != nullptr)
                result.note = why;
            else
                result.proxied = true;
        }

        return result;
    }

    if (request.wantProxy)
    {
        HRESULT spaceResult = SetColourSpace(real, HdrColourSpace(request.hdr10));
        const char* why = FAILED(spaceResult) ? "the swap chain does not take the HDR colour space" : nullptr;

        if (why == nullptr)
            why = BuildLocked(device, real, request.resolvedFormat);

        if (why != nullptr)
        {
            // Never leave the game with an HDR chain and no pass: back to its own format, unproxied
            ReleaseLocked();
            result.hr = real->ResizeBuffers(0, 0, 0, request.resolvedFormat, request.flags);

            if (SUCCEEDED(result.hr))
                real->SetColorSpace1(request.gameColourSpace);

            result.proxied = false;
            result.reverted = true;
            result.note = why;
            return result;
        }

        result.proxied = true;
        return result;
    }

    if (wasProxied)
        real->SetColorSpace1(request.gameColourSpace);

    return result;
}

HRESULT ProxyChain::GetBuffer(UINT index, REFIID riid, void** surface)
{
    std::lock_guard<std::recursive_mutex> lock(_lock);

    if (surface == nullptr)
        return E_POINTER;

    if (index >= _proxies.size())
    {
        *surface = nullptr;
        return DXGI_ERROR_INVALID_CALL;
    }

    return _proxies[index]->QueryInterface(riid, surface);
}

const char* ProxyChain::Present(ID3D12CommandQueue* queue, IDXGISwapChain3* real, PassConstants constants)
{
    std::lock_guard<std::recursive_mutex> lock(_lock);

    if (_proxies.empty() || !_pass.Ready())
        return "the HDR proxy is not set up";

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
