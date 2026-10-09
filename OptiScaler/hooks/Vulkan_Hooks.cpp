#include "pch.h"

#include "Vulkan_Hooks.h"

#include <Util.h>
#include <Config.h>
#include <SysUtils.h>

#include <menu/menu_overlay_vk.h>
#include <proxies/KernelBase_Proxy.h>
#include <upscaler_time/UpscalerTime_Vk.h>

#include <misc/FrameLimit.h>
#include "Reflex_Hooks.h"

#include <spoofing/Vulkan_Spoofing.h>

#include <vulkan/vulkan.hpp>

#include <dlssnr/DlssNr_VkExtensions.h>
#include <dlssnr/DlssNrFeature_Vk.h>
#include <native/NativeDriverVk.h>
#include <native/NativeLowLatency.h>
#include <Logger.h>
#include <native/PresentStageTiming.h>
#include <native/VkFrameSource.h>
#include <native/VkPresentBridge.h>
#include <resource_tracking/GenericDepth_Vk.h>

#include <detours/detours.h>
#include <misc/IdentifyGpu.h>

#include "Hook_Utils.h"

// for menu rendering
static VkDevice _device = VK_NULL_HANDLE;
static VkInstance _instance = VK_NULL_HANDLE;
static VkPhysicalDevice _PD = VK_NULL_HANDLE;
static HWND _hwnd = nullptr;

static std::mutex _vkPresentMutex;

PFN_vkCreateDevice o_vkCreateDevice = nullptr;
static PFN_vkDestroyDevice o_vkDestroyDevice = nullptr;
PFN_vkCreateInstance o_vkCreateInstance = nullptr;
PFN_vkCreateWin32SurfaceKHR o_vkCreateWin32SurfaceKHR = nullptr;
PFN_vkQueuePresentKHR o_QueuePresentKHR = nullptr;
PFN_vkCreateSwapchainKHR o_CreateSwapchainKHR = nullptr;
static PFN_vkDestroySwapchainKHR o_DestroySwapchainKHR = nullptr;
static PFN_vkDestroySurfaceKHR o_vkDestroySurfaceKHR = nullptr;
static PFN_vkAcquireFullScreenExclusiveModeEXT o_AcquireFullScreenExclusiveModeEXT = nullptr;
static PFN_vkReleaseFullScreenExclusiveModeEXT o_ReleaseFullScreenExclusiveModeEXT = nullptr;
static PFN_vkGetInstanceProcAddr o_vkGetInstanceProcAddr = nullptr;
static PFN_vkGetDeviceProcAddr o_vkGetDeviceProcAddr = nullptr;

// Those aren't hooked, just grabbed for use
static PFN_vkGetPhysicalDeviceFeatures2 o_vkGetPhysicalDeviceFeatures2 = nullptr;
PFN_vkCreateSemaphore VulkanHooks::o_vkCreateSemaphore = nullptr;
PFN_vkSignalSemaphore VulkanHooks::o_vkSignalSemaphore = nullptr;
PFN_vkAntiLagUpdateAMD VulkanHooks::o_vkAntiLagUpdateAMD = nullptr;

// Forward declaration
static VkResult hkvkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);
static VkResult hkvkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                                       const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain);
static void hkvkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator);

// A bridged swapchain is on a hidden window and made with exclusive full screen disallowed (native/VkPresentBridge.h):
// the game's own acquire and release of the mode succeed without doing anything.
static VkResult hkvkAcquireFullScreenExclusiveModeEXT(VkDevice device, VkSwapchainKHR swapchain)
{
    if (VkPresentBridge::Owns(swapchain))
        return VK_SUCCESS;

    return o_AcquireFullScreenExclusiveModeEXT(device, swapchain);
}

static VkResult hkvkReleaseFullScreenExclusiveModeEXT(VkDevice device, VkSwapchainKHR swapchain)
{
    if (VkPresentBridge::Owns(swapchain))
        return VK_SUCCESS;

    return o_ReleaseFullScreenExclusiveModeEXT(device, swapchain);
}

// A Vulkan game's own Reflex, through VK_NV_low_latency2: Optical F5Low's low latency stands aside once it is called
// (native/NativeLowLatency.h). Our own low latency goes through the NvAPI_Vulkan_* interface and never calls these.
static PFN_vkSetLatencySleepModeNV o_vkSetLatencySleepModeNV = nullptr;
static PFN_vkLatencySleepNV o_vkLatencySleepNV = nullptr;
static PFN_vkSetLatencyMarkerNV o_vkSetLatencyMarkerNV = nullptr;

static VkResult VKAPI_CALL hkvkSetLatencySleepModeNV(VkDevice device, VkSwapchainKHR swapchain,
                                                     const VkLatencySleepModeInfoNV* pSleepModeInfo)
{
    ReflexHooks::noteGameVulkanLowLatency2(true);
    return o_vkSetLatencySleepModeNV(device, swapchain, pSleepModeInfo);
}

static VkResult VKAPI_CALL hkvkLatencySleepNV(VkDevice device, VkSwapchainKHR swapchain,
                                              const VkLatencySleepInfoNV* pSleepInfo)
{
    ReflexHooks::noteGameVulkanLowLatency2(false);
    return o_vkLatencySleepNV(device, swapchain, pSleepInfo);
}

static void VKAPI_CALL hkvkSetLatencyMarkerNV(VkDevice device, VkSwapchainKHR swapchain,
                                              const VkSetLatencyMarkerInfoNV* pLatencyMarkerInfo)
{
    ReflexHooks::noteGameVulkanLowLatency2(false);
    o_vkSetLatencyMarkerNV(device, swapchain, pLatencyMarkerInfo);
}

// Handed out by vkGetInstanceProcAddr / vkGetDeviceProcAddr for the three calls (the loader's own stubs for extension
// functions are too small to detour, so the game gets our function and ours calls the one the loader gave). Null for any
// other name.
static PFN_vkVoidFunction HookLowLatency2(const std::string& name, PFN_vkVoidFunction orgFunc)
{
    if (name == "vkSetLatencySleepModeNV")
    {
        if (o_vkSetLatencySleepModeNV == nullptr)
            o_vkSetLatencySleepModeNV = (PFN_vkSetLatencySleepModeNV) orgFunc;

        return (PFN_vkVoidFunction) hkvkSetLatencySleepModeNV;
    }

    if (name == "vkLatencySleepNV")
    {
        if (o_vkLatencySleepNV == nullptr)
            o_vkLatencySleepNV = (PFN_vkLatencySleepNV) orgFunc;

        return (PFN_vkVoidFunction) hkvkLatencySleepNV;
    }

    if (name == "vkSetLatencyMarkerNV")
    {
        if (o_vkSetLatencyMarkerNV == nullptr)
            o_vkSetLatencyMarkerNV = (PFN_vkSetLatencyMarkerNV) orgFunc;

        return (PFN_vkVoidFunction) hkvkSetLatencyMarkerNV;
    }

    return nullptr;
}

// Devices created with the timeline semaphore feature on, as the create call finally had it (VulkanHooks::
// TimelineSemaphoresOn): the Vulkan Sleep's semaphore is one.
static std::mutex _timelineDevicesMutex;
static std::set<VkDevice> _timelineDevices;

