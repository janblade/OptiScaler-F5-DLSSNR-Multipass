#include "pch.h"

#include "GenericDepth_Dx11.h"

#include <Config.h>
#include <Util.h>

#include <native/DepthFinderCore.h>
#include <native/SharedFrame.h>

#include <detours/detours.h>

#include <imgui/imgui.h>

#include <d3d11_4.h>

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
ID3D11DeviceContext* g_context = nullptr; // the immediate context, hooked once
uint64_t g_contextId = 0;

// A plain (non-shared) D3D11 copy of the picked depth buffer for the frame; one context means one copy, unlike the D3D12
// finder's several command lists. Recreated when the size or format changes.
ID3D11Texture2D* g_copy = nullptr;
DXGI_FORMAT g_copyTypeless = DXGI_FORMAT_UNKNOWN;
DXGI_FORMAT g_copyView = DXGI_FORMAT_UNKNOWN;
uint32_t g_copyWidth = 0, g_copyHeight = 0;
bool g_copyTaken = false; // a copy was recorded for the frame just closed
bool g_installed = false;

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

// Copies `resource` (the picked buffer) into the frame's D3D11-side slot, recreating it if the geometry changed.
void TakeSnapshot(ID3D11DeviceContext* context, ID3D11Device* device, ID3D11Resource* resource, uint32_t width,
                  uint32_t height, DXGI_FORMAT format)
{
    DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN, view = DXGI_FORMAT_UNKNOWN;

    if (!native::SharedDepthFormats(format, &typeless, &view))
        return;

    std::lock_guard lock(g_mutex);

    if (g_copy == nullptr || g_copyWidth != width || g_copyHeight != height || g_copyTypeless != typeless)
    {
        if (g_copy != nullptr)
        {
            g_copy->Release();
            g_copy = nullptr;
        }

        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = typeless;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;

        if (FAILED(device->CreateTexture2D(&desc, nullptr, &g_copy)))
        {
            g_copy = nullptr;
            return;
        }

        g_copyWidth = width;
        g_copyHeight = height;
        g_copyTypeless = typeless;
        g_copyView = view;
    }

    context->CopyResource(g_copy, resource);
    g_copyTaken = true;
}

void OnBound(ID3D11DeviceContext* context, ID3D11DepthStencilView* view)
{
    native::DepthBuffer buffer;
    ID3D11Resource* resource = nullptr;
    const bool hadDepth = view != nullptr;
    const bool have = hadDepth && Describe(view, &buffer, &resource);

    ID3D11Device* dev = nullptr;
    context->GetDevice(&dev);

    const auto request = g_core.OnDepthBound(g_contextId, hadDepth, have ? &buffer : nullptr);

    if (request.take && dev != nullptr)
    {
        ID3D11Resource* snapResource = (ID3D11Resource*) (size_t) request.id;
        D3D11_TEXTURE2D_DESC desc {};
        ID3D11Texture2D* tex = nullptr;

        if (SUCCEEDED(snapResource->QueryInterface(IID_PPV_ARGS(&tex))))
        {
            tex->GetDesc(&desc);
            tex->Release();
            TakeSnapshot(context, dev, snapResource, desc.Width, desc.Height, desc.Format);
        }
    }

    if (dev != nullptr)
        dev->Release();
    if (resource != nullptr)
        resource->Release();
}

void STDMETHODCALLTYPE hkOMSetRenderTargets(ID3D11DeviceContext* This, UINT NumViews,
                                            ID3D11RenderTargetView* const* ppRenderTargetViews,
                                            ID3D11DepthStencilView* pDepthStencilView)
{
    if (g_core.Active())
        OnBound(This, pDepthStencilView);

    o_OMSetRenderTargets(This, NumViews, ppRenderTargetViews, pDepthStencilView);
}

void STDMETHODCALLTYPE hkOMSetRenderTargetsAndUAV(ID3D11DeviceContext* This, UINT NumRTVs,
                                                  ID3D11RenderTargetView* const* ppRenderTargetViews,
                                                  ID3D11DepthStencilView* pDepthStencilView, UINT UAVStartSlot,
                                                  UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
                                                  const UINT* pUAVInitialCounts)
{
    if (g_core.Active())
        OnBound(This, pDepthStencilView);

    o_OMSetRenderTargetsAndUAV(This, NumRTVs, ppRenderTargetViews, pDepthStencilView, UAVStartSlot, NumUAVs,
                              ppUnorderedAccessViews, pUAVInitialCounts);
}

