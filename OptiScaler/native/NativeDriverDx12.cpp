#include "pch.h"
#include "FlowDebugViews.h"

#include "NativeDriverDx12.h"
#include "NativeDebugDevice.h"

#include <native/Dx12FrameSource.h>
#include <native/NativeDriver.h>

#include <Config.h>

#include <menu/menu_overlay_dx.h>
#include <menu/menu_overlay_base.h>
#include <resource_tracking/GenericDepth_Dx12.h>

#include <imgui/imgui.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

namespace
{

native::NativeDriver g_driver("Native motion", "");
native::Dx12FrameSource g_source;

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

std::atomic<bool> g_previewWanted { false }; // published by the menu, consumed by the producer thread
int g_flowDebugView = 0; // default to the intuitive arrows-only diagnostic
std::atomic<int> g_requestedDebugView { 7 };
int g_flowPreviewDisplay = 0; // 0 inline only, 1 enlarged window, 2 fullscreen overlay

constexpr auto& kFlowDebugViews=native::FlowDebugViews::names;

std::unique_lock<std::mutex> g_debugFrameLock;
bool g_debugFrameSampled = false;

bool TextureHandle(ID3D12Resource* texture, DXGI_FORMAT format, bool grayFromRed,
                   D3D12_GPU_DESCRIPTOR_HANDLE& gpu)
{
    if (!g_debugFrameLock.owns_lock() || texture == nullptr)
        return false;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = grayFromRed ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1) : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    if (!MenuOverlayDx::FrameTextureView(texture, srv, gpu))
        return false;
    g_debugFrameSampled = true;
    return true;
}

bool ShowTexture(ID3D12Resource* texture, DXGI_FORMAT format, bool grayFromRed, float width, float height)
{
    D3D12_GPU_DESCRIPTOR_HANDLE gpu {};
    if (width <= 0.0f || height <= 0.0f || !TextureHandle(texture, format, grayFromRed, gpu))
        return false;
    ImGui::Image((ImTextureID) gpu.ptr, ImVec2(width, height));
    return true;
}

void DrawExpandedFlowPreview()
{
    if (g_flowPreviewDisplay == 0)
        return;

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const bool fullscreen = g_flowPreviewDisplay == 2;
    auto* producer = g_debugFrameLock.owns_lock() ? g_driver.Producer() : nullptr;
    const bool ready = producer && g_driver.GetStatus() == native::NativeDriver::Status::Running &&
                       producer->PreviewReady();
    ID3D12Resource* texture = ready ? producer->Flow()->Preview() : nullptr;
    bool open = true;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings;

    if (fullscreen)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE gpu {};
        if (TextureHandle(texture, DXGI_FORMAT_R8G8B8A8_UNORM, false, gpu))
        {
            // Cover the exact game viewport. Window padding and toolbar height must not
            // shift/stretch motion arrows relative to the objects underneath them.
            ImGui::GetBackgroundDrawList(viewport)->AddImage((ImTextureID) gpu.ptr, viewport->Pos,
                ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y));
        }
        if (MenuOverlayBase::IsVisible())
        {
            ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 16.0f, viewport->WorkPos.y + 16.0f), ImGuiCond_Always);
            if (ImGui::Begin("F5Low preview controls##f5flowfullscreen", nullptr,
                flags | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextUnformatted(kFlowDebugViews[std::clamp(g_flowDebugView, 0, 6)]);
                ImGui::TextWrapped("%s",native::FlowDebugViews::Legend(g_flowDebugView));
                if (ImGui::Button("Exit fullscreen preview")) g_flowPreviewDisplay = 0;
            }
            ImGui::End();
        }
        return;
    }

    const ImVec2 size(viewport->WorkSize.x * 0.82f, viewport->WorkSize.y * 0.82f);
    const ImVec2 position(viewport->WorkPos.x + (viewport->WorkSize.x - size.x) * 0.5f,
                          viewport->WorkPos.y + (viewport->WorkSize.y - size.y) * 0.5f);
    ImGui::SetNextWindowPos(position, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (ImGui::Begin("F5Low enlarged diagnostic##f5flowexpanded", &open, flags))
    {
        ImGui::TextUnformatted(kFlowDebugViews[std::clamp(g_flowDebugView, 0, 6)]);
                ImGui::TextWrapped("%s",native::FlowDebugViews::Legend(g_flowDebugView));
        if (texture)
        {
            const D3D12_RESOURCE_DESC desc = texture->GetDesc();
            const ImVec2 available = ImGui::GetContentRegionAvail();
            const float aspect = desc.Height > 0 ? (float) desc.Width / (float) desc.Height : 16.0f / 9.0f;
            const float width = std::min(available.x, available.y * aspect);
            const float height = aspect > 0.0f ? width / aspect : available.y;
            if (width > 0.0f && height > 0.0f)
            {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available.x - width) * 0.5f);
                ShowTexture(texture, DXGI_FORMAT_R8G8B8A8_UNORM, false, width, height);
            }
        }
        else
            ImGui::TextDisabled("Waiting for the F5Low diagnostic frame.");
    }

    ImGui::End();

    if (!open)
        g_flowPreviewDisplay = 0;
}

