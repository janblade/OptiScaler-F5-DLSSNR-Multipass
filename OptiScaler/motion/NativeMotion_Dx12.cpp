#include "pch.h"

#include "NativeMotion_Dx12.h"
#include "OpticalFlow_Dx12.h"
#include "TrustMask_Dx12.h"

#include <native/Dx12FrameSource.h>
#include <native/NativeProducer.h>
#include <native/VirtualUpscalerDriver.h>

#include <Config.h>

#include <menu/menu_overlay_dx.h>
#include <resource_tracking/GenericDepth_Dx12.h>
#include <dlssnr/DlssNrFeature_Dx12.h>

#include <imgui/imgui.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

namespace
{

constexpr float kPreviewMaxSpeed = 24.0f; // pixels per frame that show as full brightness

enum class Status
{
    Off,
    Waiting,   // the game calls an upscaler
    Failed,
    Running
};

std::unique_ptr<native::NativeProducer> g_producer;
native::Dx12FrameSource g_source;

// Made on first use and never destroyed at exit: its destructor would tear down an upscaler backend under the loader lock.
native::VirtualUpscalerDriver* g_virtualUpscaler = nullptr;

// Under frame generation the menu's present hook sees the real swap chain after frame generation ran, on its presenter's
// thread and queue; FGPresent drives the step instead. The menu's hook stands aside while FGPresent ran within this many of
// its presents (frame generation presents several frames for each of the game's).
constexpr uint64_t kFgPresentGrace = 8;
std::atomic<uint64_t> g_menuPresents { 0 };
std::atomic<uint64_t> g_menuPresentsAtFg { 0 };
std::atomic<bool> g_fgDriven { false };

// The two hooks run on different threads while frame generation starts up. The menu's only tries it: its thread is frame
// generation's presenter, which must never wait on the game thread.
std::mutex g_runMutex;

ID3D12Device* g_device = nullptr; // the menu's previews are made on it
uint64_t g_frame = 0;
bool g_previewWanted = false; // the menu node is open
bool g_previewReady = false;  // a flow preview has been recorded
bool g_trustRan = false;      // the trust mask was recorded in the last frame
bool g_nativeRan = false;     // DLSS-NR or the virtual upscaler ran on native input in the last frame
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

void ReleaseVirtualUpscaler()
{
    if (g_virtualUpscaler != nullptr)
        g_virtualUpscaler->Release();
}

void RunFrame(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, ID3D12Device* device)
{
    if (!Config::Instance()->DlssNrNativeMotion.value_or_default())
    {
        ReleaseVirtualUpscaler();
        g_status = Status::Off;
        return;
    }

    if (swapChain == nullptr || queue == nullptr || device == nullptr || g_status == Status::Failed)
        return;

    g_source.SetPresent(swapChain, queue);

    native::FrameInput input;
    const auto acquired = g_source.Acquire(input);

    if (acquired == native::AcquireStatus::WaitingForUpscaler)
    {
        // The game's own upscaler call takes over: ours goes, as a game's feature would.
        ReleaseVirtualUpscaler();
        g_status = Status::Waiting;

        if (g_producer)
            g_producer->Reset();

        return;
    }

    if (acquired != native::AcquireStatus::Ready)
        return;

    if (g_producer == nullptr || g_producer->Device() != device)
    {
        auto fresh = std::make_unique<native::NativeProducer>();

        if (!fresh->Init(device))
        {
            g_failure = fresh->Error();
            g_status = Status::Failed;
            LOG_ERROR("Native motion: {}", g_failure);
            g_source.Return(input, native::FrameOutput {});
            return;
        }

        // The upscaler backend belongs to the old device too.
        if (g_virtualUpscaler != nullptr)
        {
            delete g_virtualUpscaler;
            g_virtualUpscaler = nullptr;
        }

        g_producer = std::move(fresh);
        g_device = device;
        LOG_INFO("Native motion: optical flow and trust mask ready");
    }

    // Presents the picture to an upscaler backend as a synthetic call instead of running DLSS-NR on it. Takes priority when
    // both are on.
    const bool useVirtualUpscaler = Config::Instance()->DlssNrNativeUpscaler.value_or_default();

    if (useVirtualUpscaler && g_virtualUpscaler == nullptr)
        g_virtualUpscaler = new native::VirtualUpscalerDriver();
    else if (!useVirtualUpscaler)
        ReleaseVirtualUpscaler();

    native::NativeProducer::Options options;
    options.apply = useVirtualUpscaler || Config::Instance()->DlssNrNativeInput.value_or_default();
    options.flowPreview = g_previewWanted;
    options.previewMaxSpeed = kPreviewMaxSpeed;

    const auto apply = [useVirtualUpscaler](ID3D12GraphicsCommandList* cmd, const native::NativeFrame& frame)
    {
        if (useVirtualUpscaler)
            return g_virtualUpscaler->Run(cmd, frame);

        const DXGI_COLOR_SPACE_TYPE type =
            frame.space == native::ColorSpace::ScRgb ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
            : frame.space == native::ColorSpace::Pq  ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                                     : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        return DlssNr::ApplyNativeInput(g_source.Queue(), cmd, frame.color, frame.depth, frame.motion,
                                        frame.depthReversed, frame.reset, type, frame.pictureState);
    };

    native::FrameOutput output;
    const auto result = g_producer->Run(queue, input, options, apply, output);
    g_source.Return(input, output);

    g_trustRan = result.trustRan;
    g_nativeRan = result.nativeRan;

    if (result.trustRan)
        g_trustFrame = g_frame;

    if (result.flowValid)
    {
        // How often the depth finder has a copy for the mask: the log shows it every 600 frames.
        static uint64_t seen = 0, ran = 0;
        ++seen;
        ran += result.trustRan ? 1 : 0;

        if (seen == 600)
        {
            LOG_INFO("Native motion: the trust mask ran in {} of {} frames", ran, seen);
            seen = ran = 0;
        }
    }

    if (result.sceneCut)
    {
        ++g_cuts;
        LOG_INFO("Native motion: scene cut seen ({:.0f}% of the picture distrusted), histories reset",
                 result.distrustedShare * 100.0f);
    }

    if (result.submitted)
    {
        ++g_frame;
        g_status = Status::Running;
    }

    g_previewWanted = false;
}

} // namespace

