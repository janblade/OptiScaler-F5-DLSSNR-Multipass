#include "pch.h"

#include "NativeDriverDx11.h"

#include <native/Dx11FrameSource.h>
#include <native/NativeDriver.h>

#include <Config.h>
#include <resource_tracking/GenericDepth_Dx11.h>

#include <imgui/imgui.h>

// The D3D11 counterpart of native/NativeDriverDx12.cpp: the same shared frame step (native::NativeDriver), a different
// adapter (native::Dx11FrameSource, the shared-texture transport). No live flow/trust picture here yet: they are D3D12
// textures and the D3D11 game's menu renders with ImGui_ImplDX11, which needs a D3D11 shader-resource view; showing
// them would need another shared-texture round trip, not done for Story B's first game. The status line and the flow
// tuning work the same as the D3D12 driver's.

namespace
{

native::NativeDriver g_driver("Native motion (D3D11)",
                              ", on a private D3D12 device paired with the game's D3D11 device");
native::Dx11FrameSource g_source;

// Shared by OnPresent and OnFGPresent; the depth finder's frame close is the caller's job (see the two entry points
// below), since only one of them needs to do it. Returns the D3D12 picture the producer ended up with this frame, or
// null when nothing ran.
ID3D12Resource* RunFrame(IDXGISwapChain* swapChain, ID3D11Device* device)
{
    if (!g_driver.Enabled())
        return nullptr;

    if (swapChain == nullptr || device == nullptr || g_driver.Failed())
        return nullptr;

    g_source.SetPresent(swapChain, device);

    // The consumer is inert (same as native input and Finished Picture) while a D3D11 game's swap chain has been
    // replaced by Dx11wDx12SC for frame generation -- see OnFGPresent for that path.
    return g_driver.RunFrame(g_source, false).submitted ? g_source.ProcessedPicture() : nullptr;
}

} // namespace

namespace NativeMotionDx11
{

void OnPresent(IDXGISwapChain* swapChain, ID3D11Device* device) { RunFrame(swapChain, device); }

ID3D12Resource* OnFGPresent(IDXGISwapChain* real, ID3D11Device* device)
{
    // The one call site that closes the D3D11 depth finder's frame on this path; MenuOverlayDx::Present, which frame
    // generation's present still reaches, skips its own close under Dx11wDx12SC so this frame is not closed twice.
    GenericDepthDx11::OnPresent(real);
    return RunFrame(real, device);
}

void DrawStatus() { g_driver.DrawStatus(); }

void DrawAdvancedUi()
{
    g_driver.DrawFlowTuning();

    if (!Config::Instance()->DlssNrNativeDebugView.value_or_default() ||
        g_driver.GetStatus() != native::NativeDriver::Status::Running)
        return;

    ImGui::TextDisabled("Running (%llu frames). Trust: %s (%llu cuts seen).", (unsigned long long) g_driver.Frame(),
                        g_driver.TrustRecent() ? "running" : "waiting for the motion estimate",
                        (unsigned long long) g_driver.Cuts());
}

} // namespace NativeMotionDx11