static bool TimelineFeatureOn(const void* chain)
{
    for (auto* node = static_cast<const VkBaseInStructure*>(chain); node != nullptr; node = node->pNext)
    {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES &&
            reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(node)->timelineSemaphore)
            return true;

        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES &&
            reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures*>(node)->timelineSemaphore)
            return true;
    }

    return false;
}

bool VulkanHooks::TimelineSemaphoresOn(VkDevice device)
{
    std::lock_guard lock(_timelineDevicesMutex);
    return _timelineDevices.contains(device);
}

bool VulkanHooks::WaitTimelineSemaphore(VkDevice device, VkSemaphore semaphore, uint64_t value, uint64_t timeoutNs)
{
    static PFN_vkWaitSemaphores wait = nullptr;
    static VkDevice waitDevice = VK_NULL_HANDLE;

    if (wait == nullptr || waitDevice != device)
    {
        wait = o_vkGetDeviceProcAddr != nullptr
                   ? reinterpret_cast<PFN_vkWaitSemaphores>(o_vkGetDeviceProcAddr(device, "vkWaitSemaphores"))
                   : nullptr;

        if (wait == nullptr && o_vkGetDeviceProcAddr != nullptr)
            wait = reinterpret_cast<PFN_vkWaitSemaphores>(o_vkGetDeviceProcAddr(device, "vkWaitSemaphoresKHR"));

        waitDevice = device;
    }

    if (wait == nullptr || semaphore == VK_NULL_HANDLE)
        return false;

    VkSemaphoreWaitInfo info {};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    info.semaphoreCount = 1;
    info.pSemaphores = &semaphore;
    info.pValues = &value;

    return wait(device, &info, timeoutNs) == VK_SUCCESS;
}

static void HookDevice(VkDevice InDevice)
{
    if (o_CreateSwapchainKHR != nullptr || State::Instance().vulkanSkipHooks)
        return;

    LOG_FUNC();

    o_QueuePresentKHR = (PFN_vkQueuePresentKHR) (vkGetDeviceProcAddr(InDevice, "vkQueuePresentKHR"));
    o_CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR) (vkGetDeviceProcAddr(InDevice, "vkCreateSwapchainKHR"));
    o_DestroySwapchainKHR = (PFN_vkDestroySwapchainKHR) (vkGetDeviceProcAddr(InDevice, "vkDestroySwapchainKHR"));
    o_AcquireFullScreenExclusiveModeEXT = (PFN_vkAcquireFullScreenExclusiveModeEXT) (vkGetDeviceProcAddr(
        InDevice, "vkAcquireFullScreenExclusiveModeEXT"));
    o_ReleaseFullScreenExclusiveModeEXT = (PFN_vkReleaseFullScreenExclusiveModeEXT) (vkGetDeviceProcAddr(
        InDevice, "vkReleaseFullScreenExclusiveModeEXT"));

    if (o_CreateSwapchainKHR)
    {
        LOG_DEBUG("Hooking VkDevice");

        // Hook
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        if (o_QueuePresentKHR != nullptr)
            DetourAttach(&(PVOID&) o_QueuePresentKHR, hkvkQueuePresentKHR);

        if (o_CreateSwapchainKHR != nullptr)
            DetourAttach(&(PVOID&) o_CreateSwapchainKHR, hkvkCreateSwapchainKHR);

        if (o_DestroySwapchainKHR != nullptr)
            DetourAttach(&(PVOID&) o_DestroySwapchainKHR, hkvkDestroySwapchainKHR);

        if (o_AcquireFullScreenExclusiveModeEXT != nullptr)
            DetourAttach(&(PVOID&) o_AcquireFullScreenExclusiveModeEXT, hkvkAcquireFullScreenExclusiveModeEXT);

        if (o_ReleaseFullScreenExclusiveModeEXT != nullptr)
            DetourAttach(&(PVOID&) o_ReleaseFullScreenExclusiveModeEXT, hkvkReleaseFullScreenExclusiveModeEXT);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook VkDevice, error code: {:X}", detourResult);
            o_QueuePresentKHR = nullptr;
            o_CreateSwapchainKHR = nullptr;
        }
    }
}

VALIDATE_HOOK(hkvkCreateWin32SurfaceKHR, PFN_vkCreateWin32SurfaceKHR)
static VkResult hkvkCreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
                                          const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface)
{
    LOG_FUNC();

    auto result = o_vkCreateWin32SurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);

    auto procHwnd = Util::GetProcessWindow();
    LOG_DEBUG("procHwnd: {0:X}, swapchain hwnd: {1:X}", (UINT64) procHwnd, (UINT64) pCreateInfo->hwnd);

    if (result == VK_SUCCESS && !State::Instance().vulkanSkipHooks)
    {
        MenuOverlayVk::DestroyVulkanObjects(false);

        _instance = instance;
        State::Instance().VulkanInstance = instance;
        LOG_DEBUG("_instance captured: {0:X}", (UINT64) _instance);
        _hwnd = pCreateInfo->hwnd;
        LOG_DEBUG("_hwnd captured: {0:X}", (UINT64) _hwnd);
    }

    if (result == VK_SUCCESS && pSurface != nullptr)
        VkPresentBridge::NoteSurface(*pSurface, pCreateInfo->hwnd);

    LOG_FUNC_RESULT(result);

    return result;
}

VALIDATE_HOOK(hkvkCreateInstance, PFN_vkCreateInstance)
static VkResult hkvkCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
                                   VkInstance* pInstance)
{
    LOG_FUNC();

    VkInstanceCreateInfo localCreateInfo {};
    memcpy(&localCreateInfo, pCreateInfo, sizeof(VkInstanceCreateInfo));

    VulkanSpoofing::hkvkCreateInstance(&localCreateInfo, pAllocator, pInstance);

    VkResult result;
    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
        result = o_vkCreateInstance(&localCreateInfo, pAllocator, pInstance);
    }

    if (result == VK_SUCCESS)
    {
        State::Instance().VulkanInstance = *pInstance;
        LOG_DEBUG("State::Instance().VulkanInstance captured: {0:X}", (UINT64) State::Instance().VulkanInstance);

#ifdef VULKAN_DEBUG_LAYER
        auto address = vkGetInstanceProcAddr(State::Instance().VulkanInstance, "vkCreateDebugUtilsMessengerEXT");
        auto vkCreateDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT) address;
        VkDebugUtilsMessengerEXT debugMessenger;
        vkCreateDebugUtilsMessengerEXT(State::Instance().VulkanInstance, &VulkanSpoofing::debugCreateInfo, nullptr,
                                       &debugMessenger);
#endif
    }

    // Disabled to prevent unnecessary object release
    // if (result == VK_SUCCESS && !State::Instance().vulkanSkipHooks)
    //{
    //     MenuOverlayVk::DestroyVulkanObjects(false);
    // }

    LOG_FUNC_RESULT(result);

    return result;
}

