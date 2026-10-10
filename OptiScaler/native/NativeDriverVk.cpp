#include "pch.h"

#include "NativeDriverVk.h"

#include <native/NativeDriver.h>
#include <native/VkFrameSource.h>
#include <native/VkPresentBridge.h>

#include <Config.h>
#include <misc/IdentifyGpu.h>
#include <resource_tracking/GenericDepth_Vk.h>

#include <imgui/imgui.h>

#include <mutex>

// The Vulkan counterpart of native/NativeDriverDx11.cpp: the same shared frame step (native::NativeDriver), a different
// adapter (native::VkFrameSource). No flow/trust pictures: they are D3D12 textures and the Vulkan menu renders with
// ImGui_ImplVulkan. The status line and the flow tuning work the same as on the other APIs.

namespace
{

native::NativeDriver g_driver("Native motion (Vulkan)", ", on a private D3D12 device on the Vulkan device's adapter");
native::VkFrameSource g_source;
std::mutex g_runMutex;

// What the present waits on once the frame was processed: VkPresentInfoKHR keeps a pointer to it.
VkSemaphore g_presentWait = VK_NULL_HANDLE;

// The mode with frame generation is chosen but the game's swapchain was made without the bridge.
bool g_frameGenerationMode = false;

// Why the frame source left the present's Vulkan device alone (a second device the game made), for the menu. Copied out of
// the frame source under the run mutex; the menu thread reads it under its own.
std::mutex g_refusalMutex;
std::string g_refusal;

} // namespace