void STDMETHODCALLTYPE hkClearDepthStencilView(ID3D11DeviceContext* This, ID3D11DepthStencilView* pView,
                                               UINT ClearFlags, FLOAT Depth, UINT8 Stencil)
{
    if (g_core.Active() && (ClearFlags & D3D11_CLEAR_DEPTH) != 0)
    {
        native::DepthBuffer buffer;
        ID3D11Resource* resource = nullptr;

        if (Describe(pView, &buffer, &resource))
        {
            const auto request = g_core.OnDepthClear(g_contextId, buffer, Depth);

            if (request.take)
            {
                ID3D11Device* dev = nullptr;
                This->GetDevice(&dev);

                if (dev != nullptr)
                {
                    TakeSnapshot(This, dev, resource, buffer.width, buffer.height, (DXGI_FORMAT) buffer.format);
                    dev->Release();
                }
            }

            resource->Release();
        }
    }

    o_ClearDepthStencilView(This, pView, ClearFlags, Depth, Stencil);
}

void STDMETHODCALLTYPE hkRSSetViewports(ID3D11DeviceContext* This, UINT NumViewports, const D3D11_VIEWPORT* pViewports)
{
    if (g_core.Active() && NumViewports > 0 && pViewports != nullptr)
        g_core.OnViewport(g_contextId, pViewports[0].Width);

    o_RSSetViewports(This, NumViewports, pViewports);
}

void STDMETHODCALLTYPE hkDraw(ID3D11DeviceContext* This, UINT VertexCount, UINT StartVertexLocation)
{
    g_core.OnDraw(g_contextId, VertexCount, 1);
    o_Draw(This, VertexCount, StartVertexLocation);
}

void STDMETHODCALLTYPE hkDrawIndexed(ID3D11DeviceContext* This, UINT IndexCount, UINT StartIndexLocation,
                                     INT BaseVertexLocation)
{
    g_core.OnDraw(g_contextId, IndexCount, 1);
    o_DrawIndexed(This, IndexCount, StartIndexLocation, BaseVertexLocation);
}

void STDMETHODCALLTYPE hkDrawInstanced(ID3D11DeviceContext* This, UINT VertexCountPerInstance, UINT InstanceCount,
                                       UINT StartVertexLocation, UINT StartInstanceLocation)
{
    g_core.OnDraw(g_contextId, VertexCountPerInstance, InstanceCount);
    o_DrawInstanced(This, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}

void STDMETHODCALLTYPE hkDrawIndexedInstanced(ID3D11DeviceContext* This, UINT IndexCountPerInstance,
                                              UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation,
                                              UINT StartInstanceLocation)
{
    g_core.OnDraw(g_contextId, IndexCountPerInstance, InstanceCount);
    o_DrawIndexedInstanced(This, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation,
                          StartInstanceLocation);
}

void STDMETHODCALLTYPE hkDrawAuto(ID3D11DeviceContext* This)
{
    // The vertex count is in a GPU buffer (from a stream-output pass): unknown here, so it is counted like an indirect draw.
    g_core.OnIndirect(g_contextId, 1);
    o_DrawAuto(This);
}

void STDMETHODCALLTYPE hkDrawIndexedInstancedIndirect(ID3D11DeviceContext* This, ID3D11Buffer* pBufferForArgs,
                                                      UINT AlignedByteOffsetForArgs)
{
    g_core.OnIndirect(g_contextId, 1);
    o_DrawIndexedInstancedIndirect(This, pBufferForArgs, AlignedByteOffsetForArgs);
}

void STDMETHODCALLTYPE hkDrawInstancedIndirect(ID3D11DeviceContext* This, ID3D11Buffer* pBufferForArgs,
                                               UINT AlignedByteOffsetForArgs)
{
    g_core.OnIndirect(g_contextId, 1);
    o_DrawInstancedIndirect(This, pBufferForArgs, AlignedByteOffsetForArgs);
}

void STDMETHODCALLTYPE hkDispatch(ID3D11DeviceContext* This, UINT X, UINT Y, UINT Z)
{
    if (g_core.Active())
        g_core.Counters().dispatches.fetch_add(1, std::memory_order_relaxed);

    o_Dispatch(This, X, Y, Z);
}

} // namespace