VALIDATE_HOOK(hkvkCreateDevice, PFN_vkCreateDevice)
static VkResult hkvkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
                                 const VkAllocationCallbacks* pAllocator, VkDevice* pDevice)
{
    LOG_FUNC();

    VkDeviceCreateInfo localCreteInfo {};
    memcpy(&localCreteInfo, pCreateInfo, sizeof(VkDeviceCreateInfo));

    // Check support for AntiLag before spoof
    VkPhysicalDeviceFeatures2 features2 = {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

    VkPhysicalDeviceAntiLagFeaturesAMD antiLagFeatures = {};
    antiLagFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD;

    features2.pNext = &antiLagFeatures;

    if (o_vkGetPhysicalDeviceFeatures2)
    {
        o_vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);
        State::Instance().vkAntiLagSupported = antiLagFeatures.antiLag != 0;
    }

    VulkanSpoofing::hkvkCreateDevice(physicalDevice, &localCreteInfo, pAllocator, pDevice);

    // Neural Rendering on Vulkan without a D3D12 bridge, or the reason it cannot be.
    //
    // The model needs two NVIDIA vendor extensions to load its kernels, a game never asks for them,
    // and a device's extension list cannot be changed after creation. This is the only moment it can
    // be arranged. Reported either way: if the answer is no, the log says so here rather than leaving
    // a create failure three layers down to be explained.
    //
    // Only appended when the feature is switched on, and only what the physical device already
    // offers -- asking for an extension a driver does not have makes vkCreateDevice fail and the game
    // not start.
    DlssNr::VkExt::Merged nrExtensions;
    VkPhysicalDeviceBufferDeviceAddressFeatures nrAddressFeature {};
    // The game's own feature flag switched on for the create call, put back once it returns: the chain is the game's
    // memory and it may read it later as its own choice.
    VkBool32* nrBorrowedFlag = nullptr;
    VkBool32* nrBorrowedWriteFlag = nullptr; // the same for shaderStorageImageWriteWithoutFormat
    VkPhysicalDeviceFeatures nrCoreFeatures {};
    bool nrWritesWithoutFormat = false;

    if (Config::Instance()->DlssNrEnabled.value_or_default())
    {
        std::vector<std::string> supported;

        {
            // The device's real list. With extension spoofing on, the enumerate hook also reports the NVIDIA vendor
            // pair on devices that lack it, and asking for those would fail the create.
            ScopedSkipSpoofingThread skipSpoofing {};
            supported = DlssNr::VkExt::SupportedDeviceExtensions(o_vkGetInstanceProcAddr,
                                                                 State::Instance().VulkanInstance, physicalDevice);
        }

        // Without the vendor pair the model cannot load its kernels, so nothing about the game's device is changed
        // (another vendor, an integrated GPU, dxvk's helper devices, a driver without them).
        if (!DlssNr::VkExt::Contains(supported, "VK_NVX_binary_import") ||
            !DlssNr::VkExt::Contains(supported, "VK_NVX_image_view_handle"))
        {
            LOG_INFO("DLSS-NR Vulkan: this device does not offer VK_NVX_binary_import and VK_NVX_image_view_handle "
                     "({} extensions offered); the native path is not possible on it and the device is left as the "
                     "game made it",
                     supported.size());
        }
        else
        {
            const char* const* gameNames = pCreateInfo->ppEnabledExtensionNames;
            const uint32_t gameCount = pCreateInfo->enabledExtensionCount;
            // The EXT and KHR forms of buffer_device_address may not both be enabled. A game that asked for the EXT
            // one keeps it, and the KHR one is not added.
            const bool gameWantsExtAddress =
                DlssNr::VkExt::ListHas(gameNames, gameCount, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) &&
                !DlssNr::VkExt::ListHas(gameNames, gameCount, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);

            // The scanned list, plus whatever NGX itself says the feature needs. `owned` keeps the strings alive
            // through the create call; it is filled completely before any pointer into it is taken.
            int ngxResult = 0;
            const auto ngxWants =
                DlssNr::VkExt::NgxDeviceExtensions(State::Instance().VulkanInstance, physicalDevice, ngxResult);
            std::string fromNgx;

            for (const char* want : DlssNr::VkExt::kDevice)
                nrExtensions.owned.emplace_back(want);

            const size_t floorCount = nrExtensions.owned.size();

            for (const auto& want : ngxWants)
            {
                if (!DlssNr::VkExt::Contains(nrExtensions.owned, want.c_str()))
                {
                    nrExtensions.owned.push_back(want);
                    fromNgx += std::string(fromNgx.empty() ? "" : ", ") + want;
                }
            }

            LOG_INFO("DLSS-NR Vulkan: NGX's own requirement list for the model: {} ({} extensions{}{})",
                     ngxResult == 0                         ? std::string("not asked (DLSS off or no NGX core)")
                     : ngxResult == NVSDK_NGX_Result_Success ? std::string("answered")
                                                             : std::format("refused, {:#x}", (uint32_t) ngxResult),
                     ngxWants.size(), fromNgx.empty() ? "" : "; not in the scanned list: ", fromNgx);

            std::string present, added, missing, missingNgx, dropped;

            // The list as spoofing left it, less an EXT buffer_device_address the game did not ask for itself (the
            // spoofing path adds it; the KHR one goes in below).
            for (uint32_t i = 0; i < localCreteInfo.enabledExtensionCount; ++i)
            {
                const char* name = localCreteInfo.ppEnabledExtensionNames[i];

                if (!gameWantsExtAddress && name != nullptr &&
                    std::string(name) == VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)
                {
                    dropped = name;
                    continue;
                }

                nrExtensions.names.push_back(name);
            }

            for (size_t i = 0; i < nrExtensions.owned.size(); ++i)
            {
                const char* want = nrExtensions.owned[i].c_str();
                const bool already = DlssNr::VkExt::ListHas(localCreteInfo.ppEnabledExtensionNames,
                                                            localCreteInfo.enabledExtensionCount, want);

                if (already)
                    present += std::string(present.empty() ? "" : ", ") + want;
                else if (gameWantsExtAddress && std::string(want) == VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)
                    continue; // the game's EXT form stands in for it
                else if (!DlssNr::VkExt::Contains(supported, want))
                {
                    auto& list = i < floorCount ? missing : missingNgx;
                    list += std::string(list.empty() ? "" : ", ") + want;
                }
                else
                {
                    nrExtensions.names.push_back(want);
                    added += std::string(added.empty() ? "" : ", ") + want;
                }
            }

            // Without the KHR form in the end there is nothing for the dropped EXT one to clash with.
            if (!dropped.empty() &&
                !DlssNr::VkExt::ListHas(nrExtensions.names.data(), (uint32_t) nrExtensions.names.size(),
                                        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME))
            {
                nrExtensions.names.push_back(VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
                dropped.clear();
            }

            LOG_INFO("DLSS-NR Vulkan: device offers {} extensions. game already enabled: [{}]. added here: "
                     "[{}]. NOT AVAILABLE: [{}]{}{}{}",
                     supported.size(), present.empty() ? "none" : present, added.empty() ? "none" : added,
                     missing.empty() ? "none" : missing,
                     missingNgx.empty() ? "" : ". asked for by NGX but not offered: ", missingNgx,
                     dropped.empty() ? "" : ". VK_EXT_buffer_device_address removed (the KHR form replaces it)");

            if (gameWantsExtAddress)
                LOG_INFO("DLSS-NR Vulkan: the game enabled VK_EXT_buffer_device_address itself; the KHR form is not "
                         "added");

            if (!missing.empty())
                LOG_WARN("DLSS-NR Vulkan: the native path is not possible on this device -- the model's kernels "
                         "cannot be loaded without the extensions listed as NOT AVAILABLE");

            if (!added.empty() || !dropped.empty())
            {
                localCreteInfo.ppEnabledExtensionNames = nrExtensions.names.data();
                localCreteInfo.enabledExtensionCount = (uint32_t) nrExtensions.names.size();
            }

            // VK_KHR_buffer_device_address does nothing until its feature is switched on too. Where the game already
            // lists the feature struct (its own, or the Vulkan 1.2 one -- both may not be chained), the flag is set
            // in it for the call and put back after; otherwise ours goes at the head of the chain. Only when the
            // device supports it.
            if (DlssNr::VkExt::ListHas(localCreteInfo.ppEnabledExtensionNames, localCreteInfo.enabledExtensionCount,
                                       VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME))
            {
                VkPhysicalDeviceBufferDeviceAddressFeatures offered {};
                offered.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
                VkPhysicalDeviceFeatures2 query {};
                query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                query.pNext = &offered;

                if (o_vkGetPhysicalDeviceFeatures2)
                    o_vkGetPhysicalDeviceFeatures2(physicalDevice, &query);

                const char* what = "not offered by the device";

                if (offered.bufferDeviceAddress)
                {
                    what = nullptr;

                    for (auto* node = (VkBaseOutStructure*) localCreteInfo.pNext; node != nullptr && what == nullptr;
                         node = node->pNext)
                    {
                        VkBool32* flag = nullptr;

                        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
                            flag = &((VkPhysicalDeviceVulkan12Features*) node)->bufferDeviceAddress;
                        else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES ||
                                 node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT)
                            flag = &((VkPhysicalDeviceBufferDeviceAddressFeatures*) node)->bufferDeviceAddress;

                        if (flag == nullptr)
                            continue;

                        if (*flag)
                            what = "already on (the game's own)";
                        else if (!DlssNr::VkExt::IsWritable(flag))
                            what = "left off: the game's feature struct is read-only";
                        else
                        {
                            *flag = VK_TRUE;
                            nrBorrowedFlag = flag;
                            what = "switched on in the game's feature struct for the create call";
                        }
                    }

                    if (what == nullptr)
                    {
                        nrAddressFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
                        nrAddressFeature.bufferDeviceAddress = VK_TRUE;
                        nrAddressFeature.pNext = (void*) localCreteInfo.pNext;
                        localCreteInfo.pNext = &nrAddressFeature;
                        what = "switched on here";
                    }
                }

                LOG_INFO("DLSS-NR Vulkan: bufferDeviceAddress feature {}", what);
            }

            // Detail reuse's shader writes storage images declared without a format (DlssNrDetailReuse_Vk.h), which
            // needs shaderStorageImageWriteWithoutFormat. The core features come either as pEnabledFeatures (a copy
            // of the game's is handed on instead) or as a VkPhysicalDeviceFeatures2 in the chain (the flag is set
            // for the call and put back after), never both.
            {
                VkPhysicalDeviceFeatures2 query {};
                query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

                if (o_vkGetPhysicalDeviceFeatures2)
                    o_vkGetPhysicalDeviceFeatures2(physicalDevice, &query);

                const char* what = "not offered by the device";

                if (query.features.shaderStorageImageWriteWithoutFormat)
                {
                    VkBool32* chained = nullptr;

                    for (auto* node = (VkBaseOutStructure*) localCreteInfo.pNext; node != nullptr; node = node->pNext)
                    {
                        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
                        {
                            chained = &((VkPhysicalDeviceFeatures2*) node)->features.shaderStorageImageWriteWithoutFormat;
                            break;
                        }
                    }

                    if (chained != nullptr)
                    {
                        if (*chained)
                            what = "already on (the game's own)";
                        else if (!DlssNr::VkExt::IsWritable(chained))
                            what = "left off: the game's feature struct is read-only";
                        else
                        {
                            *chained = VK_TRUE;
                            nrBorrowedWriteFlag = chained;
                            what = "switched on in the game's feature struct for the create call";
                        }

                        nrWritesWithoutFormat = *chained != VK_FALSE;
                    }
                    else
                    {
                        if (localCreteInfo.pEnabledFeatures != nullptr)
                            nrCoreFeatures = *localCreteInfo.pEnabledFeatures;

                        what = nrCoreFeatures.shaderStorageImageWriteWithoutFormat ? "already on (the game's own)"
                                                                                    : "switched on here";
                        nrCoreFeatures.shaderStorageImageWriteWithoutFormat = VK_TRUE;
                        localCreteInfo.pEnabledFeatures = &nrCoreFeatures;
                        nrWritesWithoutFormat = true;
                    }
                }

                LOG_INFO("DLSS-NR Vulkan: shaderStorageImageWriteWithoutFormat {}", what);
            }
        }
    }

    // Optical F5Low on Vulkan hands the picture to a D3D12 device and waits for it on the GPU through a D3D12 fence opened
    // as a timeline semaphore (native/SharedFrameVk.h), which needs the timeline semaphore feature. The same as for the
    // flags above: set in the game's own struct for the call when it lists one, else ours goes at the head of the chain,
    // which is only valid when the extension is in the list (the spoofing step above adds it when offered). Not tied to
    // the NVIDIA pair: the D3D12 side also runs the vendor-neutral port.
    VkPhysicalDeviceTimelineSemaphoreFeatures nativeTimelineFeature {};
    VkBool32* nativeBorrowedTimeline = nullptr;

    if (Config::Instance()->DlssNrEnabled.value_or_default())
    {
        VkPhysicalDeviceTimelineSemaphoreFeatures offered {};
        offered.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
        VkPhysicalDeviceFeatures2 query {};
        query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        query.pNext = &offered;

        if (o_vkGetPhysicalDeviceFeatures2)
            o_vkGetPhysicalDeviceFeatures2(physicalDevice, &query);

        const char* what = "not offered by the device";

        if (offered.timelineSemaphore)
        {
            what = nullptr;

            for (auto* node = (VkBaseOutStructure*) localCreteInfo.pNext; node != nullptr && what == nullptr;
                 node = node->pNext)
            {
                VkBool32* flag = nullptr;

                if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
                    flag = &((VkPhysicalDeviceVulkan12Features*) node)->timelineSemaphore;
                else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)
                    flag = &((VkPhysicalDeviceTimelineSemaphoreFeatures*) node)->timelineSemaphore;

                if (flag == nullptr)
                    continue;

                if (*flag)
                    what = "already on (the game's own)";
                else if (!DlssNr::VkExt::IsWritable(flag))
                    what = "left off: the game's feature struct is read-only";
                else
                {
                    *flag = VK_TRUE;
                    nativeBorrowedTimeline = flag;
                    what = "switched on in the game's feature struct for the create call";
                }
            }

            if (what == nullptr)
            {
                if (DlssNr::VkExt::ListHas(localCreteInfo.ppEnabledExtensionNames, localCreteInfo.enabledExtensionCount,
                                           VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME))
                {
                    nativeTimelineFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
                    nativeTimelineFeature.timelineSemaphore = VK_TRUE;
                    nativeTimelineFeature.pNext = (void*) localCreteInfo.pNext;
                    localCreteInfo.pNext = &nativeTimelineFeature;
                    what = "switched on here";
                }
                else
                {
                    what = "left off: neither the extension nor the game's Vulkan 1.2 features are in the create call";
                }
            }
        }

        LOG_INFO("Optical F5Low Vulkan: timelineSemaphore feature {}", what);
    }

    auto result = o_vkCreateDevice(physicalDevice, &localCreteInfo, pAllocator, pDevice);

    // As the call had it, before a borrowed flag is put back
    if (result == VK_SUCCESS && pDevice != nullptr && *pDevice != VK_NULL_HANDLE &&
        TimelineFeatureOn(localCreteInfo.pNext))
    {
        std::lock_guard lock(_timelineDevicesMutex);
        _timelineDevices.insert(*pDevice);
    }

    if (nativeBorrowedTimeline != nullptr)
        *nativeBorrowedTimeline = VK_FALSE;

    if (result == VK_SUCCESS && pDevice != nullptr && *pDevice != VK_NULL_HANDLE)
    {
        native::VkFrameSource::NoteDevice(*pDevice, localCreteInfo);
        GenericDepthVk::OnDevice(*pDevice, physicalDevice);
    }

    if (nrBorrowedFlag != nullptr)
        *nrBorrowedFlag = VK_FALSE;

    if (nrBorrowedWriteFlag != nullptr)
        *nrBorrowedWriteFlag = VK_FALSE;

    if (result == VK_SUCCESS && pDevice != nullptr)
        DlssNr::VkExt::NoteDevice(*pDevice, nrWritesWithoutFormat);

    if (Config::Instance()->DlssNrEnabled.value_or_default())
        LOG_INFO("DLSS-NR Vulkan: vkCreateDevice returned {} with {} extensions requested", (int) result,
                 localCreteInfo.enabledExtensionCount);

    if (result == VK_SUCCESS && Config::Instance()->OverlayMenu.value_or_default())
    {
        if (!State::Instance().vulkanSkipHooks)
        {
            // Disabled to prevent unnecessary object release
            // MenuOverlayVk::DestroyVulkanObjects(false);

            _PD = physicalDevice;
            LOG_DEBUG("_PD captured: {0:X}", (UINT64) _PD);
            _device = *pDevice;
            LOG_DEBUG("_device captured: {0:X}", (UINT64) _device);
            HookDevice(_device);
        }

        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};

        VkPhysicalDeviceIDProperties idProps {};
        idProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;

        VkPhysicalDeviceProperties2 props2 {};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &idProps;

        vkGetPhysicalDeviceProperties2(physicalDevice, &props2);

        if (idProps.deviceLUIDValid == VK_TRUE)
        {
            auto primaryGpu = IdentifyGpu::getPrimaryGpu();
            auto luid = (PLUID) idProps.deviceLUID;
            if (!IsEqualLUID(*luid, primaryGpu.luid))
                LOG_WARN("VkDevice created with non-primary GPU");
        }
    }

    if (State::Instance().vkAntiLagSupported)
    {
        if (result == VK_SUCCESS && o_vkGetDeviceProcAddr)
        {
            VulkanHooks::o_vkAntiLagUpdateAMD =
                (PFN_vkAntiLagUpdateAMD) o_vkGetDeviceProcAddr(*pDevice, "vkAntiLagUpdateAMD");
        }
        else
        {
            State::Instance().vkAntiLagSupported = false;
            LOG_WARN("Vulkan AntiLag can't be enabled");
        }
    }

