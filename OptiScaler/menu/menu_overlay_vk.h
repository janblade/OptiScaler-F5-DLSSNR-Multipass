#pragma once

#include "SysUtils.h"
#include <vulkan/vulkan.hpp>

namespace MenuOverlayVk
{
void CreateSwapchain(VkDevice device, VkPhysicalDevice pd, VkInstance instance, HWND hwnd,
                     const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator,
                     VkSwapchainKHR* pSwapchain);
bool QueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo);
// The game's swapchain went onto the frame-generation bridge (native/VkPresentBridge.h): the menu moves to the D3D12
// swapchain. Lets go of ImGui's Vulkan renderer backend too, if one was made: the D3D12 menu sets up its own only when no
// renderer backend is there (menu_overlay_dx.cpp), and drawing under the Vulkan one crashed in the driver.
void HandOverToBridge();
void DestroyVulkanObjects(bool shutdown);
} // namespace MenuOverlayVk
