#include "pch.h"

#include "GenericDepth_Dx11.h"

#include <Config.h>
#include <Util.h>
#include <dlssnr/DlssNr_NativeMode.h>

#include <native/DepthCopyDx11.h>
#include <native/DepthFinderCore.h>
#include <native/NativeLowLatency.h>

#include <detours/detours.h>

#include <imgui/imgui.h>

#include <d3d11_4.h>

#include <atomic>
#include <mutex>

// See GenericDepth_Dx11.h. Reuses native::DepthFinderCore (resource_tracking/GenericDepth_Dx12.cpp is the D3D12 sibling of this
// file; both observers feed the same core).

namespace
{

typedef void(STDMETHODCALLTYPE* PFN_OMSetRenderTargets)(ID3D11DeviceContext* This, UINT NumViews,
                                                        ID3D11RenderTargetView* const* ppRenderTargetViews,
                                                        ID3D11DepthStencilView* pDepthStencilView);
typedef void(STDMETHODCALLTYPE* PFN_OMSetRenderTargetsAndUnorderedAccessViews)(
    ID3D11DeviceContext* This, UINT NumRTVs, ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView, UINT UAVStartSlot, UINT NumUAVs,
    ID3D11UnorderedAccessView* const* ppUnorderedAccessViews, const UINT* pUAVInitialCounts);
typedef void(STDMETHODCALLTYPE* PFN_ClearDepthStencilView)(ID3D11DeviceContext* This, ID3D11DepthStencilView* pView,
                                                           UINT ClearFlags, FLOAT Depth, UINT8 Stencil);
typedef void(STDMETHODCALLTYPE* PFN_RSSetViewports)(ID3D11DeviceContext* This, UINT NumViewports,
                                                    const D3D11_VIEWPORT* pViewports);
typedef void(STDMETHODCALLTYPE* PFN_Draw)(ID3D11DeviceContext* This, UINT VertexCount, UINT StartVertexLocation);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexed)(ID3D11DeviceContext* This, UINT IndexCount, UINT StartIndexLocation,
                                                 INT BaseVertexLocation);
typedef void(STDMETHODCALLTYPE* PFN_DrawInstanced)(ID3D11DeviceContext* This, UINT VertexCountPerInstance,
                                                   UINT InstanceCount, UINT StartVertexLocation,
                                                   UINT StartInstanceLocation);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexedInstanced)(ID3D11DeviceContext* This, UINT IndexCountPerInstance,
                                                          UINT InstanceCount, UINT StartIndexLocation,
                                                          INT BaseVertexLocation, UINT StartInstanceLocation);
typedef void(STDMETHODCALLTYPE* PFN_DrawAuto)(ID3D11DeviceContext* This);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndirect)(ID3D11DeviceContext* This, ID3D11Buffer* pBufferForArgs,
                                                  UINT AlignedByteOffsetForArgs);
typedef void(STDMETHODCALLTYPE* PFN_Dispatch)(ID3D11DeviceContext* This, UINT X, UINT Y, UINT Z);

PFN_OMSetRenderTargets o_OMSetRenderTargets = nullptr;
PFN_OMSetRenderTargetsAndUnorderedAccessViews o_OMSetRenderTargetsAndUAV = nullptr;
PFN_ClearDepthStencilView o_ClearDepthStencilView = nullptr;
PFN_RSSetViewports o_RSSetViewports = nullptr;
PFN_Draw o_Draw = nullptr;
PFN_DrawIndexed o_DrawIndexed = nullptr;
PFN_DrawInstanced o_DrawInstanced = nullptr;
PFN_DrawIndexedInstanced o_DrawIndexedInstanced = nullptr;
PFN_DrawAuto o_DrawAuto = nullptr;
PFN_DrawIndirect o_DrawIndexedInstancedIndirect = nullptr;
PFN_DrawIndirect o_DrawInstancedIndirect = nullptr;
PFN_Dispatch o_Dispatch = nullptr;

