#pragma once

// DXGI only (no Direct3D 11 or 12 here, no OptiScaler headers): the part of the proxy back buffers that is the same for
// Direct3D 12 (SdrToHdr_ProxyDx12.h) and Direct3D 11 (SdrToHdr_ProxyDx11.h) -- the in-resize state, the refusal while the game
// holds a proxy, the revert when the proxy cannot be made, and the registry the consumers look the game's buffer up in.
// Builds alone, so the tests compile it with either.
#include "SdrToHdr_Common.h"
#include "SdrToHdr_Pass.h"

#include <dxgi1_4.h>

#include <atomic>
#include <mutex>
#include <vector>

namespace SdrToHdr
{
// What a Resize call is asked to do: the game's ResizeBuffers / ResizeBuffers1 arguments, with the decision made.
struct ResizeRequest
{
    bool useV1 = false;
    UINT count = 0; // 0 keeps the count
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT rawFormat = DXGI_FORMAT_UNKNOWN;      // as the game passed it (UNKNOWN keeps the format)
    DXGI_FORMAT resolvedFormat = DXGI_FORMAT_UNKNOWN; // the game's format: UNKNOWN resolved to the one it has
    UINT flags = 0;
    const UINT* nodeMask = nullptr;     // ResizeBuffers1 only
    IUnknown* const* queues = nullptr;  // ResizeBuffers1 only
    bool wantProxy = false;             // Decide() said use, for the new state
    bool hdr10 = false;                 // PQ instead of scRGB
    // The chain was made flip model for HDR output (an older, bitblt one): a failure never puts the real chain back to a plain
    // flip back buffer for the game (it is unbound at every Present, which a bitblt-era game does not expect). It stays
    // engaged -- GetBuffer refuses -- and the result says stuckHdr: restart-required.
    bool noRevert = false;
    DXGI_COLOR_SPACE_TYPE gameColourSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709; // set again when leaving the proxy
};

struct ResizeResult
{
    HRESULT hr = S_OK;          // the real resize's, or the revert's after a failure
    bool proxied = false;       // the state after
    bool refused = false;       // INVALID_CALL because the game still holds a proxy: nothing was changed
    bool reverted = false;      // the proxy could not be made after the real resize; the chain is back to the game's format
    bool stuckHdr = false;      // the proxy could not be made AND the chain could not be put back: it stays HDR with no pass
    const char* note = nullptr; // a fixed string saying why, for the log
};

// The back buffers a game is shown in place of an HDR swap chain's, and the pass that turns them into the real ones.
//
// One per wrapped swap chain, once it is first proxied. The game gets the proxies from GetBuffer, in its own format and size;
// at present the pass converts the current proxy into the current real buffer. The derived class owns the resources
// (Direct3D 12: a proxy per buffer and a command list; Direct3D 11: one proxy, the only buffer a flip chain shows).
class ProxyChainBase
{
  public:
    virtual ~ProxyChainBase() = default;
    ProxyChainBase(const ProxyChainBase&) = delete;
    ProxyChainBase& operator=(const ProxyChainBase&) = delete;

    // The real chain is already HDR (made so, or resized): sets the colour space on it and makes the proxies and the pass
    // for its current size and count. `device` is the Direct3D 12 device or the Direct3D 11 device. Returns nullptr when
    // active, otherwise why not (the caller reverts).
    const char* Start(IUnknown* device, IDXGISwapChain3* real, DXGI_FORMAT gameFormat, bool hdr10);

    // The game's ResizeBuffers / ResizeBuffers1 (see ResizeResult). Works from a proxied or an unproxied chain to either.
    //
    // Known limits (documented, not fixed): after ResizeResult::stuckHdr (the proxy could not be rebuilt and the chain could not
    // be put back) the real chain stays HDR with nothing converting it for good -- only a restart helps, and the status says
    // so. And while a resize runs the lock is not held, so a CheckColorSpaceSupport / SetColorSpace1 the game makes meanwhile
    // reaches the real chain for that moment (the wrapper answers for the game's format only while the chain is Active).
    // Also: a resize from another thread waits on _resizeGate for the running one (DXGI serialises them too); Direct3D 11
    // views of the proxy do not hold a resize (DXGI would refuse while a view of its own buffer lives); other rare flags a
    // flip chain refuses fail the create, which the creation sites retry as the game asked; and GetDesc's Flags do not show
    // the GDI-compatible flag the resize drops (SdrToHdr::UpgradedResizeFlags).
    ResizeResult Resize(IUnknown* device, IDXGISwapChain3* real, const ResizeRequest& request);

    // Puts the real chain back to the game's format and colour space without a proxy (a failed Start, a dropped plan).
    HRESULT Revert(IDXGISwapChain3* real, DXGI_FORMAT gameFormat, UINT flags, DXGI_COLOR_SPACE_TYPE gameColourSpace);

    // Drops the proxies and the pass (waiting for the GPU). The real chain is not touched.
    void Release();

