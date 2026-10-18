#include "pch.h"

#include "DlssNr_ExposureScan_Dx11.h"
#include "DlssNr_ExposureScanCore.h"
#include "DlssNr_ExposureScanShape.h"

#include <Config.h>
#include <Util.h>
#include <native/VkPresentBridge.h>

#include <detours/detours.h>

#include <d3d11_4.h>

#include <atomic>
#include <string>

// See DlssNr_ExposureScan_Dx11.h. The D3D12 file (DlssNr_ExposureScan.cpp) has the reasons for the shape filter and the
// readback ring; this is the same on D3D11's terms:
// - D3D11 cannot copy a texture into a buffer, so each candidate gets small staging resources of its own kind (a 1x1
//   texture of its format, or a 16-byte buffer), one per ring slot;
// - a staging map with DO_NOT_WAIT never stalls: a copy the GPU has not finished is simply read at the next round;
// - nothing is released at a feature release: D3D11 has no placed heaps that a held reference could outlive (the D3D12
//   hazard). A candidate is let go once the scan's reference is the last one (the game has freed it).

namespace
{
using DlssNr::ExposureScan::Api;
using DlssNr::ExposureScan::Core::Tracked;
namespace Core = DlssNr::ExposureScan::Core;

// ID3D11Device vtable: IUnknown (0-2), then CreateBuffer, CreateTexture1D, CreateTexture2D (d3d11.h declaration order)
constexpr int kCreateBuffer = 3;
constexpr int kCreateTexture2D = 5;

using PFN_CreateBuffer = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device* This, const D3D11_BUFFER_DESC* pDesc,
                                                     const D3D11_SUBRESOURCE_DATA* pInitialData,
                                                     ID3D11Buffer** ppBuffer);
using PFN_CreateTexture2D = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device* This, const D3D11_TEXTURE2D_DESC* pDesc,
                                                        const D3D11_SUBRESOURCE_DATA* pInitialData,
                                                        ID3D11Texture2D** ppTexture2D);

PFN_CreateBuffer o_CreateBuffer = nullptr;
PFN_CreateTexture2D o_CreateTexture2D = nullptr;
std::atomic<bool> g_installed = false;
bool g_installFailed = false;

// One ring slot's staging resource for one candidate slot, and what it was made for.
struct Staging
{
    ID3D11Resource* resource = nullptr;
    ID3D11Device* device = nullptr; // not held: compared only
    uint32_t generation = 0;        // the candidate generation it was made for (0: none)
    bool copied = false;            // a copy into it is in flight or done, not yet read
    uint32_t copiedGeneration = 0;
};

Staging g_staging[Core::kSlots][Core::kMaxCandidates];
unsigned long long g_frames = 0;

// Same formats as the D3D12 filter (PlausibleFormat): float or typeless, one or two channels; the first channel's size.
unsigned int FirstChannelBytes(DXGI_FORMAT f, const char** name)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_FLOAT:
        *name = "R32_FLOAT";
        return 4;
    case DXGI_FORMAT_R16_FLOAT:
        *name = "R16_FLOAT";
        return 2;
    case DXGI_FORMAT_R32G32_FLOAT:
        *name = "R32G32_FLOAT";
        return 4;
    case DXGI_FORMAT_R16G16_FLOAT:
        *name = "R16G16_FLOAT";
        return 2;
    case DXGI_FORMAT_R32_TYPELESS:
        *name = "R32_TYPELESS";
        return 4;
    case DXGI_FORMAT_R16_TYPELESS:
        *name = "R16_TYPELESS";
        return 2;
    case DXGI_FORMAT_R32G32_TYPELESS:
        *name = "R32G32_TYPELESS";
        return 4;
    case DXGI_FORMAT_R16G16_TYPELESS:
        *name = "R16G16_TYPELESS";
        return 2;
    default:
        return 0;
    }
}