// ID3D11DeviceContext vtable indices (checked against the SDK header with offsetof; see tests/ -- a host-only sanity program,
// not shipped): DrawIndexed 12, Draw 13, DrawIndexedInstanced 20, DrawInstanced 21, OMSetRenderTargets 33,
// OMSetRenderTargetsAndUnorderedAccessViews 34, DrawAuto 38, DrawIndexedInstancedIndirect 39, DrawInstancedIndirect 40,
// Dispatch 41, RSSetViewports 44, ClearDepthStencilView 53.
constexpr int kDrawIndexed = 12, kDraw = 13, kDrawIndexedInstanced = 20, kDrawInstanced = 21, kOMSetRenderTargets = 33,
             kOMSetRenderTargetsAndUAV = 34, kDrawAuto = 38, kDrawIndexedInstancedIndirect = 39,
             kDrawInstancedIndirect = 40, kDispatch = 41, kRSSetViewports = 44, kClearDepthStencilView = 53;

std::mutex g_mutex;
native::DepthFinderCore g_core;

// The context the game draws its frames through: its device's immediate context, unwrapped from a wrapping layer's proxy
// (Streamline's), a reference held. The hooks patch the functions themselves, so every context of the same kind runs them (a
// deferred context under the debug layer, another device's immediate context): only this one's calls are counted. Followed to
// the device the game presents with (FollowPresentDevice), so a device the game makes again is watched too. Set at Install and
// on the present thread only.
ID3D11DeviceContext* g_context = nullptr;
std::atomic<uint64_t> g_contextId = 0; // g_context, as the core's id for it; read by the hooks on the game's threads
PVOID g_hookedOMSetRenderTargets = nullptr; // the function hooked, to tell whether a context runs the hooks at all
// The watched context set render targets since the last present (it is still the one the game draws through).
std::atomic<bool> g_watchedBound = false;

// The device of g_context, and the real device behind it when that is a proxy; a buffer of either can be copied on g_context.
// Not held (g_context holds its device). Under g_mutex.
ID3D11Device* g_contextDevice = nullptr;
ID3D11Device* g_contextRealDevice = nullptr;

// The copy of the picked buffer for the frame: one context means one copy, unlike the D3D12 finder's several command lists.
// Under g_mutex. Let go at stand-down and when the context changes. Never destroyed (no COM release at process exit).
native::DepthCopyDx11& g_copy = *new native::DepthCopyDx11();

// The finder's own pass is being recorded on this thread: its Dispatch is not the game's.
thread_local bool t_copying = false;

bool g_installed = false;
bool g_installFailed = false;

// Only the game's own context is watched (see g_context).
bool Watched(ID3D11DeviceContext* context)
{
    return (uint64_t) (size_t) context == g_contextId.load(std::memory_order_relaxed);
}

// Watches `context` (already unwrapped) from now on, and lets go of the copy made for the one before.
void UseContext(ID3D11DeviceContext* context)
{
    context->AddRef();

    if (g_context != nullptr)
        g_context->Release();

    g_context = context;

    std::lock_guard lock(g_mutex);

    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    g_contextDevice = device;
    g_contextRealDevice = device;

    IUnknown* real = nullptr;

    if (device != nullptr && Util::CheckForRealObject(__FUNCTION__, device, &real))
        g_contextRealDevice = (ID3D11Device*) real;

    if (device != nullptr)
        device->Release(); // the context holds it

    g_copy.Release();
    g_contextId.store((uint64_t) (size_t) context, std::memory_order_relaxed);
}

// The depth buffer a depth-stencil view points at, as a DepthBuffer (plain data for the core). `*outResource` gets an
// addref'd ID3D11Resource the caller must Release (the copy, if taken, is made from it).
bool Describe(ID3D11DepthStencilView* view, native::DepthBuffer* out, ID3D11Resource** outResource)
{
    if (view == nullptr)
        return false;

    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);

    if (resource == nullptr)
        return false;

    ID3D11Texture2D* tex = nullptr;

    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&tex))) || tex == nullptr)
    {
        resource->Release();
        return false;
    }

    D3D11_TEXTURE2D_DESC desc {};
    tex->GetDesc(&desc);
    tex->Release();

    out->id = (uint64_t) (size_t) resource;
    out->width = desc.Width;
    out->height = desc.Height;
    out->format = (uint32_t) desc.Format;
    out->readOnlyDepth = false; // D3D11 has no read-only depth bind distinct from the view's own flags; not used by the pick

    *outResource = resource; // caller releases
    return true;
}