#ifdef USE_QUEUE_SUBMIT_2_KHR
    if (result == VK_SUCCESS)
        hkvkGetDeviceProcAddr(*pDevice, "vkQueueSubmit2KHR");
#endif

    LOG_FUNC_RESULT(result);

    return result;
}

// Where a present spends its time, so a stall in a log names the stage that holds it (native/PresentStageTiming.h).
// The game's present thread only.
static native::presenttiming::StageTiming _presentTiming;

static void NotePresentTiming(double hookStartMs)
{
    const auto now = Util::MillisecondsNow();
    _presentTiming.Record(native::presenttiming::Stage::Hook, now - hookStartMs);
    const auto outcome = _presentTiming.EndPresent(now);

    if (outcome.stall)
        LOG_WARN("Vulkan present stall: {}", _presentTiming.StallLine());

    if (outcome.summary)
    {
        LOG_INFO("Vulkan present timing, {}", _presentTiming.SummaryLine());
        _presentTiming.Restart();
        NoteDroppedLogLines();
    }
}

VALIDATE_HOOK(hkvkQueuePresentKHR, PFN_vkQueuePresentKHR)
static VkResult hkvkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
    LOG_FUNC();

    using native::presenttiming::Stage;
    const auto hookStartMs = Util::MillisecondsNow();

    State::Instance().vulkanPresentCount.fetch_add(1, std::memory_order_relaxed);

    // get upscaler time
    UpscalerTimeVk::ReadUpscalingTime(_device);

    // ??? TODO: if we are hooking dxvk's vulkan calls then this present call could be either coming from dxvk or from a
    // native vk game
    if (!IdentifyGpu::getPrimaryGpu().usesDxvk)
        State::Instance().swapchainApi = Vulkan;

    // Tick feature to let it know if it's frozen
    if (auto currentFeature = State::Instance().currentFeature; currentFeature != nullptr)
    {
        if (auto currentFg = State::Instance().currentFG; currentFg != nullptr)
            currentFeature->TickFrozenCheck(currentFg->GetInterpolatedFrameCount());
        else
            currentFeature->TickFrozenCheck();
    }

    VkPresentInfoKHR localPresentInfo {};
    memcpy(&localPresentInfo, pPresentInfo, sizeof(VkPresentInfoKHR));

    // Optical F5Low: the picture through NR before the menu is drawn over it. May replace the wait list.
    const auto motionStartMs = Util::MillisecondsNow();
    NativeMotionVk::OnPresent(queue, &localPresentInfo, _device, _PD);
    _presentTiming.Record(Stage::NativeMotion, Util::MillisecondsNow() - motionStartMs);

    // The game's swapchain is on the bridge's hidden window: the menu and the frame limiter are the D3D12 swapchain's
    const bool bridged = pPresentInfo->swapchainCount == 1 && VkPresentBridge::Active(pPresentInfo->pSwapchains[0]);

    // render menu if needed
    if (!bridged && !MenuOverlayVk::QueuePresent(queue, &localPresentInfo))
    {
        LOG_ERROR("QueuePresent: false!");
        NotePresentTiming(hookStartMs);
        return VK_ERROR_OUT_OF_DATE_KHR;
    }

    ReflexHooks::update(false, true);

    // Optical F5Low's low latency (native/NativeLowLatency.h): Reflex markers around the game's present. This call is the
    // game's own present whether or not the frame-generation bridge is up -- bridged, it is on the hidden window
    // (native/VkPresentBridge.h); the D3D12 swapchain's own present, through FGHooks, is a separate thing frame generation
    // drives on its own thread, which stays out of it. Bridged, the markers and the sleep go through the D3D entry points
    // on the bridge's private D3D12 device -- the same relationship Dx11wDx12SC::Present has to its hidden D3D11 present
    // -- so XeFG's XeLL routing (ReflexHooks) reaches them; the Vulkan NVAPI belongs to a game with no bridge. Not under
    // dxvk: its D3D11 device already gets this through wrapped_swapchain.cpp's present hook, unconditionally.
    const auto lowLatencyTarget = native::lowlatency::PresentTarget(
        true, bridged, _device, bridged ? static_cast<IUnknown*>(VkPresentBridge::Device()) : nullptr);
    const bool lowLatency = !IdentifyGpu::getPrimaryGpu().usesDxvk && lowLatencyTarget.device != nullptr;

    const auto lowLatencyBegin = [&]
    {
        if (lowLatencyTarget.api == native::lowlatency::Api::Vulkan)
            native::lowlatency::OnPresentBeginVulkan(_device);
        else
            native::lowlatency::OnPresentBegin(
                const_cast<IUnknown*>(static_cast<const IUnknown*>(lowLatencyTarget.device)));
    };
    const auto lowLatencyEnd = [&]
    {
        if (lowLatencyTarget.api == native::lowlatency::Api::Vulkan)
            native::lowlatency::OnPresentEndVulkan(_device);
        else
            native::lowlatency::OnPresentEnd(
                const_cast<IUnknown*>(static_cast<const IUnknown*>(lowLatencyTarget.device)));
    };

    // The sleep is inside OnPresentEnd, so the low-latency time is both calls together
    double lowLatencyMs = 0.0;

    if (lowLatency)
    {
        const auto begin = Util::MillisecondsNow();
        lowLatencyBegin();
        lowLatencyMs = Util::MillisecondsNow() - begin;
    }

    // original call
    VkResult result;
    const auto presentStartMs = Util::MillisecondsNow();

    {
        ScopedVulkanCreatingSC scopedVulkanCreatingSC {};
        result = o_QueuePresentKHR(queue, &localPresentInfo);
    }

    _presentTiming.Record(Stage::Present, Util::MillisecondsNow() - presentStartMs);

    if (lowLatency)
    {
        const auto begin = Util::MillisecondsNow();
        lowLatencyEnd();
        lowLatencyMs += Util::MillisecondsNow() - begin;
    }

    _presentTiming.Record(Stage::LowLatency, lowLatencyMs);

    if (bridged)
    {
        // Paced by the D3D12 present (and frame generation's limiter inside it)
        const auto bridgeStartMs = Util::MillisecondsNow();
        NativeMotionVk::AfterPresent(pPresentInfo->pSwapchains[0]);
        _presentTiming.Record(Stage::BridgePresent, Util::MillisecondsNow() - bridgeStartMs);

        // How much of that was FGHooks::FGPresent waiting for frame generation's mutex (it runs inside the D3D12
        // swapchain's present, on this thread): a slow bridge present is then our work or the wait for the other present
        _presentTiming.Record(Stage::FrameGenerationMutexWait, native::presenttiming::TakeFrameGenerationMutexWait());

        // The window changed size and the hidden swapchain does not know: the game recreates when its present says so.
        if (result == VK_SUCCESS && VkPresentBridge::WindowResized())
            result = VK_ERROR_OUT_OF_DATE_KHR;
    }
    // Unsure about Vulkan Reflex fps limit and if that could be causing an issue here
    else if (!State::Instance().reflexLimitsFps)
        FrameLimit::sleep(false);

    NotePresentTiming(hookStartMs);

    LOG_FUNC_RESULT(result);
    return result;
}

