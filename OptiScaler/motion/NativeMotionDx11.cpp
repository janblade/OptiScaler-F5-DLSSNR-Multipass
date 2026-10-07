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

// The D3D11 counterpart of motion/NativeMotion_Dx12.cpp: same producer, a different adapter (native::Dx11FrameSource,
// the shared-texture transport). No live flow/trust picture here yet: they are D3D12 textures and the D3D11 game's menu
// renders with ImGui_ImplDX11, which needs a D3D11 shader-resource view; showing them would need another shared-texture
// round trip, not done for Story B's first game. The status line works the same as the D3D12 driver's.

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

    // A line every 300 presents saying where the frames go, so a "waiting for the motion estimate" that never ends can
    // be told apart: no picture (Acquire not ready), flow never valid, trust mask not running, or the backend not
    // applying.
    static uint64_t diagPresents = 0, diagReady = 0, diagWaiting = 0, diagFlowValid = 0, diagTrust = 0, diagNative = 0,
                    diagSubmitted = 0;
    ++diagPresents;
    diagReady += acquired == native::AcquireStatus::Ready ? 1 : 0;
    diagWaiting += acquired == native::AcquireStatus::WaitingForUpscaler ? 1 : 0;

    const auto logDiag = [&]()
    {
        if (diagPresents % 300 != 0)
            return;

        LOG_INFO(
            "Native motion (D3D11) frames: of {} presents, picture ready {}, waiting for the game's upscaler {}, "
            "submitted {}, flow valid {}, trust mask ran {}, native input applied {}, scene cuts {}, virtual upscaler "
            "{}",
            diagPresents, diagReady, diagWaiting, diagSubmitted, diagFlowValid, diagTrust, diagNative, g_cuts,
            g_virtualUpscaler != nullptr && g_virtualUpscaler->Active() ? "active" : "not active");
    };

    if (acquired == native::AcquireStatus::WaitingForUpscaler)
    {
        // The game's own upscaler call takes over: ours goes, as a game's feature would.
        ReleaseVirtualUpscaler();
        g_status = Status::Waiting;

        if (g_producer)
            g_producer->Reset();

        logDiag();
        return nullptr;
    }

    if (acquired != native::AcquireStatus::Ready)
    {
        logDiag();
        return nullptr;
    }

    if (g_producer == nullptr || g_producer->Device() != g_source.Device())
    {
        auto fresh = std::make_unique<native::NativeProducer>();

        if (!fresh->Init(g_source.Device()))
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
    // priority when both are on. Inert (same as native input and Finished Picture) while a D3D11 game's swap chain has
    // been replaced by Dx11wDx12SC for frame generation -- see OnFGPresent for that path.
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
        return DlssNr::ApplyNativeInput(g_source.Queue(), cmd, frame.color, frame.depth, frame.motion,
                                        frame.depthReversed, frame.reset, type, frame.pictureState);
    };

    native::FrameOutput output;
    const auto result = g_producer->Run(g_source.Queue(), input, options, apply, output);
    g_source.Return(input, output);

    if (result.stoppedAt != nullptr)
    {
        static uint64_t stopped = 0;
        ++stopped;

        if (stopped == 1 || stopped == 100 || stopped % 1000 == 0)
            LOG_WARN(
                "Native motion (D3D11): the producer stopped at \"{}\" (HRESULT 0x{:X}, device removed reason 0x{:X}), "
                "{} frames so far",
                result.stoppedAt, (unsigned) result.stoppedHr, (unsigned) result.deviceRemoved, stopped);
    }

    diagFlowValid += result.flowValid ? 1 : 0;
    diagTrust += result.trustRan ? 1 : 0;
    diagNative += result.nativeRan ? 1 : 0;
    diagSubmitted += result.submitted ? 1 : 0;

    logDiag();

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
    switch (g_status)
    {
    case Status::Off:
        ImGui::TextDisabled("Waiting for the first frame.");
        return;
    case Status::Waiting:
        ImGui::TextDisabled("Standing aside: the game is calling an upscaler.");
        return;
    case Status::Failed:
        ImGui::TextDisabled("Could not start: %s", g_failure.c_str());
        return;
    case Status::Running:
        break;
    }

    if (Config::Instance()->DlssNrNativeUpscaler.value_or_default())
    {
        if (g_nativeRan && g_virtualUpscaler != nullptr && g_virtualUpscaler->Active())
            ImGui::TextDisabled("%s is running as a stabiliser on this picture.",
                                g_virtualUpscaler->BackendName().c_str());
        else if (g_virtualUpscaler != nullptr && !g_virtualUpscaler->Error().empty())
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "Stabiliser: %s", g_virtualUpscaler->Error().c_str());
        else
            ImGui::TextDisabled("Stabiliser: waiting for the motion estimate.");
    }
    else if (g_nativeRan)
    {
        ImGui::TextDisabled("NR running on this picture.");
    }
    else
    {
        const std::string reason = DlssNr::FinishedPictureStatus();
        ImGui::TextDisabled("%s", reason.empty() ? "NR: waiting for the motion estimate." : reason.c_str());
    }
}

void DrawAdvancedUi()
{
    if (!Config::Instance()->DlssNrNativeDebugView.value_or_default() || g_status != Status::Running)
        return;

    const bool trustRecent = g_trustFrame != 0 && g_frame - g_trustFrame < 30;
    ImGui::TextDisabled("Running (%llu frames). Trust: %s (%llu cuts seen).", (unsigned long long) g_frame,
                        trustRecent ? "running" : "waiting for the motion estimate", (unsigned long long) g_cuts);
}

} // namespace NativeMotionDx11