// Copies `resource` (the picked buffer) into the frame's copy on `context` (g_context, from a hook or Present). A buffer that
// is not copied leaves no copy for the frame: the copy of an earlier one is not offered for it.
void TakeSnapshot(ID3D11DeviceContext* context, ID3D11Resource* resource, const char* where)
{
    std::lock_guard lock(g_mutex);

    static int calls = 0;

    if (calls < 8 || calls % 200 == 0)
        LOG_INFO("Depth finder (D3D11): TakeSnapshot call {} from {}, {:X}", calls, where, (size_t) resource);

    ++calls;

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    resource->GetDevice(&device);
    const char* failed = nullptr;

    if (device == nullptr || (device.Get() != g_contextDevice && device.Get() != g_contextRealDevice))
    {
        g_copy.Forget();
        failed = "the picked buffer is of another device than the context watched";
    }
    else
    {
        t_copying = true;
        failed = g_copy.Take(context, resource);
        t_copying = false;
    }

    static const char* logged = nullptr;

    if (failed != nullptr && failed != logged)
        LOG_WARN("Depth finder (D3D11): {}; no copy", failed);

    logged = failed;

    static bool loggedFirst = false;

    if (failed == nullptr && !loggedFirst)
    {
        loggedFirst = true;
        LOG_INFO("Depth finder (D3D11): first depth copy taken, {}x{}, {} R32_FLOAT", g_copy.Width(), g_copy.Height(),
                 g_copy.Converted() ? "converted to" : "copied straight into");
    }
}

void OnBound(ID3D11DeviceContext* context, ID3D11DepthStencilView* view)
{
    native::DepthBuffer buffer;
    ID3D11Resource* resource = nullptr;
    const bool hadDepth = view != nullptr;
    const bool have = hadDepth && Describe(view, &buffer, &resource);

    const auto request = g_core.OnDepthBound((uint64_t) (size_t) context, hadDepth, have ? &buffer : nullptr);

    if (request.take)
        TakeSnapshot(context, (ID3D11Resource*) (size_t) request.id, "unbind");

    if (resource != nullptr)
        resource->Release();
}

void STDMETHODCALLTYPE hkOMSetRenderTargets(ID3D11DeviceContext* This, UINT NumViews,
                                            ID3D11RenderTargetView* const* ppRenderTargetViews,
                                            ID3D11DepthStencilView* pDepthStencilView)
{
    if (Watched(This))
    {
        g_watchedBound.store(true, std::memory_order_relaxed);

        if (g_core.Active())
            OnBound(This, pDepthStencilView);
    }

    o_OMSetRenderTargets(This, NumViews, ppRenderTargetViews, pDepthStencilView);
}

