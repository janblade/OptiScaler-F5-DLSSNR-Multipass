#pragma once

#include "SysUtils.h"
#include <vulkan/vulkan.hpp>

namespace MenuOverlayVk
{
void CreateSwapchain(VkDevice device, VkPhysicalDevice pd, VkInstance instance, HWND hwnd,
                     const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator,
                     VkSwapchainKHR* pSwapchain);
bool QueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo);
// The Vulkan menu has taken ImGui's renderer backend: a D3D12 menu cannot start under it (menu_overlay_dx.cpp only sets up
// its descriptors when no backend is there), so a Vulkan present bridge made now would crash on its first frame.
bool IsUp();
void DestroyVulkanObjects(bool shutdown);
} // namespace MenuOverlayVk
