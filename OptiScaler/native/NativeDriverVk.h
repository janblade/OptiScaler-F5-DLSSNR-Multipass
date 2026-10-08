#pragma once

// The native input producer's Vulkan driver: the counterpart of native/NativeDriverDx11.h. Runs native::NativeProducer
// (optical flow, trust mask, DLSS-NR) on a private D3D12 device on the Vulkan device's adapter, through
// native::VkFrameSource (the shared-texture transport).
//
// Switched on with the same [DlssNr] NativeMotion / NativeInput / NativeDebugView keys as the D3D12 and D3D11 drivers.
// Stands aside in a game running on dxvk: its D3D presents are the D3D11/D3D12 drivers' to handle.

#include <vulkan/vulkan.h>

namespace NativeMotionVk
{
// Once per present, from the vkQueuePresentKHR hook, before the menu is drawn. When the frame was processed, `present`'s
// wait list is replaced by one semaphore of ours (its arrays then point into storage that lives until the next call).
void OnPresent(VkQueue queue, VkPresentInfoKHR* present, VkDevice device, VkPhysicalDevice physical);

// The game is destroying `device`: everything made on it goes.
void OnDeviceDestroyed(VkDevice device);

// ImGui: one status line for the mode in effect (waiting/failed, NR or the stabiliser on this picture, or why not).
void DrawStatus();

// NR is running on the finished picture (the menu header's "Optical F5Low: NR on the finished picture").
bool NrOnlyRunning();

// ImGui: with NativeDebugView, the flow tuning, the frame count and the trust mask's state. No pictures: they are D3D12
// textures and the Vulkan menu cannot show them.
void DrawAdvancedUi();

} // namespace NativeMotionVk
