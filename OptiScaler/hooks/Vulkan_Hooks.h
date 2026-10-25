#pragma once
#include "SysUtils.h"

#include <shared_mutex>

class VulkanHooks
{
  public:
    static PFN_vkCreateSemaphore o_vkCreateSemaphore;
    static PFN_vkSignalSemaphore o_vkSignalSemaphore;
    static PFN_vkAntiLagUpdateAMD o_vkAntiLagUpdateAMD;

    // The driver's VK_NV_low_latency2 (InputCommon's Reflex output on Vulkan, low_latency_tech/ll_reflex_vk.h, and the
    // game's own calls passed on): resolved on a device made with the extension, or from the game's own lookup of it
    static PFN_vkSetLatencySleepModeNV o_vkSetLatencySleepModeNV;
    static PFN_vkLatencySleepNV o_vkLatencySleepNV;
    static PFN_vkSetLatencyMarkerNV o_vkSetLatencyMarkerNV;
    static PFN_vkGetLatencyTimingsNV o_vkGetLatencyTimingsNV;
    static PFN_vkQueueNotifyOutOfBandNV o_vkQueueNotifyOutOfBandNV;

    // Set while the FG output shows a surface the game created to its Streamline, whose vkCreateWin32SurfaceKHR gets
    // this one instead of a new one
    inline static thread_local VkSurfaceKHR existingSurface = VK_NULL_HANDLE;

    // The window a surface was created for
    static HWND SurfaceWindow(VkSurfaceKHR surface);

    // vkGetDeviceProcAddr past our hook
    static PFN_vkVoidFunction GetDeviceProcAddr(VkDevice device, const char* name);

    // vkGetInstanceProcAddr without OptiScaler's hooks
    static PFN_vkVoidFunction GetInstanceProcAddr(VkInstance instance, const char* pName);

    // The driver's swapchain function behind OptiScaler's hook, below hooks others (overlays) put on the same function.
    // vkQueuePresentKHR's also times the presented frames for the menu. nullptr when it isn't hooked.
    static PFN_vkVoidFunction OriginalSwapchainFunction(const char* pName);

    // The device was created with VK_NV_low_latency2 and the timeline semaphore feature on (its sleep signals one), and
    // not by dxvk or vkd3d-proton for a D3D device (those run the D3D game's own Reflex themselves)
    static bool LowLatency2On(VkDevice device);

    // The device's newest swapchain and whether it was made with VK_NV_low_latency2's latency mode (ours chained in, or
    // the game's own); VK_NULL_HANDLE when it has none
    struct LatencySwapchain
    {
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        bool latencyMode = false;
    };
    static LatencySwapchain LatencySwapchainOf(VkDevice device);
    // The game called VK_NV_low_latency2 on this swapchain: it has latency mode (made before our hooks, or by the game)
    static void NoteLatencySwapchain(VkDevice device, VkSwapchainKHR swapchain);

    // A call of ours on the device's swapchain with latency mode (ll_reflex_vk.h): the swapchain can't be destroyed
    // while one is made (vkDestroySwapchainKHR waits for it). The game's Reflex calls that lead to it never name the
    // swapchain, so the game can't keep them apart from its own swapchain remake. Never nested on a thread.
    class LatencySwapchainUse
    {
        std::shared_lock<std::shared_mutex> _lock;

      public:
        explicit LatencySwapchainUse(VkDevice device);

        // VK_NULL_HANDLE: the device has no swapchain with latency mode
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    };

    // The device was created with the timeline semaphore feature on (the game's own, or switched on for Optical F5Low).
    static bool TimelineSemaphoresOn(VkDevice device);

    // vkWaitSemaphores on one timeline semaphore for one value; true once it is reached, false on timeout or when the
    // call is not there.
    static bool WaitTimelineSemaphore(VkDevice device, VkSemaphore semaphore, uint64_t value, uint64_t timeoutNs);

    // vkSignalSemaphore on one timeline semaphore, from the host; true when it went through.
    static bool SignalTimelineSemaphore(VkDevice device, VkSemaphore semaphore, uint64_t value);
    // The device has vkSignalSemaphore: a game's semaphore can be signalled for it instead of by the driver's sleep
    static bool CanSignalTimelineSemaphore(VkDevice device);

    // vkCreateWin32SurfaceKHR without our hook (the present bridge makes a surface of its own).
    static PFN_vkCreateWin32SurfaceKHR OriginalCreateWin32Surface();

    // vkDestroySurfaceKHR without our hook: the bridge destroys that surface holding its own lock, which the hook takes.
    static PFN_vkDestroySurfaceKHR OriginalDestroySurface();

    static void Hook(HMODULE vulkan1);
    static void Unhook();
};