namespace NativeMotionDx12
{

void OnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, ID3D12Device* device)
{
    const uint64_t presents = ++g_menuPresents;

    if (g_fgDriven.load() && presents - g_menuPresentsAtFg.load() <= kFgPresentGrace)
        return;

    std::unique_lock lock(g_runMutex, std::try_to_lock);

    if (!lock.owns_lock())
        return;

    g_fgDriven = false;
    GenericDepthDx12::OnPresent(swapChain);
    RunFrame(swapChain, queue, device);
}

void OnFGPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, ID3D12Device* device)
{
    g_menuPresentsAtFg = g_menuPresents.load();
    g_fgDriven = true;

    std::lock_guard lock(g_runMutex);
    GenericDepthDx12::OnPresent(swapChain);
    RunFrame(swapChain, queue, device);
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

    bool virtualUpscaler = config->DlssNrNativeUpscaler.value_or_default();

    if (ImGui::Checkbox("Present this to OptiScaler as an upscaler (experimental)##nativeupscaler", &virtualUpscaler))
        config->DlssNrNativeUpscaler = virtualUpscaler;

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "%s", "Experimental. Instead of feeding DLSS-NR directly, presents the depth finder's depth and the\n"
                  "estimated motion to OptiScaler's upscaler (the one chosen in the menu, FSR when none is) as if\n"
                  "the game had called it. Render size equals output size and jitter is zero, so it works as a\n"
                  "stabiliser, not a reconstruction; it makes frame generation with the Upscaler input work in a\n"
                  "game with no upscaler. Takes priority over Run Neural Rendering on this. Applies at once.");

    if (virtualUpscaler)
    {
        if (g_nativeRan && g_virtualUpscaler != nullptr && g_virtualUpscaler->Active())
            ImGui::TextDisabled("%s is running on this picture.", g_virtualUpscaler->BackendName().c_str());
        else if (g_virtualUpscaler != nullptr && !g_virtualUpscaler->Error().empty())
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "Upscaler: %s", g_virtualUpscaler->Error().c_str());
        else
            ImGui::TextDisabled("Upscaler: waiting for the first frame with depth.");
    }

    const bool debugView = config->DlssNrNativeDebugView.value_or_default();

    if (debugView && g_producer && ImGui::TreeNode("Flow tuning (to compare, applies at once)##flowtuning"))
    {
        auto& tune = g_producer->Flow()->Tuning();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderInt("Smoothing radius (0 = off)##flowsmooth", &tune.smoothRadius, 0, 3);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderInt("Search radius##flowsearch", &tune.radius, 1, 3);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderInt("Coarse cells as candidates##flowcells", &tune.coarseCells, 1, 4);
        ImGui::Checkbox("Last frame's flow as a candidate##flowhistory", &tune.useHistory);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderFloat("Confidence knee##flowknee", &tune.confidenceKnee, 0.0005f, 0.05f, "%.4f",
                           ImGuiSliderFlags_Logarithmic);

        if (ImGui::Button("Close to the earlier build##flowold"))
        {
            tune.smoothRadius = 0;
            tune.radius = 2;
            tune.coarseCells = 1;
            tune.useHistory = false;
        }

        ImGui::SameLine();

        if (ImGui::Button("Defaults##flowdefaults"))
            tune = OpticalFlowDx12::Settings {};

        ImGui::TreePop();
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

    // The pictures and their controls are debugging aids: off by default, nothing is recorded for them while they are hidden.
    if (!debugView)
    {
        ImGui::TreePop();
        return;
    }

    const float boxWidth = 360.0f;
    const float boxHeight = boxWidth * 9.0f / 16.0f;
    bool drawn = false;

    if (g_status == Status::Running)
    {
        g_previewWanted = true;

        if (g_producer && g_producer->PreviewReady())
            drawn = ShowTexture(g_flowView, g_producer->Flow()->Preview(), DXGI_FORMAT_R8G8B8A8_UNORM, false, boxWidth, boxHeight);
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

    if (g_producer)
    {
        static const char* kViews[] = { "Final mask", "Depth check", "Revealed-surface check", "Flow consistency check",
                                        "Luma check", "Outside the picture" };
        int view = g_producer->Trust()->Tuning().debugView;

        ImGui::SetNextItemWidth(220.0f);

        if (ImGui::Combo("Show##trustview", &view, kViews, IM_ARRAYSIZE(kViews)))
            g_producer->Trust()->Tuning().debugView = view;
    }

    bool maskDrawn = false;

    if (g_status == Status::Running && trustRecent && g_producer)
        maskDrawn = ShowTexture(g_maskView, g_producer->Trust()->Mask(), DXGI_FORMAT_R8_UNORM, true, boxWidth, boxHeight);

    if (!maskDrawn)
        ImGui::Dummy(ImVec2(boxWidth, boxHeight));

    ImGui::TreePop();
}

} // namespace NativeMotionDx12
