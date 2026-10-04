#include "pch.h"

#include "NativeMotionDx11.h"

#include <native/Dx11FrameSource.h>
#include <native/NativeProducer.h>

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
uint64_t g_frame = 0;
bool g_trustRan = false;
bool g_nativeRan = false;
uint64_t g_trustFrame = 0;
uint64_t g_cuts = 0;
Status g_status = Status::Off;
std::string g_failure;

} // namespace

namespace NativeMotionDx11
{

void OnPresent(IDXGISwapChain* swapChain, ID3D11Device* device)
{
    if (!Config::Instance()->DlssNrNativeMotion.value_or_default())
    {
        g_status = Status::Off;
        return;
    }

    if (swapChain == nullptr || device == nullptr || g_status == Status::Failed)
        return;

    g_source.SetPresent(swapChain, device);

    native::FrameInput input;
    const auto acquired = g_source.Acquire(input);

    if (acquired == native::AcquireStatus::WaitingForUpscaler)
    {
        g_status = Status::Waiting;

        if (g_producer)
            g_producer->Reset();

        return;
    }

    if (acquired != native::AcquireStatus::Ready)
        return;

    if (g_producer == nullptr || g_producer->Device() != g_source.Device12())
    {
        auto fresh = std::make_unique<native::NativeProducer>();

        if (!fresh->Init(g_source.Device12()))
        {
            g_failure = fresh->Error();
            g_status = Status::Failed;
            LOG_ERROR("Native motion (D3D11): {}", g_failure);
            g_source.Return(input, native::FrameOutput {});
            return;
        }

        g_producer = std::move(fresh);
        LOG_INFO("Native motion (D3D11): optical flow and trust mask ready, on a private D3D12 device paired with "
                 "the game's D3D11 device");
    }

    native::NativeProducer::Options options;
    options.applyNr = Config::Instance()->DlssNrNativeInput.value_or_default();

    const auto applyNr = [](ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, ID3D12Resource* depth,
                            ID3D12Resource* motion, bool reversed, bool reset, native::ColorSpace space,
                            D3D12_RESOURCE_STATES state)
    {
        const DXGI_COLOR_SPACE_TYPE type = space == native::ColorSpace::ScRgb ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                           : space == native::ColorSpace::Pq  ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                                                              : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        return DlssNr::ApplyNativeInput(g_source.Queue12(), cmd, color, depth, motion, reversed, reset, type, state);
    };

    native::FrameOutput output;
    const auto result = g_producer->Run(g_source.Queue12(), input, options, applyNr, output);
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
}

void DrawDebugUi()
{
    auto* config = Config::Instance();

    if (!ImGui::TreeNode("Motion estimate, D3D11 (experimental)##nativemotion11"))
        return;

    bool on = config->DlssNrNativeMotion.value_or_default();

    if (ImGui::Checkbox("Estimate motion of the picture##nativemotion11", &on))
        config->DlssNrNativeMotion = on;

    bool feed = config->DlssNrNativeInput.value_or_default();

    if (ImGui::Checkbox("Run Neural Rendering on this (native input)##nativeinput11", &feed))
        config->DlssNrNativeInput = feed;

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", "Same as the DirectX 12 native input, for a Direct3D 11 game: the picture and depth are shared\n"
                                "to a private D3D12 device, processed there, and shared back. Needs the D3D11 depth finder,\n"
                                "Finished picture and Enable Neural Rendering on. No live preview yet. Applies at once.");

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

    ImGui::TreePop();
}

} // namespace NativeMotionDx11
