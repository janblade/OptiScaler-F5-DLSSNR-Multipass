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

// The mode with frame generation is chosen, which Vulkan cannot run.
bool g_frameGenerationMode = false;

} // namespace

namespace NativeMotionVk
{

void OnPresent(VkQueue queue, VkPresentInfoKHR* present, VkDevice device, VkPhysicalDevice physical)
{
    // A dxvk game presents its D3D frames through here as well; those belong to the D3D drivers.
    if (IdentifyGpu::getPrimaryGpu().usesDxvk)
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
        // Vulkan swapchain made without the bridge the mode stands aside (the bridge is made when the game starts).
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

        if (VkPresentBridge::CopyToOutput(g_source.BridgedPicture(), g_source.BridgedFence(), g_source.BridgedCopied(),
                                          producerDone.fence, producerDone.value, done))
            g_source.NoteBridgeDone(done);
    }

    // Why a frame could not be handed over, once each time the reason changes.
    static std::string lastError;

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

void OnDeviceDestroyed(VkDevice device)
{
    std::lock_guard lock(g_runMutex);
    g_source.OnDeviceDestroyed(device);
}

void DrawStatus()
{
    if (g_frameGenerationMode)
    {
        ImGui::TextDisabled("NR + upscaler & frame generation does not run on Vulkan. Choose NR only.");
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
