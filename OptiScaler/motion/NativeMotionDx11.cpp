#include "pch.h"

#include "NativeMotionDx11.h"

#include <native/Dx11FrameSource.h>
#include <native/NativeProducer.h>
#include <native/VirtualUpscalerDriver.h>

#include <Config.h>
#include <dlssnr/DlssNrFeature_Dx12.h>
#include <resource_tracking/GenericDepth_Dx11.h>

#include <imgui/imgui.h>

#include <memory>

// The D3D11 counterpart of motion/NativeMotion_Dx12.cpp: same producer, a different adapter (native::Dx11FrameSource, the
// shared-texture transport). No live flow/trust picture here yet: they are D3D12 textures and the D3D11 game's menu renders
// with ImGui_ImplDX11, which needs a D3D11 shader-resource view; showing them would need another shared-texture round trip,
// not done for Story B's first game. The checkboxes and the status line work the same as the D3D12 driver.

namespace
{

enum class Status
{
    Off,
    Waiting, // the game calls an upscaler
    Failed,
    Running
};

std::unique_ptr<native::NativeProducer> g_producer;
native::Dx11FrameSource g_source;

// Made on first use and never destroyed at exit: its destructor would tear down an upscaler backend under the loader lock.
native::VirtualUpscalerDriver* g_virtualUpscaler = nullptr;

uint64_t g_frame = 0;
bool g_trustRan = false;
bool g_nativeRan = false;
uint64_t g_trustFrame = 0;
uint64_t g_cuts = 0;
Status g_status = Status::Off;
std::string g_failure;

void ReleaseVirtualUpscaler()
{
    if (g_virtualUpscaler != nullptr)
        g_virtualUpscaler->Release();
}

// Shared by OnPresent and OnFGPresent; the depth finder's frame close is the caller's job (see the two entry points
// below), since only one of them needs to do it. Returns the D3D12 picture the producer ended up with this frame, or
// null when nothing ran.
ID3D12Resource* RunFrame(IDXGISwapChain* swapChain, ID3D11Device* device)
{
    if (!Config::Instance()->DlssNrNativeMotion.value_or_default())
    {
        ReleaseVirtualUpscaler();
        g_status = Status::Off;
        return nullptr;
    }

    if (swapChain == nullptr || device == nullptr || g_status == Status::Failed)
        return nullptr;

    g_source.SetPresent(swapChain, device);

    native::FrameInput input;
    const auto acquired = g_source.Acquire(input);

    if (acquired == native::AcquireStatus::WaitingForUpscaler)
    {
        // The game's own upscaler call takes over: ours goes, as a game's feature would.
        ReleaseVirtualUpscaler();
        g_status = Status::Waiting;

        if (g_producer)
            g_producer->Reset();

        return nullptr;
    }

    if (acquired != native::AcquireStatus::Ready)
        return nullptr;

    if (g_producer == nullptr || g_producer->Device() != g_source.Device12())
    {
        auto fresh = std::make_unique<native::NativeProducer>();

        if (!fresh->Init(g_source.Device12()))
        {
            g_failure = fresh->Error();
            g_status = Status::Failed;
            LOG_ERROR("Native motion (D3D11): {}", g_failure);
            g_source.Return(input, native::FrameOutput {});
            return nullptr;
        }

        // The upscaler backend belongs to the old device too.
        if (g_virtualUpscaler != nullptr)
        {
            delete g_virtualUpscaler;
            g_virtualUpscaler = nullptr;
        }

        g_producer = std::move(fresh);
        LOG_INFO("Native motion (D3D11): optical flow and trust mask ready, on a private D3D12 device paired with "
                 "the game's D3D11 device");
    }

    // Presents the picture to an upscaler backend as a synthetic call instead of running DLSS-NR on it. Takes
    // priority when both are on. Inert (same as the plain native-input checkbox and Finished Picture) while a D3D11
    // game's swap chain has been replaced by Dx11wDx12SC for frame generation -- see OnFGPresent for that path.
    const bool useVirtualUpscaler = Config::Instance()->DlssNrNativeUpscaler.value_or_default();

    if (useVirtualUpscaler && g_virtualUpscaler == nullptr)
        g_virtualUpscaler = new native::VirtualUpscalerDriver();
    else if (!useVirtualUpscaler)
        ReleaseVirtualUpscaler();

    native::NativeProducer::Options options;
    options.apply = useVirtualUpscaler || Config::Instance()->DlssNrNativeInput.value_or_default();

    const auto apply = [useVirtualUpscaler](ID3D12GraphicsCommandList* cmd, const native::NativeFrame& frame)
    {
        if (useVirtualUpscaler)
            return g_virtualUpscaler->Run(cmd, frame);

        const DXGI_COLOR_SPACE_TYPE type =
            frame.space == native::ColorSpace::ScRgb ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
            : frame.space == native::ColorSpace::Pq  ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                                     : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        return DlssNr::ApplyNativeInput(g_source.Queue12(), cmd, frame.color, frame.depth, frame.motion,
                                        frame.depthReversed, frame.reset, type, frame.pictureState);
    };

    native::FrameOutput output;
    const auto result = g_producer->Run(g_source.Queue12(), input, options, apply, output);
    g_source.Return(input, output);

    g_trustRan = result.trustRan;
    g_nativeRan = result.nativeRan;

    if (result.trustRan)
        g_trustFrame = g_frame;

    if (result.flowValid)
    {
        static uint64_t seen = 0, ran = 0;
        ++seen;
        ran += result.trustRan ? 1 : 0;

        if (seen == 600)
        {
            LOG_INFO("Native motion (D3D11): the trust mask ran in {} of {} frames", ran, seen);
            seen = ran = 0;
        }
    }

    if (result.sceneCut)
    {
        ++g_cuts;
        LOG_INFO("Native motion (D3D11): scene cut seen ({:.0f}% of the picture distrusted), histories reset",
                 result.distrustedShare * 100.0f);
    }

    if (result.submitted)
    {
        ++g_frame;
        g_status = Status::Running;
    }

    return result.submitted ? g_source.ProcessedPicture() : nullptr;
}

} // namespace