HRESULT STDMETHODCALLTYPE hkCreateBuffer(ID3D11Device* This, const D3D11_BUFFER_DESC* pDesc,
                                         const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Buffer** ppBuffer)
{
    const HRESULT hr = o_CreateBuffer(This, pDesc, pInitialData, ppBuffer);

    // Only what a shader writes is looked at (and counted): the lock is not taken for the rest
    if (Core::Held() || FAILED(hr) || pDesc == nullptr || ppBuffer == nullptr || *ppBuffer == nullptr ||
        (pDesc->BindFlags & D3D11_BIND_UNORDERED_ACCESS) == 0 || !Core::Active())
        return hr;

    std::lock_guard<std::mutex> lock(Core::Mutex());
    Core::ExaminedLocked();

    if (!DlssNrExposureScanShape::SmallBuffer(pDesc->ByteWidth))
        return hr;

    if (!Core::FromGameLocked(_ReturnAddress()))
        return hr;

    Core::AdoptLocked(Api::D3D11, (ID3D11Resource*) *ppBuffer, This,
                      "buffer, " + std::to_string((unsigned int) pDesc->ByteWidth) + " bytes", 4, true, 0);
    return hr;
}

HRESULT STDMETHODCALLTYPE hkCreateTexture2D(ID3D11Device* This, const D3D11_TEXTURE2D_DESC* pDesc,
                                            const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D)
{
    const HRESULT hr = o_CreateTexture2D(This, pDesc, pInitialData, ppTexture2D);

    if (Core::Held() || FAILED(hr) || pDesc == nullptr || ppTexture2D == nullptr || *ppTexture2D == nullptr ||
        (pDesc->BindFlags & D3D11_BIND_UNORDERED_ACCESS) == 0 || !Core::Active())
        return hr;

    std::lock_guard<std::mutex> lock(Core::Mutex());
    Core::ExaminedLocked();

    const char* name = nullptr;
    const unsigned int bytes = FirstChannelBytes(pDesc->Format, &name);

    if (bytes == 0 || pDesc->SampleDesc.Count != 1 ||
        !DlssNrExposureScanShape::SmallTexture(pDesc->Width, pDesc->Height))
        return hr;

    if (!Core::FromGameLocked(_ReturnAddress()))
        return hr;

    Core::AdoptLocked(Api::D3D11, (ID3D11Resource*) *ppTexture2D, This,
                      std::to_string(pDesc->Width) + "x" + std::to_string(pDesc->Height) + " " + name, bytes, false,
                      (uint32_t) pDesc->Format);
    return hr;
}

void InstallOn(ID3D11Device* device)
{
    if (device == nullptr || g_installed || g_installFailed)
        return;

    // A D3D11 game on dxvk with its Vulkan presents read (VkPresentBridge): the Vulkan scan sees the same resources as
    // dxvk's Vulkan buffers and images, where NR then runs; scanning both would count everything twice.
    if (VkPresentBridge::DxvkThroughVulkan())
    {
        g_installFailed = true;
        LOG_INFO("DLSS-NR exposure scan (D3D11): dxvk through Vulkan, the Vulkan scan covers this game");
        return;
    }

    // The device the game holds and calls (a Streamline proxy when there is one): hooking the device behind a proxy
    // would make every creation look like Streamline's, which the source check skips
    PVOID* table = *(PVOID**) device;
    o_CreateBuffer = (PFN_CreateBuffer) table[kCreateBuffer];
    o_CreateTexture2D = (PFN_CreateTexture2D) table[kCreateTexture2D];

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) o_CreateBuffer, hkCreateBuffer);
    DetourAttach(&(PVOID&) o_CreateTexture2D, hkCreateTexture2D);

    if (const auto result = DetourTransactionCommit(); result != NO_ERROR)
    {
        LOG_ERROR("DLSS-NR exposure scan (D3D11): hooking resource creation failed ({:X})", (UINT) result);
        o_CreateBuffer = nullptr;
        o_CreateTexture2D = nullptr;
        g_installFailed = true;
        return;
    }

    g_installed = true;
    LOG_INFO("DLSS-NR exposure scan (D3D11): watching the game's buffer and 2D texture creation");
}

void ReleaseStaging(Staging& s)
{
    if (s.resource != nullptr)
        s.resource->Release();

    s = Staging {};
}

