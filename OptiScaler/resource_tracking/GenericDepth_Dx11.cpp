#include "pch.h"

#include "GenericDepth_Dx11.h"

#include <Config.h>
#include <Util.h>
#include <dlssnr/DlssNr_NativeMode.h>

#include <native/DepthFinderCore.h>
#include <native/SharedFrame.h>

#include <detours/detours.h>

#include <imgui/imgui.h>

#include <d3d11_4.h>
#include <d3dcompiler.h>

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

// A plain (non-shared) D3D11 copy of the picked depth buffer for the frame, always R32_FLOAT (see LinearizeDepth): one context
// means one copy, unlike the D3D12 finder's several command lists. Recreated when the size changes.
ID3D11Texture2D* g_copy = nullptr;
uint32_t g_copyWidth = 0, g_copyHeight = 0;
bool g_copyTaken = false; // a copy was recorded for the frame just closed
// The views the linearize pass reads and writes through, kept between copies: the write view goes with g_copy, the read view
// with the buffer and format it looks at (it holds a reference to that buffer until it is replaced).
ID3D11UnorderedAccessView* g_copyUav = nullptr;
ID3D11ShaderResourceView* g_sourceSrv = nullptr;
ID3D11Resource* g_sourceSrvResource = nullptr;
DXGI_FORMAT g_sourceSrvFormat = DXGI_FORMAT_UNKNOWN;
bool g_installed = false;
bool g_installFailed = false;

// Reads the picked depth buffer (whatever its own typeless/depth-stencil format) through a single-channel view and writes a
// plain R32_FLOAT copy, compiled once on first use. A typeless depth-stencil format (R32G8X24_TYPELESS and the like) can fail
// to make a cross-API (D3D11<->D3D12) shared NT handle outright (seen in practice: CreateTexture2D returns E_INVALIDARG for
// such a format with D3D11_RESOURCE_MISC_SHARED_NTHANDLE, even though the same device shares an ordinary colour texture of
// that size without trouble), where a plain float texture shares without issue; linearizing before sharing sidesteps the
// restriction rather than depending on it being lifted.
const char* kLinearizeSource = R"HLSL(
Texture2D<float> Src : register(t0);
RWTexture2D<float> Dst : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    Dst.GetDimensions(w, h);

    if (id.x >= w || id.y >= h)
        return;

    Dst[id.xy] = Src.Load(int3(id.xy, 0));
}
)HLSL";

ID3D11ComputeShader* g_linearizeCs = nullptr;
bool g_linearizeFailed = false;

ID3D11ComputeShader* LinearizeShader(ID3D11Device* device)
{
    if (g_linearizeCs != nullptr || g_linearizeFailed)
        return g_linearizeCs;

    ID3DBlob* code = nullptr;
    ID3DBlob* messages = nullptr;
    const HRESULT hr = D3DCompile(kLinearizeSource, strlen(kLinearizeSource), "DepthLinearize", nullptr, nullptr,
                                  "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages);

    if (FAILED(hr))
    {
        LOG_ERROR("Depth finder (D3D11): compiling the linearize shader failed: {}",
                 messages != nullptr ? (const char*) messages->GetBufferPointer() : "no message");
        if (messages != nullptr)
            messages->Release();
        g_linearizeFailed = true;
        return nullptr;
    }

    if (messages != nullptr)
        messages->Release();

    if (FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_linearizeCs)))
    {
        LOG_ERROR("Depth finder (D3D11): creating the linearize compute shader failed");
        g_linearizeFailed = true;
    }

    code->Release();
    return g_linearizeCs;
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

// Under g_mutex. Lets go of the cached read view and the reference it keeps on the game's depth buffer.
void ReleaseSourceView()
{
    if (g_sourceSrv != nullptr)
    {
        g_sourceSrv->Release();
        g_sourceSrv = nullptr;
    }

    if (g_sourceSrvResource != nullptr)
    {
        g_sourceSrvResource->Release();
        g_sourceSrvResource = nullptr;
    }

    g_sourceSrvFormat = DXGI_FORMAT_UNKNOWN;
}

