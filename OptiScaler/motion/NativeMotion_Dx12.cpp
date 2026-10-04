#include "pch.h"

#include "NativeMotion_Dx12.h"
#include "OpticalFlow_Dx12.h"

#include <Config.h>

#include <menu/menu_overlay_dx.h>
#include <resource_tracking/GenericDepth_Dx12.h>

#include <imgui/imgui.h>

#include <memory>

namespace
{

constexpr uint32_t kRing = 3;
constexpr float kPreviewMaxSpeed = 24.0f; // pixels per frame that show as full brightness

enum class Status
{
    Off,
    Waiting,   // the game calls an upscaler
    Failed,
    Running
};

std::unique_ptr<OpticalFlowDx12> g_flow;
ID3D12Device* g_device = nullptr;
ID3D12CommandAllocator* g_allocators[kRing] = {};
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Fence* g_fence = nullptr;
HANDLE g_event = nullptr;
UINT64 g_values[kRing] = {};
UINT64 g_signalled = 0;
uint64_t g_frame = 0;
uint32_t g_width = 0;
uint32_t g_height = 0;
bool g_previewWanted = false; // the menu node is open
bool g_previewReady = false;  // a preview has been recorded
Status g_status = Status::Off;
std::string g_failure;

// The menu's descriptor for the preview picture.
ID3D12DescriptorHeap* g_srvHeap = nullptr;
bool g_srvAllocated = false;
D3D12_CPU_DESCRIPTOR_HANDLE g_srvCpu {};
D3D12_GPU_DESCRIPTOR_HANDLE g_srvGpu {};
ID3D12Resource* g_srvResource = nullptr;

void WaitFor(UINT64 value)
{
    if (g_fence == nullptr || value == 0 || g_fence->GetCompletedValue() >= value)
        return;

    g_fence->SetEventOnCompletion(value, g_event);
    WaitForSingleObject(g_event, 1000);
}

bool CreateObjects(ID3D12Device* device)
{
    g_flow = std::make_unique<OpticalFlowDx12>();

    if (!g_flow->Init(device))
    {
        g_failure = g_flow->Error();
        g_flow.reset();
        return false;
    }

    for (auto& allocator : g_allocators)
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
        {
            g_failure = "creating a command allocator";
            return false;
        }

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[0], nullptr,
                                         IID_PPV_ARGS(&g_list))))
    {
        g_failure = "creating the command list";
        return false;
    }

    g_list->Close();

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
    {
        g_failure = "creating the fence";
        return false;
    }

    g_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    g_device = device;
    return true;
}

// The colour format to read the swap chain's buffer through (a typeless buffer needs a typed view).
DXGI_FORMAT ViewFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return format;
    }
}

} // namespace