void STDMETHODCALLTYPE hkOMSetRenderTargetsAndUAV(ID3D11DeviceContext* This, UINT NumRTVs,
                                                  ID3D11RenderTargetView* const* ppRenderTargetViews,
                                                  ID3D11DepthStencilView* pDepthStencilView, UINT UAVStartSlot,
                                                  UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
                                                  const UINT* pUAVInitialCounts)
{
    if (Watched(This))
    {
        g_watchedBound.store(true, std::memory_order_relaxed);

        // KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL changes only the UAVs: the depth buffer bound stays bound.
        if (g_core.Active() && NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
            OnBound(This, pDepthStencilView);
    }

    o_OMSetRenderTargetsAndUAV(This, NumRTVs, ppRenderTargetViews, pDepthStencilView, UAVStartSlot, NumUAVs,
                              ppUnorderedAccessViews, pUAVInitialCounts);
}

void STDMETHODCALLTYPE hkClearDepthStencilView(ID3D11DeviceContext* This, ID3D11DepthStencilView* pView,
                                               UINT ClearFlags, FLOAT Depth, UINT8 Stencil)
{
    if (g_core.Active() && (ClearFlags & D3D11_CLEAR_DEPTH) != 0 && Watched(This))
    {
        native::DepthBuffer buffer;
        ID3D11Resource* resource = nullptr;

        if (Describe(pView, &buffer, &resource))
        {
            const auto request = g_core.OnDepthClear((uint64_t) (size_t) This, buffer, Depth);

            if (request.take)
                TakeSnapshot(This, resource, "clear");

            resource->Release();
        }
    }

    o_ClearDepthStencilView(This, pView, ClearFlags, Depth, Stencil);
}

void STDMETHODCALLTYPE hkRSSetViewports(ID3D11DeviceContext* This, UINT NumViewports, const D3D11_VIEWPORT* pViewports)
{
    if (g_core.Active() && NumViewports > 0 && pViewports != nullptr && Watched(This))
        g_core.OnViewport((uint64_t) (size_t) This, pViewports[0].Width);

    o_RSSetViewports(This, NumViewports, pViewports);
}

void STDMETHODCALLTYPE hkDraw(ID3D11DeviceContext* This, UINT VertexCount, UINT StartVertexLocation)
{
    if (Watched(This))
    {
        g_core.OnDraw((uint64_t) (size_t) This, VertexCount, 1);
        native::lowlatency::OnFirstSubmit();
    }

    o_Draw(This, VertexCount, StartVertexLocation);
}

void STDMETHODCALLTYPE hkDrawIndexed(ID3D11DeviceContext* This, UINT IndexCount, UINT StartIndexLocation,
                                     INT BaseVertexLocation)
{
    if (Watched(This))
    {
        g_core.OnDraw((uint64_t) (size_t) This, IndexCount, 1);
        native::lowlatency::OnFirstSubmit();
    }

    o_DrawIndexed(This, IndexCount, StartIndexLocation, BaseVertexLocation);
}

void STDMETHODCALLTYPE hkDrawInstanced(ID3D11DeviceContext* This, UINT VertexCountPerInstance, UINT InstanceCount,
                                       UINT StartVertexLocation, UINT StartInstanceLocation)
{
    if (Watched(This))
    {
        g_core.OnDraw((uint64_t) (size_t) This, VertexCountPerInstance, InstanceCount);
        native::lowlatency::OnFirstSubmit();
    }

    o_DrawInstanced(This, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}

void STDMETHODCALLTYPE hkDrawIndexedInstanced(ID3D11DeviceContext* This, UINT IndexCountPerInstance,
                                              UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation,
                                              UINT StartInstanceLocation)
{
    if (Watched(This))
    {
        g_core.OnDraw((uint64_t) (size_t) This, IndexCountPerInstance, InstanceCount);
        native::lowlatency::OnFirstSubmit();
    }

    o_DrawIndexedInstanced(This, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation,
                          StartInstanceLocation);
}

void STDMETHODCALLTYPE hkDrawAuto(ID3D11DeviceContext* This)
{
    // The vertex count is in a GPU buffer (from a stream-output pass): unknown here, so it is counted like an indirect draw.
    if (Watched(This))
        g_core.OnIndirect((uint64_t) (size_t) This, 1);

    o_DrawAuto(This);
}

void STDMETHODCALLTYPE hkDrawIndexedInstancedIndirect(ID3D11DeviceContext* This, ID3D11Buffer* pBufferForArgs,
                                                      UINT AlignedByteOffsetForArgs)
{
    if (Watched(This))
        g_core.OnIndirect((uint64_t) (size_t) This, 1);

    o_DrawIndexedInstancedIndirect(This, pBufferForArgs, AlignedByteOffsetForArgs);
}

void STDMETHODCALLTYPE hkDrawInstancedIndirect(ID3D11DeviceContext* This, ID3D11Buffer* pBufferForArgs,
                                               UINT AlignedByteOffsetForArgs)
{
    if (Watched(This))
        g_core.OnIndirect((uint64_t) (size_t) This, 1);

    o_DrawInstancedIndirect(This, pBufferForArgs, AlignedByteOffsetForArgs);
}

void STDMETHODCALLTYPE hkDispatch(ID3D11DeviceContext* This, UINT X, UINT Y, UINT Z)
{
    if (g_core.Active() && !t_copying && Watched(This))
        g_core.Counters().dispatches.fetch_add(1, std::memory_order_relaxed);

    o_Dispatch(This, X, Y, Z);
}

// The game can make its device again (after a device loss, a settings change); the device it presents with says which one it
// draws with now. Another device's context is followed only once the watched one has set no render targets for 3 presents in a
// row (a game still drawing through it is not left for a second swap chain's device, or for a wrapping layer's device the swap
// chain answers with), and only if its calls run the hooks at all. A swap chain of another API (D3D12, frame generation's under
// Dx11wDx12) says nothing: the context from Install is kept. Present thread only.
void FollowPresentDevice(IDXGISwapChain* swapChain)
{
    static ID3D11Device* presentDevice = nullptr;         // the device presented with last, a reference held
    static ID3D11DeviceContext* presentContext = nullptr; // its immediate context, unwrapped, a reference held
    static int streak = 0;

    ID3D11Device* device = nullptr;

    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        return;

    if (device != presentDevice)
    {
        SAFE_RELEASE(presentContext);
        SAFE_RELEASE(presentDevice);
        presentDevice = device;
        presentDevice->AddRef();

        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);

        if (context != nullptr)
        {
            ID3D11DeviceContext* real = nullptr;

            if (!Util::CheckForRealObject(__FUNCTION__, context, (IUnknown**) &real))
                real = context;

            presentContext = real;
            presentContext->AddRef();
            context->Release();
        }
    }

    device->Release();

    const bool watchedBusy = g_watchedBound.exchange(false, std::memory_order_relaxed);

    if (presentContext == nullptr || presentContext == g_context || watchedBusy)
    {
        streak = 0;
        return;
    }

    if (++streak < 3)
        return;

    streak = 0;

    // A context whose functions are not the ones hooked is never seen: watching it would see nothing at all.
    if ((*(PVOID**) presentContext)[kOMSetRenderTargets] != g_hookedOMSetRenderTargets)
    {
        static bool loggedUnhooked = false;

        if (!loggedUnhooked)
        {
            loggedUnhooked = true;
            LOG_WARN("Depth finder (D3D11): the game presents with another device, whose immediate context runs other "
                     "functions than the ones hooked; it is not watched");
        }

        return;
    }

    LOG_INFO("Depth finder (D3D11): the game presents with another device now and draws nothing through the one watched; "
             "watching the new one's immediate context");
    UseContext(presentContext);
}

} // namespace