namespace NativeMotionDx11
{

void OnPresent(IDXGISwapChain* swapChain, ID3D11Device* device)
{
    RunFrame(swapChain, device);
}

ID3D12Resource* OnFGPresent(IDXGISwapChain* real, ID3D11Device* device)
{
    // The one call site that closes the D3D11 depth finder's frame on this path; MenuOverlayDx::Present, which frame
    // generation's present still reaches, skips its own close under Dx11wDx12SC so this frame is not closed twice.
    GenericDepthDx11::OnPresent(real);
    return RunFrame(real, device);
}

void DrawStatus()
{
    if (Config::Instance()->DlssNrNativeUpscaler.value_or_default())
    {
        if (g_nativeRan && g_virtualUpscaler != nullptr && g_virtualUpscaler->Active())
            ImGui::TextDisabled("%s is running on this picture.", g_virtualUpscaler->BackendName().c_str());
        else if (g_virtualUpscaler != nullptr && !g_virtualUpscaler->Error().empty())
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "Upscaler: %s", g_virtualUpscaler->Error().c_str());
        else
            ImGui::TextDisabled("Upscaler: waiting for the first frame with depth.");
    }

    switch (g_status)
    {
    case Status::Off:
        ImGui::TextDisabled("Off.");
        break;
    case Status::Waiting:
        ImGui::TextDisabled("Waiting: the game is calling an upscaler.");
        break;
    case Status::Failed:
        ImGui::TextDisabled("Could not start: %s", g_failure.c_str());
        break;
    case Status::Running:
        ImGui::TextDisabled("Running (%llu frames).", (unsigned long long) g_frame);
        break;
    }

    const bool trustRecent = g_trustFrame != 0 && g_frame - g_trustFrame < 30;

    if (g_status == Status::Running)
        ImGui::TextDisabled("Trust: %s (%llu cuts seen).", trustRecent ? "running" : "waiting for the depth finder",
                            (unsigned long long) g_cuts);

    if (g_status == Status::Running)
        ImGui::TextDisabled("%s", g_nativeRan ? "Native input: NR is running on this picture."
                                              : DlssNr::FinishedPictureStatus().c_str());
}

void DrawAdvancedUi()
{
    auto* config = Config::Instance();

    bool on = config->DlssNrNativeMotion.value_or_default();

    if (ImGui::Checkbox("Estimate motion of the picture##nativemotion11", &on))
        config->DlssNrNativeMotion = on;

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Also set by the single checkbox above. On its own, with neither checkbox below on, this\n"
                                "estimates the motion but feeds nothing with it. Applies at once.");

    bool feed = config->DlssNrNativeInput.value_or_default();

    if (ImGui::Checkbox("Run Neural Rendering on this (native input)##nativeinput11", &feed))
        config->DlssNrNativeInput = feed;

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Same as the DirectX 12 native input, for a Direct3D 11 game: the picture and depth are shared\n"
                                "to a private D3D12 device, processed there, and shared back. Needs the D3D11 depth finder,\n"
                                "Finished picture and Enable Neural Rendering on. No live preview yet. Does nothing with\n"
                                "FGInput=Upscaler selected (frame generation replaces this game's swap chain; use the option\n"
                                "below instead). Lower GPU cost than it, with no frame generation: the single checkbox\n"
                                "above does not use this. Applies at once.");

    bool virtualUpscaler = config->DlssNrNativeUpscaler.value_or_default();

    if (ImGui::Checkbox("Present this to OptiScaler as an upscaler (experimental)##nativeupscaler11", &virtualUpscaler))
        config->DlssNrNativeUpscaler = virtualUpscaler;

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "%s", "Experimental. Instead of feeding DLSS-NR directly, presents the depth finder's depth and the\n"
                  "estimated motion to OptiScaler's upscaler (the one chosen in the menu, FSR when none is) as if\n"
                  "the game had called it. Render size equals output size and jitter is zero, so it works as a\n"
                  "stabiliser, not a reconstruction; it makes frame generation with the Upscaler input work in a\n"
                  "D3D11 game with no upscaler, including with FGInput=Upscaler selected (unlike the checkbox\n"
                  "above, this one is read from Dx11wDx12SC::Present when that applies). Takes priority over Run\n"
                  "Neural Rendering on this. Also set by the single checkbox above. Applies at once.");
}

} // namespace NativeMotionDx11