VALIDATE_HOOK(hkvkCreateSwapchainKHR, PFN_vkCreateSwapchainKHR)
static VkResult hkvkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                                       const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
{
    LOG_FUNC();

    ScopedVulkanCreatingSC scopedVulkanCreatingSC {};
    VkResult result = VK_SUCCESS;

    // Optical F5Low copies the swapchain's images to and from its D3D12 device: TRANSFER usage is added when the surface
    // allows it (native/VkFrameSource.cpp).
    VkSwapchainCreateInfoKHR localCreateInfo {};

    // With frame generation chosen the swapchain is made on a hidden window and the real one gets a D3D12 swapchain
    // (native/VkPresentBridge.h). The usage is then asked of the hidden surface.
    bool bridged = false;

    if (pCreateInfo != nullptr)
    {
        localCreateInfo = *pCreateInfo;

        if (!State::Instance().vulkanSkipHooks)
        {
            bridged = VkPresentBridge::OnCreateSwapchain(_instance, _PD, device, *pCreateInfo, &localCreateInfo);
            localCreateInfo.imageUsage = native::VkFrameSource::SwapchainUsage(_PD, localCreateInfo);
        }
    }

    // On the hidden surface the swapchain never takes the screen; the game's own value is put back after the call.
    bool fullScreenModeReadOnly = false;
    int32_t* fullScreenMode =
        bridged ? VkPresentBridge::FullScreenExclusiveMode(localCreateInfo.pNext, &fullScreenModeReadOnly) : nullptr;

    if (fullScreenModeReadOnly)
        LOG_WARN("Vulkan bridge: the game's full screen exclusive struct is read-only, so it is made as the game asked "
                 "(the game's own acquire of the mode is still ignored on the bridge)");
    const int32_t gameFullScreenMode = fullScreenMode != nullptr ? *fullScreenMode : 0;

    if (fullScreenMode != nullptr)
        *fullScreenMode = VkPresentBridge::kFullScreenExclusiveDisallowed;

    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
        result = o_CreateSwapchainKHR(device, pCreateInfo != nullptr ? &localCreateInfo : nullptr, pAllocator,
                                      pSwapchain);
    }

    if (fullScreenMode != nullptr)
        *fullScreenMode = gameFullScreenMode;

    if (bridged)
    {
        VkPresentBridge::OnSwapchainCreated(true, result, result == VK_SUCCESS ? *pSwapchain : VK_NULL_HANDLE);

        // The hidden window would not take it: the game's own window gets it, as it asked.
        if (result != VK_SUCCESS)
        {
            bridged = false;
            localCreateInfo = *pCreateInfo;
            localCreateInfo.imageUsage = native::VkFrameSource::SwapchainUsage(_PD, localCreateInfo);

            if (VkPresentBridge::Owns(pCreateInfo->oldSwapchain))
                localCreateInfo.oldSwapchain = VK_NULL_HANDLE;

            ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
            result = o_CreateSwapchainKHR(device, &localCreateInfo, pAllocator, pSwapchain);
        }
    }

    if (result == VK_SUCCESS && pCreateInfo != nullptr && pSwapchain != nullptr && *pSwapchain != VK_NULL_HANDLE &&
        !State::Instance().vulkanSkipHooks)
        native::VkFrameSource::NoteSwapchain(device, *pSwapchain, localCreateInfo);

    if (result == VK_SUCCESS && device != VK_NULL_HANDLE && pCreateInfo != nullptr && *pSwapchain != VK_NULL_HANDLE &&
        !State::Instance().vulkanSkipHooks)
    {
        State::Instance().screenWidth = static_cast<float>(pCreateInfo->imageExtent.width);
        State::Instance().screenHeight = static_cast<float>(pCreateInfo->imageExtent.height);

        // The same question the DXGI side asks: what does one unit of this buffer mean?
        //
        // EXTENDED_SRGB_LINEAR is scRGB, 1.0 = 80 nits. HDR10_ST2084 is PQ, 1.0 = 10000 nits. Both
        // are absolute, so in either the white point is arithmetic rather than a reading -- which
        // matters most for the games that supply no exposure texture, since nothing else answers for
        // them. Logged, not yet used.
        {
            static VkColorSpaceKHR lastSpace = (VkColorSpaceKHR) -1;

            if (pCreateInfo->imageColorSpace != lastSpace)
            {
                lastSpace = pCreateInfo->imageColorSpace;

                const char* name = "other";
                const char* meaning = "relative -- no scale to be had";

                switch (pCreateInfo->imageColorSpace)
                {
                case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
                    name = "scRGB (extended sRGB, linear)";
                    meaning = "absolute: 1.0 = 80 nits, so 203-nit paper white = 2.5375";
                    break;
                case VK_COLOR_SPACE_HDR10_ST2084_EXT:
                    name = "PQ / ST.2084 (HDR10)";
                    meaning = "absolute: 1.0 = 10000 nits, so 203-nit paper white = 0.0203";
                    break;
                case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
                    name = "sRGB (SDR)";
                    break;
                case VK_COLOR_SPACE_HDR10_HLG_EXT:
                    name = "HLG";
                    break;
                default:
                    break;
                }

                LOG_INFO("DLSS-NR: swapchain colour space {} -- {} ({}), format {}",
                         (int) pCreateInfo->imageColorSpace, name, meaning, (int) pCreateInfo->imageFormat);
            }
        }

        LOG_DEBUG("if (result == VK_SUCCESS && device != VK_NULL_HANDLE && pCreateInfo != nullptr && pSwapchain != "
                  "VK_NULL_HANDLE)");

        _device = device;
        LOG_DEBUG("_device captured: {0:X}", (UINT64) _device);

        // The menu is drawn on the D3D12 swapchain of the bridge, not on the hidden Vulkan one.
        if (!bridged)
            MenuOverlayVk::CreateSwapchain(device, _PD, _instance, _hwnd, pCreateInfo, pAllocator, pSwapchain);
        else
            MenuOverlayVk::DestroyVulkanObjects(false);
    }

    LOG_FUNC_RESULT(result);
    return result;
}

