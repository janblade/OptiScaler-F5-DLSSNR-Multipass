#pragma once
#include "SysUtils.h"

class VulkanHooks
{
  public:
    static PFN_vkCreateSemaphore o_vkCreateSemaphore;
    static PFN_vkSignalSemaphore o_vkSignalSemaphore;
    static PFN_vkAntiLagUpdateAMD o_vkAntiLagUpdateAMD;

    // vkCreateWin32SurfaceKHR without our hook (the present bridge makes a surface of its own).
    static PFN_vkCreateWin32SurfaceKHR OriginalCreateWin32Surface();

    static void Hook(HMODULE vulkan1);
    static void Unhook();
};