namespace GenericDepthDx11
{
bool Installed() { return g_installed; }

void Install(ID3D11Device* device)
{
    if (device == nullptr || g_installed || !Config::Instance()->DlssNrNativeDepthFinder.value_or_default())
        return;

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);

    if (context == nullptr)
    {
        LOG_WARN("Depth finder (D3D11): could not get the immediate context, not installed");
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

    g_context = context; // keep the ref: the hooks run as long as the game uses this context
    g_contextId = (uint64_t) (size_t) realContext;

    if (result != NO_ERROR)
    {
        LOG_ERROR("Depth finder (D3D11): hooking failed ({:X}), not installed", (UINT) result);
        context->Release();
        g_context = nullptr;
        return;
    }

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

    {
        std::lock_guard lock(g_mutex);
        g_copyTaken = false;
    }

    // The overlay preview (not yet drawn for D3D11) or the motion step needs the depth copy taken.
    g_core.SetSnapshotsWanted((Config::Instance()->DlssNrNativeDepthOverlay.value_or_default() &&
                               Config::Instance()->DlssNrNativeDebugView.value_or_default()) ||
                              Config::Instance()->DlssNrNativeMotion.value_or_default());

    g_core.BeginPresent(desc.BufferDesc.Width, desc.BufferDesc.Height);
    const bool stoodDown =
        g_core.EndPresent(desc.BufferDesc.Width, desc.BufferDesc.Height,
                          Config::Instance()->DlssNrNativeDepthWarmupFrames.value_or_default());

    if (stoodDown)
    {
        std::lock_guard lock(g_mutex);
        g_copyTaken = false;
    }
}

GenericDepthSelect::Pick CurrentPick() { return g_core.CurrentPick(); }

Snapshot BestSnapshot()
{
    std::lock_guard lock(g_mutex);
    const auto pick = g_core.CurrentPick();

    Snapshot snap;

    if (g_copyTaken && g_copy != nullptr && pick.valid)
    {
        snap.valid = true;
        snap.resource = g_copy;
        snap.typelessFormat = g_copyTypeless;
        snap.viewFormat = g_copyView;
        snap.width = g_copyWidth;
        snap.height = g_copyHeight;
        snap.reversed = pick.reversed;
    }

    return snap;
}

void NoteUpscalerCall()
{
    if (g_installed)
        g_core.NoteUpscalerCall();
}

bool GameCallsUpscaler() { return g_installed && g_core.GameCallsUpscaler(); }
bool Armed() { return g_installed && g_core.Armed(); }

void DrawDebugUi()
{
    if (!ImGui::TreeNode("Depth finder, D3D11 (experimental)##depthfinder11"))
        return;

    auto* config = Config::Instance();
    bool finder = config->DlssNrNativeDepthFinder.value_or_default();

    if (ImGui::Checkbox("Find the scene's depth##depthfinder11", &finder))
        config->DlssNrNativeDepthFinder = finder;

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Same as the DirectX 12 depth finder, for a Direct3D 11 game: watches the game's immediate\n"
                                "context and picks the scene's depth buffer. Deferred contexts are not watched yet.\n"
                                "Applies at the next start: save the settings and restart the game.");

    if (!g_installed)
    {
        ImGui::TextDisabled("Not installed yet (needs a restart after turning it on).");
        ImGui::TreePop();
        return;
    }

    const auto pick = CurrentPick();
    const uint32_t warmup = Config::Instance()->DlssNrNativeDepthWarmupFrames.value_or_default();

    if (g_core.GameCallsUpscaler())
        ImGui::TextDisabled("Stood down: the game is calling an upscaler. Turn it off in the game.");
    else if (!g_core.Armed())
        ImGui::TextDisabled("Watching (%llu of %u frames)...",
                            (unsigned long long) (g_core.Presents() - g_core.WarmupStart()), warmup);
    else if (!pick.valid)
        ImGui::TextDisabled("No depth buffer qualifies yet.");
    else
        ImGui::Text("Picked %ux%u, format %u%s", pick.width, pick.height, pick.format,
                    pick.reversed ? ", reversed-Z" : "");

    ImGui::TextDisabled("The log has the candidates (Depth finder lines).");
    ImGui::TreePop();
}
} // namespace GenericDepthDx11