VALIDATE_HOOK(hkvkDestroySwapchainKHR, PFN_vkDestroySwapchainKHR)
static void hkvkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator)
{
    o_DestroySwapchainKHR(device, swapchain, pAllocator);

    if (swapchain != VK_NULL_HANDLE)
        VkPresentBridge::OnSwapchainDestroyed(device, swapchain);
}

VALIDATE_HOOK(hkvkDestroySurfaceKHR, PFN_vkDestroySurfaceKHR)
static void hkvkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* pAllocator)
{
    VkPresentBridge::ForgetSurface(surface);
    o_vkDestroySurfaceKHR(instance, surface, pAllocator);
}

// Neural Rendering's model, its parameter block and NGX itself are released while the device still lives -- NGX's
// order (release features, destroy parameters, Shutdown1, then the device) -- rather than abandoned with it.
VALIDATE_HOOK(hkvkDestroyDevice, PFN_vkDestroyDevice)
static void hkvkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
{
    // Not gated on the NR setting: switched off after running, NR still holds this device's handles. A lock and a
    // compare when NR never ran on it.
    if (device != VK_NULL_HANDLE)
        DlssNr::ShutdownVkForDevice(device, "the game is destroying the device NR runs on");

    if (device != VK_NULL_HANDLE)
        DlssNr::VkExt::ForgetDevice(device);

    if (device != VK_NULL_HANDLE)
    {
        VkPresentBridge::OnDeviceDestroying(device);
        NativeMotionVk::OnDeviceDestroyed(device);
        GenericDepthVk::OnDeviceDestroyed(device);
        native::lowlatency::OnDeviceReleasedVulkan(device);

        std::lock_guard lock(_timelineDevicesMutex);
        _timelineDevices.erase(device);
    }

    if (o_vkDestroyDevice != nullptr)
        o_vkDestroyDevice(device, pAllocator);

    if (device != VK_NULL_HANDLE)
        VkPresentBridge::OnDeviceDestroyed(device);
}

