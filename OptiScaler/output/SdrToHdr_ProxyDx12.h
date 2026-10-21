#pragma once

// Direct3D 12 and DXGI only (no OptiScaler headers but SdrToHdr_Common.h, SdrToHdr_Dx12.h and
// native/F5LowShaderBytecode.h), so tests/sdr_to_hdr_swapchain_dx12_gpu.cpp builds it alone.
#include "SdrToHdr_Common.h"
#include "SdrToHdr_Dx12.h"

#include <d3d12.h>
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
// One per wrapped swap chain, only while HDR output is active on it. The game gets the proxies from GetBuffer, in its own
// format, size and count; at present the pass converts proxy[current] -> real[current]. Every method takes the lock:
// GetBuffer can come from any thread.
class ProxyChain
{
  public:
    ProxyChain() = default;
    ~ProxyChain() { Release(); }
    ProxyChain(const ProxyChain&) = delete;
    ProxyChain& operator=(const ProxyChain&) = delete;

    // The real chain is already HDR (made so, or resized): sets the colour space on it and makes the proxies and the pass
    // for its current size and count. Returns nullptr when active, otherwise why not (the chain is then not touched
    // further: the caller reverts).
    const char* Start(ID3D12Device* device, IDXGISwapChain3* real, DXGI_FORMAT gameFormat, bool hdr10);

    // The game's ResizeBuffers / ResizeBuffers1 (see ResizeResult). Works from a proxied or an unproxied chain to either.
    ResizeResult Resize(ID3D12Device* device, IDXGISwapChain3* real, const ResizeRequest& request);

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
    bool Engaged() const { return _reportFormat.load(std::memory_order_acquire) != DXGI_FORMAT_UNKNOWN; }
    DXGI_FORMAT ReportedFormat() const { return _reportFormat.load(std::memory_order_acquire); }
    DXGI_FORMAT GameFormat() const { return ReportedFormat(); }

    // True the first time it is asked, once per chain: the present's failure is logged once.
    bool NoteFailureLogged() { return !_failureLogged.exchange(true); }

    // Test hooks (tests/sdr_to_hdr_swapchain_dx12_gpu.cpp): fault injection and a callback run where the real DXGI resize
    // would be, with the lock released and the in-resize state set, to stand in for a window message the game answers.
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

    // The game's GetBuffer. DXGI_ERROR_INVALID_CALL for an index past the count, as DXGI does.
    HRESULT GetBuffer(UINT index, REFIID riid, void** surface);

    // True while the game (or anything else) still holds a reference to a proxy.
    bool HeldByGame();

    // Converts the current proxy into the current real buffer on `queue`. nullptr when submitted, otherwise why not.
    const char* Present(ID3D12CommandQueue* queue, IDXGISwapChain3* real, PassConstants constants);

    // Whether `resource` is one of our proxies, and whether it is a typed (no relaxed casting) _SRGB-capable one.
    bool OwnsResource(const ID3D12Resource* resource, bool& relaxedCasting);

  private:
    const char* BuildLocked(ID3D12Device* device, IDXGISwapChain3* real, DXGI_FORMAT gameFormat);
    HRESULT RevertRealLocked(IDXGISwapChain3* real, const ResizeRequest& request, bool realResized);
    void ReleaseLocked();
    bool HeldLocked();

    // Recursive only because Start / Revert / Release call each other's locked parts; it is NOT held across the real DXGI
    // resize (a window message sent from inside it must not wait for it, and the lock-free reads above need none).
    mutable std::recursive_mutex _lock;
    std::atomic<bool> _active { false };
    std::atomic<bool> _inResize { false };
    std::atomic<DXGI_FORMAT> _reportFormat { DXGI_FORMAT_UNKNOWN };
    std::atomic<bool> _failureLogged { false };
    std::vector<ID3D12Resource*> _proxies;
    Dx12Pass _pass;
    DXGI_FORMAT _gameFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT _realFormat = DXGI_FORMAT_UNKNOWN; // the real chain's: scRGB (FP16) or PQ (10 bit) follows from it
    bool _relaxed = false;     // made with castable formats: GetDesc shows the game's format and the pass reads it raw
    bool _typedSrgb = false;   // a typed _SRGB proxy: read through an _SRGB view, the shader encodes it again
    IUnknown* _identity = nullptr; // the real chain's IUnknown, the registry key (not owned)
};

// For the code that reads "the game's picture" off a swap chain it was handed (the real one): the proxy when the chain has
// one, otherwise the chain's own buffer. Cheap when no chain is proxied.
HRESULT GameBuffer(IDXGISwapChain* realChain, UINT index, REFIID riid, void** surface);

// Whether any chain is proxied (a relaxed read, for hooks that run on every call).
bool AnyProxied();

// For a render target view the game made: whether `resource` is a typed proxy (no relaxed casting) getting an _SRGB view,
// which a swap chain's own buffer would allow and a typed resource may not. True once per process (detection only).
bool ShouldWarnSrgbViewOnTypedProxy(const ID3D12Resource* resource, DXGI_FORMAT viewFormat);
} // namespace SdrToHdr