    // Lock-free reads (GetDesc, GetBuffer and the like run on any thread, and DXGI can call them from inside a resize):
    //  Active   the proxies and the pass exist
    //  InResize a Resize is between its first step and its last (the real DXGI call runs with the lock NOT held)
    //  Engaged  the game is being shown a proxy chain: Active, or in a resize that comes from / goes to one
    //  ReportedFormat the format GetDesc / GetDesc1 must report while Engaged (the target's during a resize)
    bool Active() const { return _active.load(std::memory_order_acquire); }
    bool InResize() const { return _inResize.load(std::memory_order_acquire); }
    // A resize that runs on THIS thread: the only re-entrancy DXGI refuses (a window message sent from inside it, answered by
    // the game's own ResizeBuffers). A resize from another thread waits its turn instead (see Resize).
    bool InResizeOnThisThread() const
    {
        return _inResize.load(std::memory_order_acquire) && _resizeThread.load(std::memory_order_acquire) == CurrentThread();
    }
    bool Engaged() const { return _reportFormat.load(std::memory_order_acquire) != DXGI_FORMAT_UNKNOWN; }
    DXGI_FORMAT ReportedFormat() const { return _reportFormat.load(std::memory_order_acquire); }
    DXGI_FORMAT GameFormat() const { return ReportedFormat(); }

    // After a failure on an upgraded chain: the game keeps being shown a (broken) proxy chain, never the real HDR buffer.
    void HoldEngaged(DXGI_FORMAT gameFormat) { _reportFormat.store(gameFormat, std::memory_order_release); }

    // True the first time it is asked, once per chain: the present's failure is logged once.
    bool NoteFailureLogged() { return !_failureLogged.exchange(true); }

    // True the first time it is asked, once per chain: a late stand-down (the game turned on its own DLSS-G) is logged once.
    bool NoteLateStandDownLogged() { return !_lateLogged.exchange(true); }

    // Test hooks (tests/sdr_to_hdr_swapchain_dx12_gpu.cpp and tests/sdr_to_hdr_dx11_gpu.cpp): fault injection and a callback run where the real DXGI resize would
    // be, with the lock released and the in-resize state set, to stand in for a window message the game answers.
    struct TestHooks
    {
        bool failBuild = false;  // the next proxy build fails
        bool failRevert = false; // every revert fails
        void (*duringRealResize)(void*) = nullptr;
        void* context = nullptr;
    };
    static TestHooks& Hooks()
    {
        static TestHooks hooks;
        return hooks;
    }

    // The game's GetBuffer. DXGI_ERROR_INVALID_CALL for an index past the buffers shown, and while a resize runs.
    HRESULT GetBuffer(UINT index, REFIID riid, void** surface);

    // True while the game (or anything else) still holds a reference to a proxy.
    bool HeldByGame();

    // Converts the current proxy into the current real buffer. `queueOrContext` is the Direct3D 12 command queue or the
    // Direct3D 11 immediate context. nullptr when done, otherwise why not.
    virtual const char* Present(IUnknown* queueOrContext, IDXGISwapChain3* real, PassConstants constants) = 0;

    // Whether `resource` is one of our proxies, and whether it can be viewed in the other of UNORM / _SRGB (Direct3D 12 with
    // relaxed format casting only).
    virtual bool OwnsResource(const void* resource, bool& relaxedCasting) = 0;

  protected:
    ProxyChainBase() = default;

    // Made and released by the derived class; the base calls them with its lock held.
    virtual const char* BuildResourcesLocked(IUnknown* device, IDXGISwapChain3* real, DXGI_FORMAT gameFormat) = 0;
    virtual void ReleaseResourcesLocked() = 0;
    virtual bool ResourcesLocked() const = 0;         // proxies and pass exist
    virtual bool HeldResourcesLocked() = 0;           // the game still holds a proxy
    virtual void QuiesceLocked() = 0;                 // nothing of ours in flight / bound, before the holders are counted
    virtual HRESULT GetBufferLocked(UINT index, REFIID riid, void** surface) = 0;

    mutable std::recursive_mutex _lock;
    DXGI_FORMAT _gameFormat = DXGI_FORMAT_UNKNOWN;

  private:
    const char* BuildLocked(IUnknown* device, IDXGISwapChain3* real, DXGI_FORMAT gameFormat);
    void ReleaseLocked();
    HRESULT RevertRealLocked(IDXGISwapChain3* real, const ResizeRequest& request, DXGI_FORMAT format, bool realResized);

    std::atomic<bool> _active { false };
    static unsigned long CurrentThread();

    std::atomic<bool> _inResize { false };
    std::atomic<unsigned long> _resizeThread { 0 };
    std::mutex _resizeGate; // one resize at a time; a second thread waits for the first
    std::atomic<bool> _lateLogged { false };
    std::atomic<DXGI_FORMAT> _reportFormat { DXGI_FORMAT_UNKNOWN };
    std::atomic<bool> _failureLogged { false };
    IUnknown* _identity = nullptr; // the real chain's IUnknown, the registry key (not owned)
};

// For the code that reads "the game's picture" off a swap chain it was handed (the real one): the proxy when the chain has
// one, otherwise the chain's own buffer. Cheap when no chain is proxied.
HRESULT GameBuffer(IDXGISwapChain* realChain, UINT index, REFIID riid, void** surface);

// Whether any chain is proxied (a relaxed read, for hooks that run on every call).
bool AnyProxied();

// For a render target view the game made: whether `resource` is a typed proxy (no relaxed casting) getting an _SRGB view,
// which a swap chain's own buffer would allow and a typed resource may not. True once per process (detection only).
bool ShouldWarnSrgbViewOnTypedProxy(const void* resource, DXGI_FORMAT viewFormat);
} // namespace SdrToHdr
