#include "pch.h"

#include "NativeDriverDx11.h"

#include <native/DebugPreviewDx11.h>
#include <native/Dx11FrameSource.h>
#include <native/FlowDiagnosticUi.h>
#include <native/NativeDriver.h>
#include <native/VkPresentBridge.h>

#include <Config.h>
#include <State.h>
#include <misc/IdentifyGpu.h>
#include <dlssnr/DlssNr_ExposureScan_Dx11.h>
#include <resource_tracking/GenericDepth_Dx11.h>

#include <imgui/imgui.h>

#include <cstring>

// The D3D11 counterpart of native/NativeDriverDx12.cpp: the same shared frame step (native::NativeDriver), a different
// adapter (native::Dx11FrameSource, the shared-texture transport). The debug view is a D3D12 texture; the D3D11 menu
// (ImGui_ImplDX11) shows a shared copy of it (native/DebugPreviewDx11.h). RunFrame and the menu run on the present
// thread, one after the other (menu/menu_overlay_dx.cpp's RenderImGui_DX11), so they need no lock. Under frame
// generation's D3D12 swap chain the menu draws with D3D12 on another device: no picture there.

namespace
{

native::NativeDriver g_driver("Native motion (D3D11)",
                              ", on a private D3D12 device paired with the game's D3D11 device");
native::Dx11FrameSource g_source;
native::FlowDiagnosticUi g_ui;
native::DebugPreviewDx11 g_preview;

// The debug view's picture for the D3D11 menu, or 0 (the menu draws with another API, or the copy could not be made).
ImTextureID FlowTexture(ID3D12Resource* texture)
{
    const char* renderer = ImGui::GetIO().BackendRendererName;

    if (renderer == nullptr || std::strcmp(renderer, "imgui_impl_dx11") != 0)
        return 0;

    return (ImTextureID) g_preview.Export(g_source.Queue(), texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

// Shared by OnPresent and OnFGPresent; the depth finder's frame close is the caller's job (see the two entry points
// below), since only one of them needs to do it. Returns the D3D12 picture the producer ended up with this frame, or
// null when nothing ran.
ID3D12Resource* RunFrame(IDXGISwapChain* swapChain, ID3D11Device* device, bool copyBack)
{
    if (!g_driver.Enabled())
        return nullptr;

    if (swapChain == nullptr || device == nullptr || g_driver.Failed())
        return nullptr;

    g_source.SetPresent(swapChain, device, copyBack);

    // While Dx11wDx12SC has replaced the game's swap chain for frame generation this runs from OnFGPresent only, and the
    // bridge copies the result into frame generation's back buffer.
    return g_driver.RunFrame(g_source, g_ui.TakeRequest(), g_ui.View()).submitted ? g_source.ProcessedPicture()
                                                                                  : nullptr;
}

} // namespace

namespace NativeMotionDx11
{

void OnPresent(IDXGISwapChain* swapChain, ID3D11Device* device)
{
    // DlssNrNativeDxvkVulkan: on a dxvk game this driver's shared-texture bridge stands aside for the Vulkan one
    // (NativeDriverVk.cpp), which reads dxvk's own Vulkan calls instead. OnFGPresent needs no such check: with the key on
    // a dxvk game never gets Dx11wDx12SC (hooks/DxgiFactory_Hooks.cpp); its frame generation is the Vulkan present bridge.
    if (VkPresentBridge::DxvkThroughVulkan())
        return;

    // Under Dx11wDx12SC the frame is OnFGPresent's. Frame generation's own presents still reach the overlay, which
    // calls here: a second run in the same frame fed NR and frame generation twice per frame (GTA V, 2026-10-21: half
    // the frame rate, the native step's inputs alternating between two pictures, a scene cut every few frames,
    // blinking).
    if (State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12)
        return;

    RunFrame(swapChain, device, true);
}

ID3D12Resource* OnFGPresent(IDXGISwapChain* real, ID3D11Device* device)
{
    // The one call site that closes the D3D11 depth finder's frame on this path; MenuOverlayDx::Present, which frame
    // generation's present still reaches, skips its own close under Dx11wDx12SC so this frame is not closed twice.
    GenericDepthDx11::OnPresent(real);
    DlssNr::ExposureScanDx11::OnPresent(real);
    // The bridge copies the D3D12 result into frame generation's back buffer itself: no copy back to D3D11
    return RunFrame(real, device, false);
}

void DrawStatus() { g_driver.DrawStatus(); }

bool NrOnlyRunning() { return g_driver.NrOnlyRunning(); }

void DrawAdvancedUi()
{
    g_driver.DrawFlowTuning();

    const bool dx11Menu = ImGui::GetIO().BackendRendererName != nullptr &&
                          std::strcmp(ImGui::GetIO().BackendRendererName, "imgui_impl_dx11") == 0;
    g_ui.DrawAdvanced(g_driver, FlowTexture,
                      dx11Menu ? nullptr
                               : "The debug view's picture is not shown while frame generation draws the menu.");

    if (dx11Menu && Config::Instance()->DlssNrNativeDebugView.value_or_default() && !g_preview.Error().empty())
        ImGui::TextDisabled("%s", g_preview.Error().c_str());

    if (!Config::Instance()->DlssNrNativeDebugView.value_or_default() ||
        g_driver.GetStatus() != native::NativeDriver::Status::Running)
        return;

    ImGui::TextDisabled("Running (%llu frames). Trust: %s (%llu cuts seen).", (unsigned long long) g_driver.Frame(),
                        g_driver.TrustRecent() ? "running" : "waiting for the motion estimate",
                        (unsigned long long) g_driver.Cuts());
}

bool ExpandedPreviewVisible() { return g_ui.Expanded(); }

void DrawExpandedPreview()
{
    const char* renderer = ImGui::GetIO().BackendRendererName;
    g_ui.DrawExpanded(g_driver, FlowTexture, renderer != nullptr && std::strcmp(renderer, "imgui_impl_dx11") == 0);
}

void BeginDebugFrame(ID3D11Device* device) { g_preview.BeginFrame(device); }

void EndDebugFrame() { g_preview.EndFrame(); }

void ReleaseDebugViews() { g_preview.Reset(); }

} // namespace NativeMotionDx11