VALIDATE_HOOK(hkvkGetInstanceProcAddr, PFN_vkGetInstanceProcAddr)
PFN_vkVoidFunction hkvkGetInstanceProcAddr(VkInstance instance, const char* pName)
{
    auto orgFunc = o_vkGetInstanceProcAddr(instance, pName);

    if (orgFunc == VK_NULL_HANDLE)
        return VK_NULL_HANDLE;

    auto procName = std::string(pName);

    if (procName == std::string("vkCreateInstance"))
    {
        if (o_vkCreateInstance == nullptr)
            o_vkCreateInstance = (PFN_vkCreateInstance) orgFunc;

        LOG_DEBUG("vkCreateInstance");
        return (PFN_vkVoidFunction) hkvkCreateInstance;
    }
    else if (procName == std::string("vkCreateDevice"))
    {
        if (o_vkCreateDevice == nullptr)
            o_vkCreateDevice = (PFN_vkCreateDevice) orgFunc;

        LOG_DEBUG("vkCreateDevice");
        return (PFN_vkVoidFunction) hkvkCreateDevice;
    }
    else if (procName == std::string("vkDestroyDevice"))
    {
        // Hook() sets the trampoline first; the loader's own export is the detoured one and would call back here.
        if (o_vkDestroyDevice == nullptr && orgFunc != (PFN_vkVoidFunction) hkvkDestroyDevice)
            o_vkDestroyDevice = (PFN_vkDestroyDevice) orgFunc;

        return (PFN_vkVoidFunction) hkvkDestroyDevice;
    }

    if (auto lowLatency2 = HookLowLatency2(procName, orgFunc); lowLatency2 != nullptr)
        return lowLatency2;

    auto result = VulkanSpoofing::hkvkGetInstanceProcAddr(orgFunc, pName);
    if (result != VK_NULL_HANDLE)
        return result;

    return orgFunc;
}

VALIDATE_HOOK(hkvkGetDeviceProcAddr, PFN_vkGetDeviceProcAddr)
PFN_vkVoidFunction hkvkGetDeviceProcAddr(VkDevice device, const char* pName)
{
    auto orgFunc = o_vkGetDeviceProcAddr(device, pName);

    if (orgFunc == VK_NULL_HANDLE)
        return VK_NULL_HANDLE;

    auto procName = std::string(pName);

    if (procName == std::string("vkCreateInstance"))
    {
        if (o_vkCreateInstance == nullptr)
            o_vkCreateInstance = (PFN_vkCreateInstance) orgFunc;

        LOG_DEBUG("vkCreateInstance");
        return (PFN_vkVoidFunction) hkvkCreateInstance;
    }
    else if (procName == std::string("vkCreateDevice"))
    {
        if (o_vkCreateDevice == nullptr)
            o_vkCreateDevice = (PFN_vkCreateDevice) orgFunc;

        LOG_DEBUG("vkCreateDevice");
        return (PFN_vkVoidFunction) hkvkCreateDevice;
    }
    else if (procName == std::string("vkDestroyDevice"))
    {
        // Hook() sets the trampoline first; the loader's own export is the detoured one and would call back here.
        if (o_vkDestroyDevice == nullptr && orgFunc != (PFN_vkVoidFunction) hkvkDestroyDevice)
            o_vkDestroyDevice = (PFN_vkDestroyDevice) orgFunc;

        return (PFN_vkVoidFunction) hkvkDestroyDevice;
    }

    if (auto lowLatency2 = HookLowLatency2(procName, orgFunc); lowLatency2 != nullptr)
        return lowLatency2;

    auto result = VulkanSpoofing::hkvkGetDeviceProcAddr(orgFunc, pName);
    if (result != VK_NULL_HANDLE)
        return result;

    return orgFunc;
}

