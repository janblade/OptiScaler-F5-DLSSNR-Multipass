// Not built with the precompiled header: DXGI only, so the swap chain tests compile it alone.
#include "SdrToHdr_ProxyBase.h"

namespace SdrToHdr
{
namespace
{
// The chains that have a proxy, by the IUnknown of the real chain: what GameBuffer looks through.
std::mutex g_registryLock;
std::vector<std::pair<IUnknown*, ProxyChainBase*>> g_registry;
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

void Register(IUnknown* identity, ProxyChainBase* chain)
{
    std::lock_guard lock(g_registryLock);
    g_registry.emplace_back(identity, chain);
    g_registered = (int) g_registry.size();
}

void Unregister(ProxyChainBase* chain)
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

bool IsSrgbFormat(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
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
        ProxyChainBase* chain = nullptr;

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

bool ShouldWarnSrgbViewOnTypedProxy(const void* resource, DXGI_FORMAT viewFormat)
{
    if (!AnyProxied() || !IsSrgbFormat(viewFormat) || g_warnedSrgbView.load(std::memory_order_relaxed))
        return false;

    std::vector<ProxyChainBase*> chains;

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

void ProxyChainBase::ReleaseLocked()
{
    _active = false;

    if (_identity != nullptr)
    {
        Unregister(this);
        _identity = nullptr;
    }

    ReleaseResourcesLocked();
}

void ProxyChainBase::Release()
{
    std::lock_guard<std::recursive_mutex> lock(_lock);
    ReleaseLocked();
    _gameFormat = DXGI_FORMAT_UNKNOWN;
    _reportFormat = DXGI_FORMAT_UNKNOWN;
}

const char* ProxyChainBase::BuildLocked(IUnknown* device, IDXGISwapChain3* real, DXGI_FORMAT gameFormat)
{
    ReleaseLocked();
    _gameFormat = gameFormat;

    if (Hooks().failBuild)
    {
        Hooks().failBuild = false;
        return "the HDR proxy build failed (injected)";
    }

    if (device == nullptr || real == nullptr)
        return "the HDR proxy was given no device or swap chain";

    if (!IsSupportedGameFormat(gameFormat))
        return "the HDR proxy cannot stand in for this back buffer format";

    if (const char* why = BuildResourcesLocked(device, real, gameFormat))
    {
        ReleaseLocked();
        return why;
    }

    _identity = IdentityOf(real);

    if (_identity != nullptr)
        Register(_identity, this);

    _reportFormat = gameFormat;
    _active = true;
    return nullptr;
}

const char* ProxyChainBase::Start(IUnknown* device, IDXGISwapChain3* real, DXGI_FORMAT gameFormat, bool hdr10)
{
    std::lock_guard<std::recursive_mutex> lock(_lock);

    if (real == nullptr)
        return "the HDR proxy was given no swap chain";

    if (FAILED(SetColourSpace(real, HdrColourSpace(hdr10))))
        return "the swap chain does not take the HDR colour space";

    const char* why = BuildLocked(device, real, gameFormat);

    if (why != nullptr)
        _reportFormat = DXGI_FORMAT_UNKNOWN;

    return why;
}

HRESULT ProxyChainBase::Revert(IDXGISwapChain3* real, DXGI_FORMAT gameFormat, UINT flags,
                               DXGI_COLOR_SPACE_TYPE gameColourSpace)
{
    std::lock_guard<std::recursive_mutex> lock(_lock);
    ReleaseLocked();
    _reportFormat = DXGI_FORMAT_UNKNOWN;

    if (real == nullptr)
        return E_POINTER;

    if (Hooks().failRevert)
        return E_FAIL;

    // Count, size and buffer flags stay as they are; only the format goes back
    const HRESULT hr = real->ResizeBuffers(0, 0, 0, gameFormat, flags);

    if (SUCCEEDED(hr))
        real->SetColorSpace1(gameColourSpace);

    return hr;
}

bool ProxyChainBase::HeldByGame()
{
    std::lock_guard<std::recursive_mutex> lock(_lock);
    return HeldResourcesLocked();
}

// Puts the real chain back to the game's format with the arguments of the game's own call (ResizeBuffers1 keeps its node
// masks and queues). `realResized`: the real resize went through, so the size and count it asked for are the chain's;
// otherwise the chain still has its old ones and only the format goes back.
HRESULT ProxyChainBase::RevertRealLocked(IDXGISwapChain3* real, const ResizeRequest& request, DXGI_FORMAT format,
                                         bool realResized)
{
    if (Hooks().failRevert)
        return E_FAIL;

    // The real resize failed: the chain keeps its old size, count and format (as DXGI leaves them), only the format goes
    // back to the game's previous one. Plain ResizeBuffers: the game's node mask / queue arrays are sized for the count it
    // asked for, not the one the chain still has (and a proxied chain has one node and its creation queue only, see
    // Reason::MultiNode / OtherPresentQueue)
    if (!realResized)
    {
        const HRESULT hr = real->ResizeBuffers(0, 0, 0, format, request.flags);

        if (SUCCEEDED(hr))
            real->SetColorSpace1(request.gameColourSpace);

        return hr;
    }

    const HRESULT hr = request.useV1 ? real->ResizeBuffers1(request.count, request.width, request.height, format,
                                                             request.flags, request.nodeMask, request.queues)
                                     : real->ResizeBuffers(request.count, request.width, request.height, format,
                                                           request.flags);

    if (SUCCEEDED(hr))
        real->SetColorSpace1(request.gameColourSpace);

    return hr;
}

unsigned long ProxyChainBase::CurrentThread() { return (unsigned long) GetCurrentThreadId(); }

namespace
{
// The chain is in a resize from construction to destruction, on this thread
class ResizeFlag
{
  public:
    ResizeFlag(std::atomic<bool>& flag, std::atomic<unsigned long>& thread) : _flag(flag), _thread(thread)
    {
        _thread.store((unsigned long) GetCurrentThreadId(), std::memory_order_release);
        _flag.store(true, std::memory_order_release);
    }

    ~ResizeFlag()
    {
        _flag.store(false, std::memory_order_release);
        _thread.store(0, std::memory_order_release);
    }

    ResizeFlag(const ResizeFlag&) = delete;
    ResizeFlag& operator=(const ResizeFlag&) = delete;

  private:
    std::atomic<bool>& _flag;
    std::atomic<unsigned long>& _thread;
};
} // namespace

ResizeResult ProxyChainBase::Resize(IUnknown* device, IDXGISwapChain3* real, const ResizeRequest& request)
{
    ResizeResult result;

    // A resize inside a resize on this thread (a window message the game answers with its own ResizeBuffers) is refused, as
    // DXGI refuses it. A resize from another thread waits for the one running, as DXGI serialises them.
    if (InResizeOnThisThread())
    {
        result.hr = DXGI_ERROR_INVALID_CALL;
        result.refused = true;
        result.proxied = Active();
        result.note = "a resize is already running";
        return result;
    }

    std::lock_guard<std::mutex> gate(_resizeGate);
    ResizeFlag inResize(_inResize, _resizeThread);

    bool wasProxied = false;
    DXGI_FORMAT previousFormat = DXGI_FORMAT_UNKNOWN; // the game's format before this resize: what a failed one leaves

    {
        std::lock_guard<std::recursive_mutex> lock(_lock);
        wasProxied = ResourcesLocked();
        previousFormat = _gameFormat;

        if (real == nullptr)
        {
            result.hr = E_POINTER;
            result.proxied = wasProxied;
            return result;
        }

        if (wasProxied)
        {
            // DXGI refuses a resize while the game holds a back buffer; the proxies are the game's back buffers
            QuiesceLocked();

            if (HeldResourcesLocked())
            {
                result.hr = DXGI_ERROR_INVALID_CALL;
                result.proxied = true;
                result.refused = true;
                result.note = "the game still holds a back buffer";
                return result;
            }
        }

        // From here until the end the game is told the format it will have after, and GetBuffer says INVALID_CALL
        if (wasProxied || request.wantProxy)
            _reportFormat = request.resolvedFormat;

        ReleaseLocked();
    }

    const DXGI_FORMAT realFormat = request.wantProxy ? RealFormat(request.hdr10)
                                   : wasProxied     ? request.resolvedFormat
                                                    : request.rawFormat;

    // The real DXGI call, lock NOT held: it can send the window a message the game answers with GetBuffer / GetDesc
    if (Hooks().duringRealResize != nullptr)
        Hooks().duringRealResize(Hooks().context);

    const HRESULT hr = request.useV1 ? real->ResizeBuffers1(request.count, request.width, request.height, realFormat,
                                                             request.flags, request.nodeMask, request.queues)
                                     : real->ResizeBuffers(request.count, request.width, request.height, realFormat,
                                                           request.flags);
    result.hr = hr;

    std::lock_guard<std::recursive_mutex> lock(_lock);

    const auto finish = [&]()
    {
        if (!result.proxied)
            _reportFormat = DXGI_FORMAT_UNKNOWN;

        return result;
    };

    if (FAILED(hr))
    {
        // The chain is as it was, in the format it had: give the game its back buffers again (DXGI keeps the old format
        // when a resize fails, so the game's format is the previous one, not the one it asked for)
        if (wasProxied)
        {
            const char* why = BuildLocked(device, real, previousFormat);

            if (why == nullptr)
            {
                result.proxied = true;
            }
            else
            {
                // The real chain is still HDR and there is no proxy: put it back to the game's format
                result.note = why;
                const HRESULT revert = RevertRealLocked(real, request, previousFormat, false);

                if (SUCCEEDED(revert))
                    result.reverted = true;
                else
                    result.stuckHdr = true; // honest: it stays HDR, nothing converts it (known limit: for good)
            }
        }

        return finish();
    }

    if (request.wantProxy)
    {
        const HRESULT spaceResult = SetColourSpace(real, HdrColourSpace(request.hdr10));
        const char* why = FAILED(spaceResult) ? "the swap chain does not take the HDR colour space" : nullptr;

        if (why == nullptr)
            why = BuildLocked(device, real, request.resolvedFormat);

        if (why != nullptr)
        {
            // Never leave the game with an HDR chain and no pass: back to its own format, unproxied
            ReleaseLocked();
            const HRESULT revert = RevertRealLocked(real, request, request.resolvedFormat, true);
            result.hr = revert;
            result.proxied = false;
            result.note = why;

            if (SUCCEEDED(revert))
                result.reverted = true;
            else
                result.stuckHdr = true;

            return finish();
        }

        result.proxied = true;
        return finish();
    }

    if (wasProxied)
        real->SetColorSpace1(request.gameColourSpace);

    return finish();
}

HRESULT ProxyChainBase::GetBuffer(UINT index, REFIID riid, void** surface)
{
    if (surface == nullptr)
        return E_POINTER;

    // Inside a resize the game has no back buffers (DXGI says the same of its own)
    if (_inResize.load(std::memory_order_acquire))
    {
        *surface = nullptr;
        return DXGI_ERROR_INVALID_CALL;
    }

    std::lock_guard<std::recursive_mutex> lock(_lock);
    return GetBufferLocked(index, riid, surface);
}
} // namespace SdrToHdr