// The staging resource for a candidate in one ring slot, made on first use and remade when the slot has a new owner or
// the presenting device changed. The caller holds the lock; the creation hooks pass these calls by (Core::HeldScope).
ID3D11Resource* StagingFor(Staging& s, const Tracked& t, ID3D11Device* device)
{
    if (s.resource != nullptr && s.generation == t.generation && s.device == device)
        return s.resource;

    ReleaseStaging(s);

    if (t.isBuffer)
    {
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth = 16;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Buffer* buffer = nullptr;

        if (SUCCEEDED(device->CreateBuffer(&desc, nullptr, &buffer)))
            s.resource = buffer;
    }
    else
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = 1;
        desc.Height = 1;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = (DXGI_FORMAT) t.format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* texture = nullptr;

        if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)))
            s.resource = texture;
    }

    s.device = device;
    s.generation = t.generation;
    return s.resource;
}
} // namespace

namespace DlssNr
{
namespace ExposureScanDx11
{
void Install(ID3D11Device* device)
{
    if (Core::Wanted())
        InstallOn(device);
}

void OnPresent(IDXGISwapChain* swapChain)
{
    if (swapChain == nullptr || !Core::Active())
        return;

    ID3D11Device* device = nullptr;

    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        return;

    // The device's own calls, not a proxy's (Streamline): the context the copies go on, and the device the candidates'
    // GetDevice gives back
    ID3D11Device* real = nullptr;

    if (!Util::CheckForRealObject(__FUNCTION__, device, (IUnknown**) &real) || real == nullptr)
        real = device;

    InstallOn(device);

    ID3D11DeviceContext* context = nullptr;
    real->GetImmediateContext(&context);

    if (context == nullptr || !g_installed)
    {
        if (context != nullptr)
            context->Release();

        device->Release();
        return;
    }

    std::lock_guard<std::mutex> lock(Core::Mutex());
    Core::HeldScope held;
    std::vector<Tracked>& list = Core::ListLocked();
    const unsigned int slot = (unsigned int) (g_frames % Core::kSlots);

    // Read what was copied kSlots presents ago; a copy not done yet is read next round
    for (size_t i = 0; i < list.size() && i < Core::kMaxCandidates; ++i)
    {
        Staging& s = g_staging[slot][i];

        if (!s.copied || s.resource == nullptr)
            continue;

        D3D11_MAPPED_SUBRESOURCE mapped {};
        const HRESULT hr = context->Map(s.resource, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);

        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
            continue;

        s.copied = false;

        if (SUCCEEDED(hr) && mapped.pData != nullptr)
        {
            Core::ConsumeLocked(i, s.copiedGeneration, mapped.pData);
            context->Unmap(s.resource, 0);
        }
    }

    // Let go of candidates the game has freed: the scan's reference is the last one
    const bool checkLifetimes = g_frames % 120 == 0;

    for (size_t i = 0; i < list.size() && i < Core::kMaxCandidates; ++i)
    {
        Tracked& t = list[i];

        if (!t.alive || t.api != Api::D3D11)
            continue;

        ID3D11Resource* resource = (ID3D11Resource*) t.handle;

        if (checkLifetimes)
        {
            resource->AddRef();

            if (resource->Release() <= 1)
            {
                Core::DropLocked(i);
                continue;
            }
        }

        // Only a candidate of the presenting device: another device's context cannot copy it
        ID3D11Device* owner = nullptr;
        resource->GetDevice(&owner);

        if (owner != nullptr)
            owner->Release();

        if (owner != real && owner != device)
            continue;

        Staging& s = g_staging[slot][i];

        if (s.copied) // still unread from kSlots presents ago: skip rather than overwrite a copy in flight
            continue;

        ID3D11Resource* staging = StagingFor(s, t, real);

        if (staging == nullptr)
            continue;

        if (t.isBuffer)
        {
            const D3D11_BOX box { 0, 0, 0, t.bytes, 1, 1 };
            context->CopySubresourceRegion(staging, 0, 0, 0, 0, resource, 0, &box);
        }
        else
        {
            const D3D11_BOX box { 0, 0, 0, 1, 1, 1 };
            context->CopySubresourceRegion(staging, 0, 0, 0, 0, resource, 0, &box);
        }

        s.copied = true;
        s.copiedGeneration = t.generation;
    }

    g_frames++;
    context->Release();
    device->Release();
}
} // namespace ExposureScanDx11
} // namespace DlssNr
