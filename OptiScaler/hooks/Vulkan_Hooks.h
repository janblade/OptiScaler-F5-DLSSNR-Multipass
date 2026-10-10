#pragma once
#include "SysUtils.h"

class VulkanHooks
{
  public:
    static PFN_vkCreateSemaphore o_vkCreateSemaphore;
    static PFN_vkSignalSemaphore o_vkSignalSemaphore;
    static PFN_vkAntiLagUpdateAMD o_vkAntiLagUpdateAMD;

    // The device was created with the timeline semaphore feature on (the game's own, or switched on for Optical F5Low).
    static bool TimelineSemaphoresOn(VkDevice device);

    // vkWaitSemaphores on one timeline semaphore for one value; true once it is reached, false on timeout or when the
    // call is not there.
    static bool WaitTimelineSemaphore(VkDevice device, VkSemaphore semaphore, uint64_t value, uint64_t timeoutNs);

    // vkCreateWin32SurfaceKHR without our hook (the present bridge makes a surface of its own).
    static PFN_vkCreateWin32SurfaceKHR OriginalCreateWin32Surface();

    // vkDestroySurfaceKHR without our hook: the bridge destroys that surface holding its own lock, which the hook takes.
    static PFN_vkDestroySurfaceKHR OriginalDestroySurface();

    static void Hook(HMODULE vulkan1);
    static void Unhook();
};