PFN_vkCreateWin32SurfaceKHR VulkanHooks::OriginalCreateWin32Surface()
{
    return o_vkCreateWin32SurfaceKHR != nullptr ? o_vkCreateWin32SurfaceKHR : &vkCreateWin32SurfaceKHR;
}

void VulkanHooks::Hook(HMODULE vulkan1)
{
    if (vulkanModule == nullptr)
        vulkanModule = vulkan1;

    VulkanSpoofing::HookForVulkanSpoofing(vulkan1);
    VulkanSpoofing::HookForVulkanExtensionSpoofing(vulkan1);
    VulkanSpoofing::HookForVulkanVRAMSpoofing(vulkan1);

    if (o_vkCreateDevice != nullptr)
        return;

    FARPROC address = nullptr;

    o_vkCreateDevice = (PFN_vkCreateDevice) KernelBaseProxy::GetProcAddress_()(vulkan1, "vkCreateDevice");
    o_vkCreateInstance = (PFN_vkCreateInstance) KernelBaseProxy::GetProcAddress_()(vulkan1, "vkCreateInstance");
    o_vkDestroyDevice = (PFN_vkDestroyDevice) KernelBaseProxy::GetProcAddress_()(vulkan1, "vkDestroyDevice");

    address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkGetInstanceProcAddr");
    o_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr) address;

    address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkGetDeviceProcAddr");
    o_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr) address;

    address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkCreateWin32SurfaceKHR");
    o_vkCreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR) address;

    address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkDestroySurfaceKHR");
    o_vkDestroySurfaceKHR = (PFN_vkDestroySurfaceKHR) address;

    // address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkCmdPipelineBarrier");
    // o_vkCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier) address;

    address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkGetPhysicalDeviceFeatures2");
    o_vkGetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2) address;

    address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkCreateSemaphore");
    o_vkCreateSemaphore = (PFN_vkCreateSemaphore) address;

    address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkSignalSemaphore");
    o_vkSignalSemaphore = (PFN_vkSignalSemaphore) address;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_vkCreateDevice != nullptr)
        DetourAttach(&(PVOID&) o_vkCreateDevice, hkvkCreateDevice);

    if (o_vkDestroyDevice != nullptr)
        DetourAttach(&(PVOID&) o_vkDestroyDevice, hkvkDestroyDevice);

    if (o_vkGetInstanceProcAddr != nullptr)
        DetourAttach(&(PVOID&) o_vkGetInstanceProcAddr, hkvkGetInstanceProcAddr);

    if (o_vkGetDeviceProcAddr != nullptr)
        DetourAttach(&(PVOID&) o_vkGetDeviceProcAddr, hkvkGetDeviceProcAddr);

    if (o_vkCreateInstance != nullptr)
        DetourAttach(&(PVOID&) o_vkCreateInstance, hkvkCreateInstance);

    if (o_vkCreateWin32SurfaceKHR != nullptr)
        DetourAttach(&(PVOID&) o_vkCreateWin32SurfaceKHR, hkvkCreateWin32SurfaceKHR);

    if (o_vkDestroySurfaceKHR != nullptr)
        DetourAttach(&(PVOID&) o_vkDestroySurfaceKHR, hkvkDestroySurfaceKHR);

    // if (o_vkCmdPipelineBarrier != nullptr)
    //     DetourAttach(&(PVOID&) o_vkCmdPipelineBarrier, hkvkCmdPipelineBarrier);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to hook Vulkan, error code: {:X}", detourResult);
        o_vkCreateDevice = nullptr;
        o_vkDestroyDevice = nullptr;
        o_vkCreateInstance = nullptr;
        o_vkGetInstanceProcAddr = nullptr;
        o_vkGetDeviceProcAddr = nullptr;
        o_vkCreateWin32SurfaceKHR = nullptr;
        // o_vkCmdPipelineBarrier = nullptr;
    }
}

void VulkanHooks::Unhook()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_QueuePresentKHR != nullptr)
        DetourDetach(&(PVOID&) o_QueuePresentKHR, hkvkQueuePresentKHR);

    if (o_CreateSwapchainKHR != nullptr)
        DetourDetach(&(PVOID&) o_CreateSwapchainKHR, hkvkCreateSwapchainKHR);

    if (o_DestroySwapchainKHR != nullptr)
        DetourDetach(&(PVOID&) o_DestroySwapchainKHR, hkvkDestroySwapchainKHR);

    if (o_AcquireFullScreenExclusiveModeEXT != nullptr)
        DetourDetach(&(PVOID&) o_AcquireFullScreenExclusiveModeEXT, hkvkAcquireFullScreenExclusiveModeEXT);

    if (o_ReleaseFullScreenExclusiveModeEXT != nullptr)
        DetourDetach(&(PVOID&) o_ReleaseFullScreenExclusiveModeEXT, hkvkReleaseFullScreenExclusiveModeEXT);

    if (o_vkDestroySurfaceKHR != nullptr)
        DetourDetach(&(PVOID&) o_vkDestroySurfaceKHR, hkvkDestroySurfaceKHR);

    if (o_vkCreateDevice != nullptr)
        DetourDetach(&(PVOID&) o_vkCreateDevice, hkvkCreateDevice);

    if (o_vkDestroyDevice != nullptr)
        DetourDetach(&(PVOID&) o_vkDestroyDevice, hkvkDestroyDevice);

    if (o_vkCreateInstance != nullptr)
        DetourDetach(&(PVOID&) o_vkCreateInstance, hkvkCreateInstance);

    if (o_vkCreateWin32SurfaceKHR != nullptr)
        DetourDetach(&(PVOID&) o_vkCreateWin32SurfaceKHR, hkvkCreateWin32SurfaceKHR);

    // if (o_vkCmdPipelineBarrier != nullptr)
    //     DetourDetach(&(PVOID&) o_vkCmdPipelineBarrier, hkvkCmdPipelineBarrier);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to unhook Vulkan, error code: {:X}", detourResult);
    }
    else
    {
        o_QueuePresentKHR = nullptr;
        o_CreateSwapchainKHR = nullptr;
        o_DestroySwapchainKHR = nullptr;
        o_AcquireFullScreenExclusiveModeEXT = nullptr;
        o_ReleaseFullScreenExclusiveModeEXT = nullptr;
        o_vkDestroySurfaceKHR = nullptr;
        o_vkCreateDevice = nullptr;
        o_vkDestroyDevice = nullptr;
        o_vkCreateInstance = nullptr;
        o_vkGetInstanceProcAddr = nullptr;
        o_vkGetDeviceProcAddr = nullptr;
        o_vkCreateWin32SurfaceKHR = nullptr;
        // o_vkCmdPipelineBarrier = nullptr;
    }
}