namespace NativeMotionDx12
{

void OnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, ID3D12Device* device)
{
    if (!Config::Instance()->DlssNrNativeMotion.value_or_default())
    {
        g_status = Status::Off;
        return;
    }

    if (swapChain == nullptr || queue == nullptr || device == nullptr || g_status == Status::Failed)
        return;

    // The game has an upscaler of its own: this is not the case the producer is for, and the pictures before and after a
    // change would be compared across it.
    if (GenericDepthDx12::GameCallsUpscaler())
    {
        g_status = Status::Waiting;

        if (g_flow)
            g_flow->Reset();

        return;
    }

    if (g_flow == nullptr || g_device != device)
    {
        if (!CreateObjects(device))
        {
            g_status = Status::Failed;
            LOG_ERROR("Native motion: {}", g_failure);
            return;
        }

        LOG_INFO("Native motion: optical flow ready");
    }

    IDXGISwapChain3* chain3 = nullptr;
    UINT index = 0;

    if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&chain3))))
    {
        index = chain3->GetCurrentBackBufferIndex();
        chain3->Release();
    }

    ID3D12Resource* backBuffer = nullptr;

    if (FAILED(swapChain->GetBuffer(index, IID_PPV_ARGS(&backBuffer))))
        return;

    const D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();

    // The previous work may still be reading the textures a new size replaces.
    if (desc.Width != g_width || desc.Height != g_height)
    {
        WaitFor(g_signalled);
        g_width = (uint32_t) desc.Width;
        g_height = desc.Height;
        g_flow->Reset();
    }

    const UINT slot = (UINT) (g_frame % kRing);
    WaitFor(g_values[slot]);

    if (FAILED(g_allocators[slot]->Reset()) || FAILED(g_list->Reset(g_allocators[slot], nullptr)))
    {
        backBuffer->Release();
        return;
    }

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    g_list->ResourceBarrier(1, &barrier);

    const bool recorded = g_flow->Dispatch(g_list, backBuffer, ViewFormat(desc.Format));

    if (recorded && g_flow->FlowValid() && g_previewWanted)
        g_previewReady = g_flow->Visualise(g_list, kPreviewMaxSpeed) || g_previewReady;

    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    g_list->ResourceBarrier(1, &barrier);

    if (SUCCEEDED(g_list->Close()))
    {
        ID3D12CommandList* lists[] = { g_list };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(g_fence, ++g_signalled);
        g_values[slot] = g_signalled;
        ++g_frame;
        g_status = Status::Running;
    }

    backBuffer->Release();
    g_previewWanted = false;
}

void DrawDebugUi()
{
    auto* config = Config::Instance();

    if (!ImGui::TreeNode("Motion estimate (experimental)##nativemotion"))
        return;

    bool on = config->DlssNrNativeMotion.value_or_default();

    if (ImGui::Checkbox("Estimate motion of the picture##nativemotion", &on))
        config->DlssNrNativeMotion = on;

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Second step toward DLSS-NR in a game with no DLSS, FSR or XeSS: estimates how the picture\n"
                                "moves from one frame to the next on the GPU (optical flow). Nothing in the game changes.\n"
                                "It waits while the game calls an upscaler. Applies at once.");

    // Every branch writes one line and the picture has a box of its own size, so nothing below moves.
    switch (g_status)
    {
    case Status::Off:
        ImGui::TextDisabled("Off.");
        break;
    case Status::Waiting:
        ImGui::TextDisabled("Waiting: the game is calling an upscaler.");
        break;
    case Status::Failed:
        ImGui::TextDisabled("Could not start (see the log).");
        break;
    default:
        ImGui::TextDisabled("Hue is the direction, brightness the speed.");
        break;
    }

    const float boxWidth = 360.0f;
    const float boxHeight = boxWidth * 9.0f / 16.0f;
    bool drawn = false;

    if (g_status == Status::Running)
    {
        g_previewWanted = true;

        ID3D12DescriptorHeap* heap = MenuOverlayDx::SrvHeap();
        ID3D12Resource* preview = g_flow ? g_flow->Preview() : nullptr;

        if (heap != nullptr && preview != nullptr && g_previewReady)
        {
            if (heap != g_srvHeap)
            {
                g_srvHeap = heap;
                g_srvAllocated = false;
                g_srvResource = nullptr;
            }

            if (!g_srvAllocated && MenuOverlayDx::AllocSrv(&g_srvCpu, &g_srvGpu))
                g_srvAllocated = true;

            if (g_srvAllocated)
            {
                if (preview != g_srvResource)
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
                    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srv.Texture2D.MipLevels = 1;
                    g_device->CreateShaderResourceView(preview, &srv, g_srvCpu);
                    g_srvResource = preview;
                }

                ImGui::Image((ImTextureID) g_srvGpu.ptr, ImVec2(boxWidth, boxHeight));
                drawn = true;
            }
        }
    }

    if (!drawn)
        ImGui::Dummy(ImVec2(boxWidth, boxHeight));

    ImGui::TreePop();
}

} // namespace NativeMotionDx12