namespace GenericDepthDx11
{
bool Installed() { return g_installed; }
bool InstallFailed() { return g_installFailed; }

void Install(ID3D11Device* device)
{
    if (device == nullptr || g_installed || !Config::Instance()->DlssNrNativeDepthFinder.value_or_default())
        return;

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);

    if (context == nullptr)
    {
        LOG_WARN("Depth finder (D3D11): could not get the immediate context, not installed");
        g_installFailed = true;
        return;
    }

    ID3D11DeviceContext* realContext = nullptr;

    if (!Util::CheckForRealObject(__FUNCTION__, context, (IUnknown**) &realContext))
        realContext = context;

    PVOID* table = *(PVOID**) realContext;

    o_DrawIndexed = (PFN_DrawIndexed) table[kDrawIndexed];
    o_Draw = (PFN_Draw) table[kDraw];
    o_DrawIndexedInstanced = (PFN_DrawIndexedInstanced) table[kDrawIndexedInstanced];
    o_DrawInstanced = (PFN_DrawInstanced) table[kDrawInstanced];
    o_OMSetRenderTargets = (PFN_OMSetRenderTargets) table[kOMSetRenderTargets];
    o_OMSetRenderTargetsAndUAV = (PFN_OMSetRenderTargetsAndUnorderedAccessViews) table[kOMSetRenderTargetsAndUAV];
    o_DrawAuto = (PFN_DrawAuto) table[kDrawAuto];
    o_DrawIndexedInstancedIndirect = (PFN_DrawIndirect) table[kDrawIndexedInstancedIndirect];
    o_DrawInstancedIndirect = (PFN_DrawIndirect) table[kDrawInstancedIndirect];
    o_Dispatch = (PFN_Dispatch) table[kDispatch];
    o_RSSetViewports = (PFN_RSSetViewports) table[kRSSetViewports];
    o_ClearDepthStencilView = (PFN_ClearDepthStencilView) table[kClearDepthStencilView];
    g_hookedOMSetRenderTargets = table[kOMSetRenderTargets];

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    DetourAttach(&(PVOID&) o_DrawIndexed, hkDrawIndexed);
    DetourAttach(&(PVOID&) o_Draw, hkDraw);
    DetourAttach(&(PVOID&) o_DrawIndexedInstanced, hkDrawIndexedInstanced);
    DetourAttach(&(PVOID&) o_DrawInstanced, hkDrawInstanced);
    DetourAttach(&(PVOID&) o_OMSetRenderTargets, hkOMSetRenderTargets);
    DetourAttach(&(PVOID&) o_OMSetRenderTargetsAndUAV, hkOMSetRenderTargetsAndUAV);
    DetourAttach(&(PVOID&) o_DrawAuto, hkDrawAuto);
    DetourAttach(&(PVOID&) o_DrawIndexedInstancedIndirect, hkDrawIndexedInstancedIndirect);
    DetourAttach(&(PVOID&) o_DrawInstancedIndirect, hkDrawInstancedIndirect);
    DetourAttach(&(PVOID&) o_Dispatch, hkDispatch);
    DetourAttach(&(PVOID&) o_RSSetViewports, hkRSSetViewports);
    DetourAttach(&(PVOID&) o_ClearDepthStencilView, hkClearDepthStencilView);

