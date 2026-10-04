#include "pch.h"

#include "NativeMotion_Dx12.h"
#include "OpticalFlow_Dx12.h"
#include "TrustMask_Dx12.h"

#include <Config.h>

#include <menu/menu_overlay_dx.h>
#include <resource_tracking/GenericDepth_Dx12.h>
#include <dlssnr/DlssNrFeature_Dx12.h>

#include <imgui/imgui.h>

#include <algorithm>
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
std::unique_ptr<TrustMaskDx12> g_trust;
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
bool g_previewReady = false;  // a flow preview has been recorded
bool g_trustRan = false;      // the trust mask was recorded in the last frame
bool g_nativeRan = false;     // DLSS-NR ran on native input in the last frame
uint64_t g_trustFrame = 0;    // the last frame it was
uint64_t g_cuts = 0;          // scene cuts the mask reported
Status g_status = Status::Off;
std::string g_failure;

// The menu's descriptors for the two preview pictures.
struct PreviewView
{
    bool allocated = false;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu {};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu {};
    ID3D12Resource* resource = nullptr;
};

ID3D12DescriptorHeap* g_srvHeap = nullptr;
PreviewView g_flowView;
PreviewView g_maskView;

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
    g_trust = std::make_unique<TrustMaskDx12>();

    if (!g_flow->Init(device))
    {
        g_failure = g_flow->Error();
        g_flow.reset();
        g_trust.reset();
        return false;
    }

    if (!g_trust->Init(device))
    {
        g_failure = g_trust->Error();
        g_flow.reset();
        g_trust.reset();
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

// Shows a texture in the menu: a descriptor in the menu's heap (made once, and again if the heap or the texture changes).
// `grayFromRed` shows a one-channel texture as gray.
bool ShowTexture(PreviewView& view, ID3D12Resource* texture, DXGI_FORMAT format, bool grayFromRed, float width,
                 float height)
{
    ID3D12DescriptorHeap* heap = MenuOverlayDx::SrvHeap();

    if (heap == nullptr || texture == nullptr)
        return false;

    if (heap != g_srvHeap)
    {
        g_srvHeap = heap;
        g_flowView = PreviewView {};
        g_maskView = PreviewView {};
    }

    if (!view.allocated && MenuOverlayDx::AllocSrv(&view.cpu, &view.gpu))
        view.allocated = true;

    if (!view.allocated)
        return false;

    if (texture != view.resource)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping =
            grayFromRed ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                              D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
                              D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
                              D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
                              D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1)
                        : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        g_device->CreateShaderResourceView(texture, &srv, view.cpu);
        view.resource = texture;
    }

    ImGui::Image((ImTextureID) view.gpu.ptr, ImVec2(width, height));
    return true;
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
        if (g_trust)
            g_trust->Reset();

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

        LOG_INFO("Native motion: optical flow and trust mask ready");
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
        g_trust->Reset();
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

    g_trustRan = false;
    g_nativeRan = false;
    bool backBufferInPresent = false;
    TrustMaskDx12::Inputs nativeInputs;
    bool nativeReady = false;
    bool nativeReset = false;

    if (recorded && g_flow->FlowValid())
    {
        if (g_previewWanted)
            g_previewReady = g_flow->Visualise(g_list, kPreviewMaxSpeed) || g_previewReady;

        // The trust mask needs the scene's depth as the depth finder copied it this frame.
        const auto depth = GenericDepthDx12::BestSnapshot();

        if (depth.valid)
        {
            TrustMaskDx12::Inputs in;
            in.flow = g_flow->Flow();
            in.flowWidth = g_flow->FlowWidth();
            in.flowHeight = g_flow->FlowHeight();
            in.fullPerFlow = (float) desc.Width / (float) g_flow->FlowWidth();
            in.lumaNow = g_flow->LumaOfLastFrame();
            in.lumaBefore = g_flow->LumaOfFrameBefore();
            in.depthCount = (std::min)(depth.copyCount, (int) TrustMaskDx12::Inputs::kMaxDepths);

            for (int i = 0; i < in.depthCount; ++i)
                in.depths[i] = depth.copies[i];

            in.depthFormat = depth.viewFormat;
            in.depthWidth = depth.width;
            in.depthHeight = depth.height;
            in.depthReversed = depth.reversed;
            g_trustRan = g_trust->Dispatch(g_list, in);
            nativeInputs = in;
            nativeReady = g_trustRan;

            if (g_trustRan)
                g_trustFrame = g_frame;
        }

        // How often the depth finder has a copy for the mask: the log shows it every 600 frames.
        static uint64_t seen = 0, ran = 0;
        ++seen;
        ran += g_trustRan ? 1 : 0;

        if (seen == 600)
        {
            LOG_INFO("Native motion: the trust mask ran in {} of {} frames", ran, seen);
            seen = ran = 0;
        }

        // A hard cut: nothing carried over from before it is worth keeping.
        if (g_trust->SceneCutSeen())
        {
            ++g_cuts;
            LOG_INFO("Native motion: scene cut seen ({:.0f}% of the picture distrusted), histories reset",
                     g_trust->DistrustedShare() * 100.0f);
            g_flow->Reset();
            g_trust->Reset();
            nativeReady = false;
            nativeReset = true;
        }
    }

    // Native input: DLSS-NR on this picture with the finder's depth and the flow, on this same list.
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    g_list->ResourceBarrier(1, &barrier);
    backBufferInPresent = true;

    if (nativeReady && Config::Instance()->DlssNrNativeInput.value_or_default())
    {
        if (g_trust->BuildGuides(g_list, nativeInputs, (uint32_t) desc.Width, desc.Height))
        {
            // The fallback to the plain picture where the last frame cannot be trusted: keep the picture, run NR, fade back.
            const float fallback = std::clamp(Config::Instance()->DlssNrNativeInputFallback.value_or_default(), 0.0f, 1.0f);
            const bool keep = fallback > 0.0f && g_trust->CopyPicture(g_list, backBuffer);

            g_nativeRan = DlssNr::ApplyNativeInput(swapChain, queue, g_list, backBuffer, g_trust->GuideDepth(),
                                                    g_trust->GuideMotion(), nativeInputs.depthReversed, nativeReset);

            if (keep && g_nativeRan)
                g_trust->BlendWithPicture(g_list, backBuffer, fallback);
        }
    }

    (void) backBufferInPresent;

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

    bool feed = config->DlssNrNativeInput.value_or_default();

    if (ImGui::Checkbox("Run Neural Rendering on this (native input)##nativeinput", &feed))
        config->DlssNrNativeInput = feed;

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Experimental. Feeds DLSS-NR the depth finder's depth and the estimated motion, so it runs in a\n"
                                "game with no upscaler. Needs the depth finder, Finished picture and Enable Neural Rendering on.\n"
                                "SDR and scRGB only for now. Applies at once.");

    if (feed)
    {
        float fallback = config->DlssNrNativeInputFallback.value_or_default();

        ImGui::SetNextItemWidth(220.0f);

        if (ImGui::SliderFloat("Fall back to the plain picture##nativefallback", &fallback, 0.0f, 1.0f, "%.2f"))
            config->DlssNrNativeInputFallback = fallback;

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", "Where the trust mask says the last frame cannot be trusted (a surface that was just uncovered,\n"
                                    "a cut), fade the Neural Rendering result back toward the picture as the game drew it, by\n"
                                    "this amount times the mask. 0 is off. Costs a copy and a blend of the picture.");
    }

    if (feed)
        ImGui::TextDisabled("%s", g_nativeRan ? "Native input: NR is running on this picture."
                                              : DlssNr::FinishedPictureStatus().c_str());

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Second step toward DLSS-NR in a game with no DLSS, FSR or XeSS: estimates how the picture\n"
                                "moves from one frame to the next on the GPU (optical flow), and with the depth finder's\n"
                                "depth which pixels the previous frame cannot be trusted at. The depth copy is recorded\n"
                                "into the game's own command list. It waits while the game calls an upscaler. Applies at once.");

    // Every branch writes one line and each picture has a box of its own size, so nothing below moves.
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
        ImGui::TextDisabled("Motion: hue is the direction, brightness the speed.");
        break;
    }

    const float boxWidth = 360.0f;
    const float boxHeight = boxWidth * 9.0f / 16.0f;
    bool drawn = false;

    if (g_status == Status::Running)
    {
        g_previewWanted = true;

        if (g_flow && g_previewReady)
            drawn = ShowTexture(g_flowView, g_flow->Preview(), DXGI_FORMAT_R8G8B8A8_UNORM, false, boxWidth, boxHeight);
    }

    if (!drawn)
        ImGui::Dummy(ImVec2(boxWidth, boxHeight));

    // A frame can have no depth copy; the picture then keeps the last mask rather than going black for a frame.
    const bool trustRecent = g_trustFrame != 0 && g_frame - g_trustFrame < 30;

    if (g_status == Status::Running)
    {
        if (trustRecent)
            ImGui::TextDisabled("Trust: white is where the last frame cannot be trusted (%llu cuts seen).",
                                (unsigned long long) g_cuts);
        else
            ImGui::TextDisabled("Trust: waiting for the depth finder's copy of the depth.");
    }
    else
        ImGui::TextDisabled("Trust: -");

    if (g_trust)
    {
        static const char* kViews[] = { "Final mask", "Depth check", "Revealed-surface check", "Flow consistency check",
                                        "Luma check", "Outside the picture" };
        int view = g_trust->Tuning().debugView;

        ImGui::SetNextItemWidth(220.0f);

        if (ImGui::Combo("Show##trustview", &view, kViews, IM_ARRAYSIZE(kViews)))
            g_trust->Tuning().debugView = view;
    }

    bool maskDrawn = false;

    if (g_status == Status::Running && trustRecent && g_trust)
        maskDrawn = ShowTexture(g_maskView, g_trust->Mask(), DXGI_FORMAT_R8_UNORM, true, boxWidth, boxHeight);

    if (!maskDrawn)
        ImGui::Dummy(ImVec2(boxWidth, boxHeight));

    ImGui::TreePop();
}

} // namespace NativeMotionDx12