namespace NativeMotionVk
{

void OnPresent(VkQueue queue, VkPresentInfoKHR* present, VkDevice device, VkPhysicalDevice physical)
{
    // A dxvk game presents its D3D frames through here as well; those belong to the D3D drivers, unless the
    // experimental DlssNrNativeDxvkVulkan key asks this driver to read dxvk's own Vulkan calls instead
    // (NativeDriverDx11.cpp::OnPresent stands aside in that case).
    if (VkPresentBridge::DxvkGame() && !VkPresentBridge::DxvkThroughVulkan())
        return;

    // Whenever the D3D11 bridge (with_dx12/dx11_with_dx12_sc.h, NativeMotionDx11::OnFGPresent) owns the frame, this
    // driver stays out of its way, the invariant NativeDriverDx11.h describes ("only one of the two drivers' OnPresent is
    // ever fed real work"). With DlssNrNativeDxvkVulkan on it is never made for a dxvk game: that game's frame generation
    // is the Vulkan present bridge, as for a native Vulkan game (hooks/DxgiFactory_Hooks.cpp).
    if (State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12)
        return;

    if (present == nullptr || present->swapchainCount != 1 || queue == VK_NULL_HANDLE || device == VK_NULL_HANDLE ||
        physical == VK_NULL_HANDLE)
        return;

    std::unique_lock lock(g_runMutex, std::try_to_lock);

    if (!lock.owns_lock())
        return;

    // The depth finder closes the frame first (whether or not the producer runs, as on D3D12), so the frame source
    // hands over this frame's copy.
    uint32_t width = 0, height = 0;

    if (native::VkFrameSource::SwapchainExtent(present->pSwapchains[0], &width, &height))
        GenericDepthVk::OnPresent(device, width, height);

    // With the Vulkan present bridge up the game presents on a hidden window and this is the only place its picture goes on
    // to the D3D12 swapchain: every present is handled, producer or not.
    const bool bridged = VkPresentBridge::Active(present->pSwapchains[0]);
    g_source.SetBridged(bridged);
    const bool producing = g_driver.Enabled() && !g_driver.Failed();

    if (!bridged)
    {
        if (!producing)
            return;

        // "NR + upscaler & frame generation" presents through OptiScaler's frame generation, which is D3D12's: on a
        // Vulkan swapchain made without the bridge the mode stands aside until the game makes a new one (its present is
        // told the swapchain is out of date, hooks/Vulkan_Hooks.cpp).
        g_frameGenerationMode = Config::Instance()->DlssNrNativeUpscaler.value_or_default();

        if (g_frameGenerationMode)
            return;
    }
    else
    {
        g_frameGenerationMode = false;
    }

    g_source.SetPresent(device, physical, queue, present->pSwapchains[0], present->pImageIndices[0],
                        present->pWaitSemaphores, present->waitSemaphoreCount);

    if (producing)
    {
        g_driver.RunFrame(g_source, false);
    }
    else
    {
        native::FrameInput unused;
        g_source.Acquire(unused);
    }

    if (bridged)
    {
        // The producer did not run (or the game calls an upscaler): the present still needs its semaphore.
        g_source.FinishPresent();

        const uint64_t done = g_source.NextFenceValue();
        const auto producerDone = g_source.BridgedProducerDone();

        bool signalled = VkPresentBridge::CopyToOutput(g_source.BridgedPicture(), g_source.BridgedFence(),
                                                       g_source.BridgedCopied(), producerDone.fence, producerDone.value,
                                                       done);

        // The copy to the output failed and nothing was queued for `done`: the producer may still be reading the shared
        // picture, and the next copy in must wait for it all the same
        if (!signalled)
            signalled = g_source.HandBackBridged(done);

        // Only a value that will be signalled: the next copy in waits for it on the GPU
        if (signalled)
            g_source.NoteBridgeDone(done);
    }

    // Why a frame could not be handed over, once each time the reason changes.
    static std::string lastError;

    {
        std::lock_guard lock(g_refusalMutex);
        g_refusal = g_source.Refusal();
    }

    if (g_source.Error() != lastError)
    {
        lastError = g_source.Error();

        if (!lastError.empty())
            LOG_WARN("Native motion (Vulkan): {}", lastError);
    }

    if (!g_source.TookPresentWaits())
        return;

    g_presentWait = g_source.PresentWait();
    present->waitSemaphoreCount = g_presentWait != VK_NULL_HANDLE ? 1 : 0;
    present->pWaitSemaphores = g_presentWait != VK_NULL_HANDLE ? &g_presentWait : nullptr;
}

void AfterPresent(VkSwapchainKHR swapchain)
{
    if (VkPresentBridge::Active(swapchain))
        VkPresentBridge::PresentOutput();
}

bool AcquireBridgeDevice(VkPhysicalDevice physical, ID3D12Device** device, ID3D12CommandQueue** queue, std::string& why)
{
    if (!g_source.EnsureD3D12(physical, why))
        return false;

    g_source.PinD3D12(true);
    *device = g_source.Device12();
    *queue = g_source.Queue12();
    return true;
}

void ReleaseBridgeDevice()
{
    g_source.PinD3D12(false);
}

ID3D12Device* ShareBridgeDevice(VkPhysicalDevice physical, std::string& why)
{
    // The game's render thread (its upscaler feature being made) while the present thread may be in OnPresent
    std::lock_guard lock(g_runMutex);

    if (!g_source.EnsureD3D12(physical, why))
        return nullptr;

    ID3D12Device* device = g_source.Device12();
    device->AddRef();
    return device;
}

void OnDeviceDestroyed(VkDevice device)
{
    std::lock_guard lock(g_runMutex);
    g_source.OnDeviceDestroyed(device);
}

void DrawStatus()
{
    // The bridge was turned off because its D3D12 swapchain could not present (VkPresentBridge::PresentOutput)
    if (const std::string given = VkPresentBridge::FailureReason(); !given.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f),
                           "Frame generation stopped: %s. The game was asked to present on its own window again. "
                           "Restart the game to try frame generation again.",
                           given.c_str());
        return;
    }

    {
        std::lock_guard lock(g_refusalMutex);

        if (!g_refusal.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "%s", g_refusal.c_str());
            return;
        }
    }

    if (g_frameGenerationMode)
    {
        ImGui::TextDisabled("Waiting for the game to make a new swapchain for frame generation (it was told to). If "
                            "nothing changes, switch the game's window mode or resolution once. OptiFG (Upscaler) and "
                            "an output must be set when the game starts.");
        return;
    }

    g_driver.DrawStatus();
}

bool NrOnlyRunning() { return g_driver.NrOnlyRunning(); }

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

} // namespace NativeMotionVk