    const auto result = DetourTransactionCommit();

    if (result != NO_ERROR)
    {
        LOG_ERROR("Depth finder (D3D11): hooking failed ({:X}), not installed", (UINT) result);
        context->Release();
        g_installFailed = true;
        return;
    }

    UseContext(realContext); // holds its own reference
    context->Release();

    g_installFailed = false;
    g_core.Start([](const std::string& line) { LOG_INFO("{}", line); });
    g_installed = true;
    LOG_INFO("Depth finder (D3D11): observing the game's depth buffers (immediate context only), after {} frames of "
             "warm-up; it stands down if the game makes an upscaler call",
             Config::Instance()->DlssNrNativeDepthWarmupFrames.value_or_default());
}

void OnPresent(IDXGISwapChain* swapChain)
{
    if (!g_installed || swapChain == nullptr)
        return;

    DXGI_SWAP_CHAIN_DESC desc {};

    if (FAILED(swapChain->GetDesc(&desc)))
        return;

    FollowPresentDevice(swapChain);

    // The copy taken is NOT forgotten here: the mid-frame "unbind" events that call TakeSnapshot happen while the game renders
    // the frame that is only now finishing (well before this Present call), so forgetting it at the top of this same function
    // would wipe out the copy they just took, before anything downstream (BestSnapshot, the diagnostic below) ever reads it. It
    // is instead let go below when the finder stands down, and kept fresh by every TakeSnapshot (a buffer not copied leaves no
    // copy).

    // The overlay preview (not yet drawn for D3D11) or the motion step needs the depth copy taken.
    g_core.SetSnapshotsWanted((Config::Instance()->DlssNrNativeDepthOverlay.value_or_default() &&
                               Config::Instance()->DlssNrNativeDebugView.value_or_default()) ||
                              Config::Instance()->DlssNrNativeMotion.value_or_default());

    // A game that neither re-clears nor unbinds the picked buffer before Present (it stays bound across many frames) would
    // otherwise never offer a copy: the immediate context has no "list closes" moment the way a D3D12 command list does, so this
    // is its equivalent, once per presented frame.
    if (g_context != nullptr)
    {
        const auto request = g_core.FlushForPresent((uint64_t) (size_t) g_context);

        if (request.take)
            TakeSnapshot(g_context, (ID3D11Resource*) (size_t) request.id, "present");
    }

    g_core.BeginPresent(desc.BufferDesc.Width, desc.BufferDesc.Height);
    const bool stoodDown =
        g_core.EndPresent(desc.BufferDesc.Width, desc.BufferDesc.Height,
                          Config::Instance()->DlssNrNativeDepthWarmupFrames.value_or_default());

    // Self-diagnosis: a pick that never gets a copy despite wanting one is otherwise invisible until someone asks why the trust
    // mask never runs.
    static uint64_t noCopyStreak = 0;
    bool copyTakenNow = false;
    {
        std::lock_guard lock(g_mutex);
        copyTakenNow = g_copy.Taken();
    }
    const bool stillNoCopy = g_core.Armed() && g_core.CurrentPick().valid && !copyTakenNow &&
                            (Config::Instance()->DlssNrNativeMotion.value_or_default() ||
                             (Config::Instance()->DlssNrNativeDepthOverlay.value_or_default() &&
                              Config::Instance()->DlssNrNativeDebugView.value_or_default()));

    if (stillNoCopy)
    {
        if (++noCopyStreak % 300 == 0)
        {
            const auto pick = g_core.CurrentPick();
            const auto diag = g_core.Diagnose((uint64_t) (size_t) g_context);
            LOG_WARN("Depth finder (D3D11): {} frames wanting a copy with no copy taken. Picked {:X}. Context: {}, {}, "
                     "bound to {:X}, this stretch {} vertices / {} draws (floor {}), wanted={}",
                     noCopyStreak, pick.id, diag.hasContext ? "known" : "UNKNOWN",
                     diag.hasBoundBuffer ? "has a bound depth buffer" : "NOTHING BOUND", diag.boundResource,
                     diag.currentVertices, diag.currentDrawcalls, g_core.SnapshotFloor(), diag.wanted);
        }
    }
    else
    {
        noCopyStreak = 0;
    }

    // Stood down: nothing is copied while the game calls an upscaler, so what the copies need (and the device it was made on)
    // is let go until the finder wakes.
    if (stoodDown)
    {
        std::lock_guard lock(g_mutex);
        g_copy.Release();
    }
}