void RunFrame(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, ID3D12Device* device)
{
    if (!g_driver.Enabled())
        return;

    if (swapChain == nullptr || queue == nullptr || device == nullptr || g_driver.Failed())
        return;

    g_source.SetPresent(swapChain, queue, device);

    const bool requested = g_previewWanted.exchange(false, std::memory_order_acq_rel);
    const bool preview = Config::Instance()->DlssNrNativeDebugView.value_or_default() && requested;
    g_driver.RunFrame(g_source, preview, g_requestedDebugView.load(std::memory_order_acquire));
}

} // namespace

namespace NativeMotionDx12
{

bool WaitForPendingFrame()
{
    std::unique_lock lock(g_runMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    auto* producer = g_driver.Producer();
    if (producer && !producer->Drain())
    {
        LOG_WARN("F5Low GPU work still pending; postponing swapchain resize");
        return false;
    }
    if (producer) producer->Reset();
    g_source.OnResize();
    return true;
}

void BeginDebugFrame(ID3D12CommandQueue* queue, ID3D12Fence* menuFence)
{
    g_debugFrameSampled = false;
    if (!Config::Instance()->DlssNrNativeDebugView.value_or_default() || queue == nullptr)
        return;
    g_debugFrameLock = std::unique_lock(g_runMutex, std::try_to_lock);
    if (!g_debugFrameLock.owns_lock())
        return; // FG's presenter must never block on the game's thread.
    auto* producer = g_driver.Producer();
    if (producer == nullptr)
        return;
    ID3D12Device* queueDevice = nullptr;
    const HRESULT deviceHr = queue->GetDevice(IID_PPV_ARGS(&queueDevice));
    const auto ready = producer->LastSubmission();
    const bool sameDevice = SUCCEEDED(deviceHr) &&
                            native::SameDebugDevice(queueDevice, producer->Device(), ready.fence, menuFence);
    if (queueDevice) queueDevice->Release();
    if (!sameDevice || (ready.fence && FAILED(queue->Wait(ready.fence, ready.value))))
        g_debugFrameLock.unlock();
}

void EndDebugFrame(ID3D12Fence* fence, UINT64 value)
{
    if (!g_debugFrameLock.owns_lock())
        return;
    if (g_debugFrameSampled && fence && g_driver.Producer())
        g_driver.Producer()->PreviewReadSubmitted(fence, value);
    g_debugFrameLock.unlock();
}

bool ExpandedPreviewVisible()
{
    return g_flowPreviewDisplay != 0 && Config::Instance()->DlssNrNativeMotion.value_or_default() &&
           Config::Instance()->DlssNrNativeDebugView.value_or_default();
}

void DrawExpandedPreview()
{
    if (!ExpandedPreviewVisible())
    {
        g_flowPreviewDisplay = 0;
        return;
    }
    g_previewWanted.store(true, std::memory_order_release);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        g_flowPreviewDisplay = 0;
    DrawExpandedFlowPreview();
}

void OnPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue, ID3D12Device* device)
{
    // A D3D11 game's frame generation swap chain is the interop's D3D12 one: its picture is the game's, already handled by
    // NativeMotionDx11 from Dx11wDx12SC::Present, and this present runs inside frame generation's own present, under the
    // lock the virtual upscaler's UpscaleStart (-> EvaluateState) takes again: that deadlocked.
    if (State::Instance().swapchainInteropApi != SwapchainInteropApi::None)
        return;

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

void DrawStatus()
{
    std::unique_lock lock(g_runMutex, std::defer_lock);
    if (!g_debugFrameLock.owns_lock() && !lock.try_lock())
    {
        ImGui::TextDisabled("Motion processing in progress.");
        return;
    }
    g_driver.DrawStatus();
}

bool NrOnlyRunning()
{
    if (g_debugFrameLock.owns_lock()) return g_driver.NrOnlyRunning();
    std::unique_lock lock(g_runMutex, std::try_to_lock);
    return lock.owns_lock() && g_driver.NrOnlyRunning();
}

void DrawAdvancedUi()
{
    const bool debugView = Config::Instance()->DlssNrNativeDebugView.value_or_default();

    std::unique_lock lock(g_runMutex, std::defer_lock);
    if (!g_debugFrameLock.owns_lock() && !lock.try_lock())
    {
        ImGui::TextDisabled("Motion processing in progress.");
        return;
    }
    g_driver.DrawFlowTuning();

    // The pictures and their controls are debugging aids: off by default, nothing is recorded for them while they are hidden.
    if (!debugView)
    {
        g_previewWanted.store(false, std::memory_order_release);
        g_flowPreviewDisplay = 0;
        return;
    }

    const float boxWidth = 360.0f;
    const float boxHeight = boxWidth * 9.0f / 16.0f;
    bool drawn = false;

    ImGui::SetNextItemWidth(240.0f);
    ImGui::Combo("F5Low diagnostic view##f5flowdebug", &g_flowDebugView, kFlowDebugViews,
                 (int)(sizeof(kFlowDebugViews) / sizeof(kFlowDebugViews[0])));
    g_requestedDebugView.store(native::FlowDebugViews::Mode(g_flowDebugView), std::memory_order_release);
    ImGui::TextWrapped("%s",native::FlowDebugViews::Legend(g_flowDebugView));

    if (ImGui::Button("Enlarge preview"))
        g_flowPreviewDisplay = 1;
    ImGui::SameLine();
    if (ImGui::Button("Fullscreen preview"))
        g_flowPreviewDisplay = 2;

    if (g_flowDebugView == 0)
    {
        ImGui::TextDisabled("Arrows point with screen motion; hue shows direction, length shows speed (capped). Units: game pixels/frame.");
        if (g_driver.Producer() && ImGui::TreeNode("Arrow appearance##f5lowarrows"))
        {
            auto& arrows = g_driver.Producer()->Flow()->DebugTuning();
            ImGui::SliderFloat("Grid spacing (px)##arrowgrid", &arrows.spacing, 16.0f, 128.0f, "%.0f");
            ImGui::SliderFloat("Length gain##arrowgain", &arrows.gain, 0.25f, 8.0f, "%.2f");
            ImGui::SliderFloat("Minimum motion (px/frame)##arrowminimum", &arrows.minimumMotion, 0.05f, 4.0f, "%.2f");
            ImGui::SliderFloat("Line width##arrowwidth", &arrows.thickness, 0.5f, 3.0f, "%.2f");
            ImGui::SliderFloat("Arrow opacity##arrowopacity", &arrows.opacity, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Grid opacity##arrowgridopacity", &arrows.gridOpacity, 0.0f, 0.5f, "%.2f");
            ImGui::Checkbox("Show captured game picture##arrowbackground", &arrows.background);
            if (arrows.background) ImGui::TextDisabled("Picture is captured at the flow input, before NR. Disable for transparent arrows over the final game picture.");
            if (ImGui::Button("Reset arrow appearance##arrowdefaults")) arrows = OpticalFlowDx12::DebugSettings {};
            ImGui::TreePop();
        }
    }

    if (g_driver.GetStatus() == native::NativeDriver::Status::Running)
    {
        g_previewWanted = true;

        if (g_driver.Producer() && g_driver.Producer()->PreviewReady())
            drawn = ShowTexture(g_driver.Producer()->Flow()->Preview(), DXGI_FORMAT_R8G8B8A8_UNORM, false, boxWidth, boxHeight);
    }

    if (!drawn)
        ImGui::Dummy(ImVec2(boxWidth, boxHeight));


}

} // namespace NativeMotionDx12