// Reads `resource` (the picked buffer, in whatever format the game made it) through the linearize shader into the frame's
// D3D11-side slot, recreating it if the size changed.
void TakeSnapshot(ID3D11DeviceContext* context, ID3D11Device* device, ID3D11Resource* resource, uint32_t width,
                  uint32_t height, DXGI_FORMAT format, const char* where)
{
    static int calls = 0;

    if (calls < 8 || calls % 200 == 0)
        LOG_INFO("Depth finder (D3D11): TakeSnapshot call {} from {}, {:X} {}x{} format {}", calls, where,
                 (size_t) resource, width, height, (int) format);

    ++calls;

    DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN, view = DXGI_FORMAT_UNKNOWN;

    if (!native::SharedDepthFormats(format, &typeless, &view))
    {
        static bool loggedFormatFail = false;

        if (!loggedFormatFail)
        {
            loggedFormatFail = true;
            LOG_WARN("Depth finder (D3D11): format {} does not map to a shareable depth format, no copy", (int) format);
        }

        return;
    }

    ID3D11ComputeShader* shader = LinearizeShader(device);

    if (shader == nullptr)
        return;

    std::lock_guard lock(g_mutex);

    if (g_copy == nullptr || g_copyWidth != width || g_copyHeight != height)
    {
        if (g_copyUav != nullptr)
        {
            g_copyUav->Release();
            g_copyUav = nullptr;
        }

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
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

        const HRESULT hr = device->CreateTexture2D(&desc, nullptr, &g_copy);

        if (FAILED(hr))
        {
            g_copy = nullptr;
            LOG_WARN("Depth finder (D3D11): creating the {}x{} R32_FLOAT copy texture failed: {:X}", width, height,
                     (UINT) hr);
            return;
        }

        g_copyWidth = width;
        g_copyHeight = height;
    }

    if (g_sourceSrv == nullptr || g_sourceSrvResource != resource || g_sourceSrvFormat != view)
    {
        ReleaseSourceView();

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc {};
        srvDesc.Format = view;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        if (FAILED(device->CreateShaderResourceView(resource, &srvDesc, &g_sourceSrv)))
        {
            g_sourceSrv = nullptr;
            static bool loggedSrvFail = false;

            if (!loggedSrvFail)
            {
                loggedSrvFail = true;
                LOG_WARN("Depth finder (D3D11): creating the read view (format {}) on the picked buffer failed", (int) view);
            }

            return;
        }

        resource->AddRef();
        g_sourceSrvResource = resource;
        g_sourceSrvFormat = view;
    }

    if (g_copyUav == nullptr)
    {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc {};
        uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        const HRESULT uavResult = device->CreateUnorderedAccessView(g_copy, &uavDesc, &g_copyUav);

        if (FAILED(uavResult))
        {
            g_copyUav = nullptr;
            static bool loggedUavFail = false;

            if (!loggedUavFail)
            {
                loggedUavFail = true;
                LOG_WARN("Depth finder (D3D11): creating the write view on the copy texture failed: {:X}",
                         (UINT) uavResult);
            }

            return;
        }
    }

    // The game's own compute state: this runs in the middle of its frame, so what it had bound at slot 0 is put back after.
    ID3D11ComputeShader* gameShader = nullptr;
    ID3D11ClassInstance* gameInstances[D3D11_SHADER_MAX_INTERFACES] {};
    UINT gameInstanceCount = D3D11_SHADER_MAX_INTERFACES;
    ID3D11ShaderResourceView* gameSrv = nullptr;
    ID3D11UnorderedAccessView* gameUav = nullptr;
    context->CSGetShader(&gameShader, gameInstances, &gameInstanceCount);
    context->CSGetShaderResources(0, 1, &gameSrv);
    context->CSGetUnorderedAccessViews(0, 1, &gameUav);

    context->CSSetShader(shader, nullptr, 0);
    context->CSSetShaderResources(0, 1, &g_sourceSrv);
    context->CSSetUnorderedAccessViews(0, 1, &g_copyUav, nullptr);
    context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

    context->CSSetShaderResources(0, 1, &gameSrv);
    context->CSSetUnorderedAccessViews(0, 1, &gameUav, nullptr);
    context->CSSetShader(gameShader, gameInstances, gameInstanceCount);

    if (gameShader != nullptr)
        gameShader->Release();

    for (UINT i = 0; i < gameInstanceCount; ++i)
        if (gameInstances[i] != nullptr)
            gameInstances[i]->Release();

    if (gameSrv != nullptr)
        gameSrv->Release();

    if (gameUav != nullptr)
        gameUav->Release();

    g_copyTaken = true;

    static bool loggedFirst = false;

    if (!loggedFirst)
    {
        loggedFirst = true;
        LOG_INFO("Depth finder (D3D11): first depth copy taken, {}x{}, linearized to R32_FLOAT", g_copyWidth, g_copyHeight);
    }
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
            TakeSnapshot(context, dev, snapResource, desc.Width, desc.Height, desc.Format, "unbind");
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
                    TakeSnapshot(This, dev, resource, buffer.width, buffer.height, (DXGI_FORMAT) buffer.format, "clear");
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

    // Every draw counts on the immediate context's id, whichever context makes it, so draws count under the core's lock. Set
    // before the hooks go live: a draw on another thread must never see the lock-free path.
    g_core.SetSharedContexts(true);

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
        g_installFailed = true;
        return;
    }

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

    // g_copyTaken is NOT reset here: the mid-frame "unbind" events that call TakeSnapshot happen while the game renders the
    // frame that is only now finishing (well before this Present call), so resetting it at the top of this same function would
    // wipe out the very flag they just set, before anything downstream (BestSnapshot, the diagnostic below) ever reads it. It is
    // instead invalidated below when the finder stands down, and implicitly kept fresh by TakeSnapshot overwriting g_copy on
    // every successful copy.

    // The overlay preview (not yet drawn for D3D11) or the motion step needs the depth copy taken.
    g_core.SetSnapshotsWanted((Config::Instance()->DlssNrNativeDepthOverlay.value_or_default() &&
                               Config::Instance()->DlssNrNativeDebugView.value_or_default()) ||
                              Config::Instance()->DlssNrNativeMotion.value_or_default());

    // A game that neither re-clears nor unbinds the picked buffer before Present (it stays bound across many frames) would
    // otherwise never offer a copy: the immediate context has no "list closes" moment the way a D3D12 command list does, so this
    // is its equivalent, once per presented frame.
    if (g_context != nullptr)
    {
        const auto request = g_core.FlushForPresent(g_contextId);

        if (request.take)
        {
            ID3D11Resource* snapResource = (ID3D11Resource*) (size_t) request.id;
            ID3D11Texture2D* tex = nullptr;

            if (SUCCEEDED(snapResource->QueryInterface(IID_PPV_ARGS(&tex))))
            {
                D3D11_TEXTURE2D_DESC texDesc {};
                tex->GetDesc(&texDesc);
                tex->Release();

                ID3D11Device* dev = nullptr;
                g_context->GetDevice(&dev);

                if (dev != nullptr)
                {
                    TakeSnapshot(g_context, dev, snapResource, texDesc.Width, texDesc.Height, texDesc.Format, "present");
                    dev->Release();
                }
            }
        }
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
        copyTakenNow = g_copyTaken;
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
            const auto diag = g_core.Diagnose(g_contextId);
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

    if (stoodDown)
    {
        std::lock_guard lock(g_mutex);
        g_copyTaken = false;
    }

    // The cached read view keeps the buffer it reads alive: once that buffer is no longer the pick (a resolution change made
    // a new one, or the finder stood down), it goes, so the game's own release frees the old buffer.
    {
        const auto pick = g_core.CurrentPick();
        std::lock_guard lock(g_mutex);

        if (g_sourceSrvResource != nullptr && (!pick.valid || pick.id != (uint64_t) (size_t) g_sourceSrvResource))
            ReleaseSourceView();
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
        snap.typelessFormat = DXGI_FORMAT_R32_FLOAT;
        snap.viewFormat = DXGI_FORMAT_R32_FLOAT;
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