GenericDepthSelect::Pick CurrentPick() { return g_core.CurrentPick(); }

Snapshot BestSnapshot()
{
    std::lock_guard lock(g_mutex);
    const auto pick = g_core.CurrentPick();

    Snapshot snap;

    if (g_copy.Taken() && pick.valid)
    {
        snap.valid = true;
        snap.resource = g_copy.Copy();
        snap.typelessFormat = DXGI_FORMAT_R32_FLOAT;
        snap.viewFormat = DXGI_FORMAT_R32_FLOAT;
        snap.width = g_copy.Width();
        snap.height = g_copy.Height();
        snap.reversed = pick.reversed;
    }

    return snap;
}

void NoteUpscalerCall()
{
    // Noted with no finder installed too: the menu and the frame source must know the game has an upscaler either way.
    native::NoteGameUpscalerCall();

    if (g_installed)
        g_core.NoteUpscalerCall();
}

bool GameCallsUpscaler() { return native::GameUpscalerCalledRecently() || (g_installed && g_core.GameCallsUpscaler()); }
bool Armed() { return g_installed && g_core.Armed(); }

void DrawStatus()
{
    const bool wanted = Config::Instance()->DlssNrNativeDepthFinder.value_or_default();

    switch (DlssNrNativeMode::FinderFor(wanted, g_installed, g_installFailed))
    {
    case DlssNrNativeMode::Finder::Off:
        ImGui::TextDisabled("Depth: off; NR runs on motion only and does not stand aside for a game upscaler.");
        return;
    case DlssNrNativeMode::Finder::NeedsRestart:
        ImGui::TextDisabled("Depth: the finder needs a restart.");
        return;
    case DlssNrNativeMode::Finder::CouldNotStart:
        ImGui::TextDisabled("Depth: the finder could not start (see the log); NR runs on motion only.");
        return;
    case DlssNrNativeMode::Finder::Installed:
        break;
    }

    const auto pick = CurrentPick();
    const uint32_t warmup = Config::Instance()->DlssNrNativeDepthWarmupFrames.value_or_default();
    const char* untilRestart = wanted ? "" : " (finder off at next start)";

    if (g_core.GameCallsUpscaler())
        ImGui::TextDisabled("Depth: stood down, the game is calling an upscaler. Turn it off in the game.");
    else if (!g_core.Armed())
        ImGui::TextDisabled("Depth: watching (%llu of %u frames)...%s",
                            (unsigned long long) (g_core.Presents() - g_core.WarmupStart()), warmup, untilRestart);
    else if (!pick.valid)
        ImGui::TextDisabled("Depth: none found yet; NR runs on motion only%s.", untilRestart);
    else
        ImGui::Text("Depth: picked %ux%u%s%s", pick.width, pick.height, pick.reversed ? ", reversed-Z" : "",
                    untilRestart);

    if (ImGui::IsItemHovered() && pick.valid)
        ImGui::SetTooltip("Format %u. The log has the candidates (Depth finder lines).", pick.format);
    else if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The log has the candidates (Depth finder lines).");
}

void DrawAdvancedUi()
{
    auto* config = Config::Instance();
    bool finder = config->DlssNrNativeDepthFinder.value_or_default();

    if (ImGui::Checkbox("Use the game's depth (better quality; needs a restart)##depthfinder11", &finder))
        config->DlssNrNativeDepthFinder = finder;

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "%s", "Watches the game's immediate context (Direct3D 11) and picks the scene's depth buffer, so\n"
                  "NR and the stabiliser get depth as well as motion. Optional: without it they run on motion\n"
                  "only. It is also what notices the game calling its own upscaler: without it, nothing here\n"
                  "stands aside for that. Deferred contexts are not watched yet. Choosing a mode above turns\n"
                  "it on, Off turns it off. Applies at the next start: save the settings and restart the game.");
}
} // namespace GenericDepthDx11
