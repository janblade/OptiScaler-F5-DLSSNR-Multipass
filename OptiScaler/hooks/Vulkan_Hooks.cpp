#include "pch.h"

#include "Vulkan_Hooks.h"

#include <Util.h>
#include <native/DriverInitGate.h>
#include <Config.h>
#include <SysUtils.h>

#include <menu/menu_overlay_vk.h>
#include <menu/menu_common.h>
#include <proxies/KernelBase_Proxy.h>
#include <upscaler_time/UpscalerTime_Vk.h>

#include <framegen/ffx/FSRFG_Vk.h>
#include <framegen/dlssg/DLSSG_Vk.h>
#include <fsr4/FSR4DriverProviderVk.h>
#include <proxies/FfxApi_Proxy.h>
#include <misc/FrameLimit.h>
#include "Reflex_Hooks.h"

#include <spoofing/Vulkan_Spoofing.h>
#include <hudfix/Hudfix_Vk.h>

#include <vulkan/vulkan.hpp>

#include <dlssnr/DlssNr_VkExtensions.h>
#include <dlssnr/DlssNrFeature_Vk.h>
#include <native/NativeDriverVk.h>
#include <native/NativeLowLatency.h>
#include <Logger.h>
#include <native/PresentStageTiming.h>
#include <native/VkFrameSource.h>
#include <native/VkPresentBridge.h>
#include <dlssnr/DlssNr_ExposureScan_Vk.h>
#include <resource_tracking/GenericDepth_Vk.h>

#include <detours/detours.h>
#include <misc/IdentifyGpu.h>

#include "Hook_Utils.h"

#include <low_latency/input/OutputRule.h>
#include <low_latency/input/VkLatencyMode.h>
#include <low_latency/input/input_antilag_vk.h>
#include <low_latency/input/input_reflex.h>
#include <low_latency/input/input_reflex_vk.h>
#include <low_latency/low_latency_tech/ll_reflex_nvapi_vk.h>

#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

// for menu rendering
static VkDevice _device = VK_NULL_HANDLE;
static VkInstance _instance = VK_NULL_HANDLE;
static VkPhysicalDevice _PD = VK_NULL_HANDLE;
static HWND _hwnd = nullptr;

static std::mutex _surfaceWindowsMutex;
static std::unordered_map<VkSurfaceKHR, HWND> _surfaceWindows;

static std::mutex _vkPresentMutex;

PFN_vkCreateDevice o_vkCreateDevice = nullptr;
static PFN_vkDestroyDevice o_vkDestroyDevice = nullptr;
PFN_vkCreateInstance o_vkCreateInstance = nullptr;
PFN_vkCreateWin32SurfaceKHR o_vkCreateWin32SurfaceKHR = nullptr;
PFN_vkQueuePresentKHR o_QueuePresentKHR = nullptr;
PFN_vkCreateSwapchainKHR o_CreateSwapchainKHR = nullptr;
static PFN_vkDestroySwapchainKHR o_DestroySwapchainKHR = nullptr;
static PFN_vkGetSwapchainImagesKHR o_GetSwapchainImagesKHR = nullptr;
static PFN_vkAcquireNextImageKHR o_AcquireNextImageKHR = nullptr;
static PFN_vkAcquireNextImage2KHR o_AcquireNextImage2KHR = nullptr;
static PFN_vkSetHdrMetadataEXT o_SetHdrMetadataEXT = nullptr;
static PFN_vkWaitForPresentKHR o_WaitForPresentKHR = nullptr;
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
PFN_vkSetLatencySleepModeNV VulkanHooks::o_vkSetLatencySleepModeNV = nullptr;
PFN_vkLatencySleepNV VulkanHooks::o_vkLatencySleepNV = nullptr;
PFN_vkSetLatencyMarkerNV VulkanHooks::o_vkSetLatencyMarkerNV = nullptr;
PFN_vkGetLatencyTimingsNV VulkanHooks::o_vkGetLatencyTimingsNV = nullptr;
PFN_vkQueueNotifyOutOfBandNV VulkanHooks::o_vkQueueNotifyOutOfBandNV = nullptr;

// Forward declaration
static VkResult hkvkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);
static VkResult hkvkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                                       const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain);
static void hkvkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator);
static VkResult hkvkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount,
                                          VkImage* pSwapchainImages);
static VkResult hkvkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                        VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex);
static VkResult hkvkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* pAcquireInfo,
                                         uint32_t* pImageIndex);
static void hkvkSetHdrMetadataEXT(VkDevice device, uint32_t swapchainCount, const VkSwapchainKHR* pSwapchains,
                                  const VkHdrMetadataEXT* pMetadata);
static VkResult hkvkWaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t presentId, uint64_t timeout);

// Set while the FG output handles a swapchain call
static thread_local bool _inFGCall = false;

struct ScopedFGCall
{
    ScopedFGCall() { _inFGCall = true; }
    ~ScopedFGCall() { _inFGCall = false; }
};

// FFX's and Streamline's swapchains call the driver through these hooks, those go straight to it.
// Streamline's DLSSG also presents the generated frames from its own thread.
static bool FromFGOutput(void* returnAddress)
{
    if (_inFGCall)
        return true;

    auto& state = State::Instance();
    return state.optiSlInterposer != nullptr && state.IsOptiStreamlineModule(Util::GetCallerModule(returnAddress));
}

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

// dxvk, vkd3d-proton and an nvapi call Vulkan's low latency for a D3D game's own Reflex, which is its D3D input
// already: a D3D runtime or nvapi module that calls Vulkan is one of those (the system's own never do). Never
// OptiScaler itself, which may be named dxgi.dll.
static bool D3DOnVulkanModule(HMODULE module)
{
    if (module == nullptr || module == dllModule)
        return false;

    static std::mutex cacheMutex;
    static std::unordered_map<HMODULE, bool> cache;

    std::scoped_lock lock(cacheMutex);

    if (auto it = cache.find(module); it != cache.end())
        return it->second;

    wchar_t path[MAX_PATH] {};
    GetModuleFileNameW(module, path, MAX_PATH);

    auto name = std::filesystem::path(path).filename().wstring();
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);

    const bool d3d = name == L"d3d9.dll" || name == L"d3d10core.dll" || name == L"d3d11.dll" || name == L"d3d12.dll" ||
                     name == L"d3d12core.dll" || name == L"dxgi.dll" || name == L"nvapi64.dll";
    cache[module] = d3d;
    return d3d;
}

// FG outputs present the real and the generated frames through this
static VkResult FGOutputQueuePresent(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
    MenuCommon::FGOutputPresent();
    return o_QueuePresentKHR(queue, pPresentInfo);
}

// OptiScaler's Streamline calls the driver's swapchain functions directly. Overlays like RTSS hook the same ones on top
// of OptiScaler, through them its calls, also those from its own present thread, would look like the game's.
static PFN_vkVoidFunction StreamlineSwapchainFunction(const char* pName, void* returnAddress)
{
    auto func = VulkanHooks::OriginalSwapchainFunction(pName);

    if (func == nullptr || !State::Instance().IsOptiStreamlineModule(Util::GetCallerModule(returnAddress)))
        return nullptr;

    return func;
}

// The game's own Vulkan low latency calls are InputCommon's inputs (low_latency/input): VK_NV_low_latency2's
// (InputReflexVk) and VK_AMD_anti_lag's (InputAntiLagVk). Handed out by vkGetInstanceProcAddr / vkGetDeviceProcAddr:
// the loader's own stubs for extension functions are too small to detour, so the game gets the input's function, and
// the driver's stays in VulkanHooks for the outputs (taken from here too, for a device made before our hooks). A D3D
// game's translation layer gets the driver's (D3DOnVulkanModule). Null for any other name.
static PFN_vkVoidFunction LowLatencyInputHook(const std::string& name, PFN_vkVoidFunction orgFunc, void* returnAddress)
{
    if (ScopedSkipVulkanHooks::Active())
        return nullptr;

    auto hook = [&]<typename PFN>(PFN& original, auto input) -> PFN_vkVoidFunction
    {
        if (original == nullptr)
            original = (PFN) orgFunc;

        if (D3DOnVulkanModule(Util::GetCallerModule(returnAddress)))
            return nullptr;

        return (PFN_vkVoidFunction) input;
    };

    if (name == "vkSetLatencySleepModeNV")
        return hook(VulkanHooks::o_vkSetLatencySleepModeNV, &InputReflexVk::SetLatencySleepMode);

    if (name == "vkLatencySleepNV")
        return hook(VulkanHooks::o_vkLatencySleepNV, &InputReflexVk::LatencySleep);

    if (name == "vkSetLatencyMarkerNV")
        return hook(VulkanHooks::o_vkSetLatencyMarkerNV, &InputReflexVk::SetLatencyMarker);

    if (name == "vkGetLatencyTimingsNV")
        return hook(VulkanHooks::o_vkGetLatencyTimingsNV, &InputReflexVk::GetLatencyTimings);

    if (name == "vkQueueNotifyOutOfBandNV")
        return hook(VulkanHooks::o_vkQueueNotifyOutOfBandNV, &InputReflexVk::QueueNotifyOutOfBand);

    if (name == "vkAntiLagUpdateAMD")
        return hook(VulkanHooks::o_vkAntiLagUpdateAMD, &InputAntiLagVk::AntiLagUpdate);

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

// The FG outputs sync with timeline semaphores, turns them on when the game didn't. Spoofing adds their extension.
// `added` gets chained into createInfo when the game has no feature struct for them, it has to outlive vkCreateDevice.
static bool EnableTimelineSemaphores(VkPhysicalDevice physicalDevice, VkDeviceCreateInfo& createInfo,
                                     VkPhysicalDeviceTimelineSemaphoreFeatures& added)
{
    if (TimelineFeatureOn(createInfo.pNext))
        return true;

    VkPhysicalDeviceTimelineSemaphoreFeatures supported {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES
    };
    VkPhysicalDeviceFeatures2 features { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &supported };
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features);

    if (!supported.timelineSemaphore)
        return false;

    // A feature struct of the game already listing it as off, otherwise a new one
    for (auto next = (VkBaseOutStructure*) createInfo.pNext; next != nullptr; next = next->pNext)
    {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
        {
            ((VkPhysicalDeviceVulkan12Features*) next)->timelineSemaphore = VK_TRUE;
            return true;
        }

        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)
        {
            ((VkPhysicalDeviceTimelineSemaphoreFeatures*) next)->timelineSemaphore = VK_TRUE;
            return true;
        }
    }

    added = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, (void*) createInfo.pNext, VK_TRUE };
    createInfo.pNext = &added;
    return true;
}

// Devices made with VK_NV_low_latency2 and timeline semaphores, not for a D3D device (VulkanHooks::LowLatency2On)
static std::mutex _lowLatencyMutex;
static std::set<VkDevice> _lowLatency2Devices;
// Each device's newest swapchain (LatencySwapchainOf): LatencySwapchainMutex, shared for a read or a call on it
// (VulkanHooks::LatencySwapchainUse), alone to change it (vkDestroySwapchainKHR waits for a call under way)
static std::map<VkDevice, VulkanHooks::LatencySwapchain> _latencySwapchains;

static std::shared_mutex& LatencySwapchainMutex()
{
    static std::shared_mutex mutex;
    return mutex;
}

bool VulkanHooks::LowLatency2On(VkDevice device)
{
    std::lock_guard lock(_lowLatencyMutex);
    return _lowLatency2Devices.contains(device);
}

VulkanHooks::LatencySwapchain VulkanHooks::LatencySwapchainOf(VkDevice device)
{
    std::shared_lock lock(LatencySwapchainMutex());
    auto it = _latencySwapchains.find(device);
    return it != _latencySwapchains.end() ? it->second : LatencySwapchain {};
}

VulkanHooks::LatencySwapchainUse::LatencySwapchainUse(VkDevice device) : _lock(LatencySwapchainMutex())
{
    if (auto it = _latencySwapchains.find(device); it != _latencySwapchains.end() && it->second.latencyMode)
        swapchain = it->second.swapchain;
}

void VulkanHooks::NoteLatencySwapchain(VkDevice device, VkSwapchainKHR swapchain)
{
    if (device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE)
        return;

    // FFX's handle of the native FG output's swapchain is no driver one, and its real one has no latency mode
    if (auto fg = State::Instance().currentVkFG;
        fg != nullptr && fg->OwnsSwapchain(swapchain) && !fg->HasDriverSwapchain())
    {
        return;
    }

    // Every call of the game's names it: the exclusive lock only when it is news
    if (auto known = LatencySwapchainOf(device); known.swapchain == swapchain && known.latencyMode)
        return;

    std::unique_lock lock(LatencySwapchainMutex());
    _latencySwapchains[device] = { swapchain, true };
}

PFN_vkVoidFunction VulkanHooks::GetInstanceProcAddr(VkInstance instance, const char* pName)
{
    if (o_vkGetInstanceProcAddr != nullptr)
        return o_vkGetInstanceProcAddr(instance, pName);

    return vkGetInstanceProcAddr(instance, pName);
}

PFN_vkVoidFunction VulkanHooks::OriginalSwapchainFunction(const char* pName)
{
    if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0)
        return (PFN_vkVoidFunction) o_CreateSwapchainKHR;

    if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0)
        return (PFN_vkVoidFunction) o_DestroySwapchainKHR;

    if (std::strcmp(pName, "vkGetSwapchainImagesKHR") == 0)
        return (PFN_vkVoidFunction) o_GetSwapchainImagesKHR;

    if (std::strcmp(pName, "vkAcquireNextImageKHR") == 0)
        return (PFN_vkVoidFunction) o_AcquireNextImageKHR;

    if (std::strcmp(pName, "vkQueuePresentKHR") == 0)
        return o_QueuePresentKHR != nullptr ? (PFN_vkVoidFunction) FGOutputQueuePresent : nullptr;

    return nullptr;
}

HWND VulkanHooks::SurfaceWindow(VkSurfaceKHR surface)
{
    std::scoped_lock lock(_surfaceWindowsMutex);

    auto it = _surfaceWindows.find(surface);
    return it != _surfaceWindows.end() ? it->second : nullptr;
}

// A promoted core function is missing on a device that only has its extension (RDR2's vkGetSemaphoreCounterValue and
// vkCmdPushDescriptorSetWithTemplate are null), the extension's name then gives the same function. FFX and OptiScaler
// ask for the core names.
PFN_vkVoidFunction VulkanHooks::GetDeviceProcAddr(VkDevice device, const char* name)
{
    if (o_vkGetDeviceProcAddr == nullptr)
        return nullptr;

    if (auto function = o_vkGetDeviceProcAddr(device, name))
        return function;

    if (name == nullptr)
        return nullptr;

    for (auto suffix : { "KHR", "EXT" })
    {
        const auto extensionName = std::string(name) + suffix;

        if (auto function = o_vkGetDeviceProcAddr(device, extensionName.c_str()))
        {
            LOG_DEBUG("{} is missing, using {}", name, extensionName);
            return function;
        }
    }

    return nullptr;
}

// A swapchain on a device with VK_NV_low_latency2 is made with its latency mode (low_latency/input/VkLatencyMode.h)
static bool ChainLatencyMode(VkDevice device, VkSwapchainCreateInfoKHR& info, VkSwapchainLatencyCreateInfoNV& latency)
{
    return lloutput::ChainLatencyMode(VulkanHooks::LowLatency2On(device), info, latency);
}

bool VulkanHooks::TimelineSemaphoresOn(VkDevice device)
{
    std::lock_guard lock(_timelineDevicesMutex);
    return _timelineDevices.contains(device);
}

// A device function past our hooks (the core name, else the KHR one), cached for the device asked last: the function
// and the device it was looked up on go together (any thread). One cache per function type.
template <typename PFN> static PFN CachedDeviceFunction(VkDevice device, const char* name, const char* khrName)
{
    static std::mutex cacheMutex;
    static PFN cached = nullptr;
    static VkDevice cachedDevice = VK_NULL_HANDLE;

    std::lock_guard lock(cacheMutex);

    if ((cached == nullptr || cachedDevice != device) && o_vkGetDeviceProcAddr != nullptr)
    {
        cached = reinterpret_cast<PFN>(o_vkGetDeviceProcAddr(device, name));

        if (cached == nullptr)
            cached = reinterpret_cast<PFN>(o_vkGetDeviceProcAddr(device, khrName));

        cachedDevice = device;
    }

    return cached;
}

bool VulkanHooks::WaitTimelineSemaphore(VkDevice device, VkSemaphore semaphore, uint64_t value, uint64_t timeoutNs)
{
    const auto wait = CachedDeviceFunction<PFN_vkWaitSemaphores>(device, "vkWaitSemaphores", "vkWaitSemaphoresKHR");

    if (wait == nullptr || semaphore == VK_NULL_HANDLE)
        return false;

    VkSemaphoreWaitInfo info {};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    info.semaphoreCount = 1;
    info.pSemaphores = &semaphore;
    info.pValues = &value;

    return wait(device, &info, timeoutNs) == VK_SUCCESS;
}

bool VulkanHooks::CanSignalTimelineSemaphore(VkDevice device)
{
    return CachedDeviceFunction<PFN_vkSignalSemaphore>(device, "vkSignalSemaphore", "vkSignalSemaphoreKHR") != nullptr;
}

bool VulkanHooks::SignalTimelineSemaphore(VkDevice device, VkSemaphore semaphore, uint64_t value)
{
    const auto signal =
        CachedDeviceFunction<PFN_vkSignalSemaphore>(device, "vkSignalSemaphore", "vkSignalSemaphoreKHR");

    if (signal == nullptr || semaphore == VK_NULL_HANDLE)
        return false;

    VkSemaphoreSignalInfo info {};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    info.semaphore = semaphore;
    info.value = value;

    return signal(device, &info) == VK_SUCCESS;
}

static void HookDevice(VkDevice InDevice)
{
    if (o_CreateSwapchainKHR != nullptr || ScopedSkipVulkanHooks::Active())
        return;

    LOG_FUNC();

    o_QueuePresentKHR = (PFN_vkQueuePresentKHR) (vkGetDeviceProcAddr(InDevice, "vkQueuePresentKHR"));
    o_CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR) (vkGetDeviceProcAddr(InDevice, "vkCreateSwapchainKHR"));
    o_DestroySwapchainKHR = (PFN_vkDestroySwapchainKHR) (vkGetDeviceProcAddr(InDevice, "vkDestroySwapchainKHR"));
    o_GetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR) (vkGetDeviceProcAddr(InDevice, "vkGetSwapchainImagesKHR"));
    o_AcquireNextImageKHR = (PFN_vkAcquireNextImageKHR) (vkGetDeviceProcAddr(InDevice, "vkAcquireNextImageKHR"));
    o_AcquireNextImage2KHR = (PFN_vkAcquireNextImage2KHR) (vkGetDeviceProcAddr(InDevice, "vkAcquireNextImage2KHR"));
    o_SetHdrMetadataEXT = (PFN_vkSetHdrMetadataEXT) (vkGetDeviceProcAddr(InDevice, "vkSetHdrMetadataEXT"));
    o_WaitForPresentKHR = (PFN_vkWaitForPresentKHR) (vkGetDeviceProcAddr(InDevice, "vkWaitForPresentKHR"));
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

        if (o_GetSwapchainImagesKHR != nullptr)
            DetourAttach(&(PVOID&) o_GetSwapchainImagesKHR, hkvkGetSwapchainImagesKHR);

        if (o_AcquireNextImageKHR != nullptr)
            DetourAttach(&(PVOID&) o_AcquireNextImageKHR, hkvkAcquireNextImageKHR);

        if (o_AcquireNextImage2KHR != nullptr)
            DetourAttach(&(PVOID&) o_AcquireNextImage2KHR, hkvkAcquireNextImage2KHR);

        if (o_SetHdrMetadataEXT != nullptr)
            DetourAttach(&(PVOID&) o_SetHdrMetadataEXT, hkvkSetHdrMetadataEXT);

        if (o_WaitForPresentKHR != nullptr)
            DetourAttach(&(PVOID&) o_WaitForPresentKHR, hkvkWaitForPresentKHR);

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
            o_GetSwapchainImagesKHR = nullptr;
            o_AcquireNextImageKHR = nullptr;
            o_AcquireNextImage2KHR = nullptr;
            o_SetHdrMetadataEXT = nullptr;
            o_WaitForPresentKHR = nullptr;
        }
    }
}

// Created with the device as the output's queues have to be added to it. This is the native Vulkan FG output, FFX's
// swapchain in place of the game's; a game that takes the present bridge (native/VkPresentBridge.h) keeps its own swapchain
// on a hidden window and its frame generation is the D3D12 outputs'.
static IFGFeature_Vk* CreateFGFeature()
{
    auto& state = State::Instance();

    const bool supportedInput = state.activeFgInput == FGInput::DLSSG || state.activeFgInput == FGInput::Upscaler;

    if (state.currentVkFG != nullptr || !supportedInput)
        return state.currentVkFG;

    if (state.activeFgOutput == FGOutput::FSRFG)
    {
        // Driver provided ML FG works without amd_fidelityfx_vk.dll
        const bool sdkLoaded = FfxApiProxy::InitFfxVk();
        const bool driverLoaded = FSR4DriverProviderVk::InitFg();

        if (sdkLoaded || driverLoaded)
            state.currentVkFG = new FSRFG_Vk();
        else
            LOG_ERROR("Can't load amd_fidelityfx_vk.dll, FSR FG won't be available");
    }
    else if (state.activeFgOutput == FGOutput::DLSSG)
    {
        if (StreamlineProxy::InitWithVulkan())
            state.currentVkFG = new DLSSG_Vk();
        else
            LOG_ERROR("Can't init Streamline, DLSSG won't be available");
    }

    return state.currentVkFG;
}

VALIDATE_HOOK(hkvkCreateWin32SurfaceKHR, PFN_vkCreateWin32SurfaceKHR)
static VkResult hkvkCreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
                                          const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface)
{
    if (VulkanHooks::existingSurface != VK_NULL_HANDLE)
    {
        *pSurface = VulkanHooks::existingSurface;
        return VK_SUCCESS;
    }

    LOG_FUNC();

    auto result = o_vkCreateWin32SurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);

    if (result == VK_SUCCESS && pSurface != nullptr)
    {
        std::scoped_lock lock(_surfaceWindowsMutex);
        _surfaceWindows[*pSurface] = pCreateInfo->hwnd;
    }

    auto procHwnd = Util::GetProcessWindow();
    LOG_DEBUG("procHwnd: {0:X}, swapchain hwnd: {1:X}", (UINT64) procHwnd, (UINT64) pCreateInfo->hwnd);

    if (result == VK_SUCCESS && !ScopedSkipVulkanHooks::Active())
    {
        _instance = instance;
        State::Instance().VulkanInstance = instance;
        LOG_DEBUG("_instance captured: {0:X}", (UINT64) _instance);

        if (!MenuOverlayVk::OnOtherWindow(pCreateInfo->hwnd))
        {
            MenuOverlayVk::DestroyVulkanObjects(false);

            _hwnd = pCreateInfo->hwnd;
            LOG_DEBUG("_hwnd captured: {0:X}", (UINT64) _hwnd);
        }
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

    // FFX and Streamline use Vulkan 1.2 core functions like vkWaitSemaphores. Wine only provides those to 1.2 apps,
    // older ones crash calling them, Streamline's own vkCreateInstance raises the version too.
    VkApplicationInfo appInfo { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    const auto fgOutput = State::Instance().activeFgOutput;

    if ((fgOutput == FGOutput::FSRFG || fgOutput == FGOutput::DLSSG) && !ScopedSkipVulkanHooks::Active() &&
        !VkPresentBridge::DxvkGame())
    {
        if (localCreateInfo.pApplicationInfo != nullptr)
            appInfo = *localCreateInfo.pApplicationInfo;

        if (appInfo.apiVersion < VK_API_VERSION_1_2)
        {
            LOG_INFO("Raising the API version from {}.{} to 1.2 for FG", VK_API_VERSION_MAJOR(appInfo.apiVersion),
                     VK_API_VERSION_MINOR(appInfo.apiVersion));

            appInfo.apiVersion = VK_API_VERSION_1_2;
            localCreateInfo.pApplicationInfo = &appInfo;
        }
    }

    VkResult result;
    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
        result = o_vkCreateInstance(&localCreateInfo, pAllocator, pInstance);
    }

    // OptiScaler's own Streamline only makes short lived ones
    if (result == VK_SUCCESS && !StreamlineProxy::IsInitializing())
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
    // if (result == VK_SUCCESS && !ScopedSkipVulkanHooks::Active())
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

    // Not at the same moment as DLSS-NR's feature create: the driver can freeze the two against each other
    native::DriverInitScope driverInit;

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

    // Not for the D3D layers' own devices (dxvk, vkd3d-proton): their games present through DXGI. Before spoofing,
    // which drops the extensions it only pretends to have.
    IFGFeature_Vk* nativeFg = nullptr;
    VkPhysicalDeviceTimelineSemaphoreFeatures nativeFgTimeline {};

    if (!ScopedSkipVulkanHooks::Active() && !D3DOnVulkanModule(Util::GetCallerModule(_ReturnAddress())) &&
        !VkPresentBridge::DxvkGame() && !VkPresentBridge::ModeWantsBridge() &&
        (nativeFg = CreateFGFeature()) != nullptr)
    {
        if (EnableTimelineSemaphores(physicalDevice, localCreteInfo, nativeFgTimeline))
        {
            nativeFg->PrepareDevice(physicalDevice, localCreteInfo);
        }
        else
        {
            LOG_WARN("Timeline semaphores aren't supported, can't use FG with this device");
            nativeFg = nullptr;
        }
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
    VkBool32* nrBorrowedPushFlag = nullptr;  // and for Vulkan 1.4's pushDescriptor
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

            // With Vulkan 1.4's feature struct in the chain, VK_KHR_push_descriptor may only be named with its
            // pushDescriptor on there. When it was added here (not the game's own), the flag is set for the call and
            // put back after, like the address flag below; where that is not possible the extension goes again.
            if (!DlssNr::VkExt::ListHas(gameNames, gameCount, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) &&
                DlssNr::VkExt::ListHas(localCreteInfo.ppEnabledExtensionNames, localCreteInfo.enabledExtensionCount,
                                       VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME))
            {
                for (auto* node = (VkBaseOutStructure*) localCreteInfo.pNext; node != nullptr; node = node->pNext)
                {
                    if (node->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES)
                        continue;

                    VkBool32* flag = &((VkPhysicalDeviceVulkan14Features*) node)->pushDescriptor;

                    if (*flag)
                        break;

                    VkPhysicalDeviceVulkan14Features offered {};
                    offered.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
                    VkPhysicalDeviceFeatures2 query {};
                    query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                    query.pNext = &offered;

                    if (o_vkGetPhysicalDeviceFeatures2)
                        o_vkGetPhysicalDeviceFeatures2(physicalDevice, &query);

                    if (offered.pushDescriptor && DlssNr::VkExt::IsWritable(flag))
                    {
                        *flag = VK_TRUE;
                        nrBorrowedPushFlag = flag;
                        LOG_INFO("DLSS-NR Vulkan: pushDescriptor switched on in the game's Vulkan 1.4 features for the "
                                 "create call");
                    }
                    else
                    {
                        std::erase_if(nrExtensions.names, [](const char* name)
                                      { return std::strcmp(name, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) == 0; });
                        localCreteInfo.ppEnabledExtensionNames = nrExtensions.names.data();
                        localCreteInfo.enabledExtensionCount = (uint32_t) nrExtensions.names.size();
                        LOG_WARN("DLSS-NR Vulkan: VK_KHR_push_descriptor left out -- the game's Vulkan 1.4 features "
                                 "have pushDescriptor off and it cannot be switched on here");
                    }

                    break;
                }
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

    // VK_NV_low_latency2 (VulkanSpoofing adds it on NVIDIA) signals a timeline semaphore in its sleep: InputCommon's
    // Reflex output needs the feature too. Not on a device dxvk or vkd3d-proton makes for a D3D device: those run the
    // D3D game's own Reflex.
    const bool lowLatency2Asked =
        !ScopedCreatingD3DDevice::Active() &&
        DlssNr::VkExt::ListHas(localCreteInfo.ppEnabledExtensionNames, localCreteInfo.enabledExtensionCount,
                               VK_NV_LOW_LATENCY_2_EXTENSION_NAME);

    if (Config::Instance()->DlssNrEnabled.value_or_default() || lowLatency2Asked)
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

        LOG_INFO("Vulkan: timelineSemaphore feature {} (Optical F5Low, Reflex)", what);
    }

    // VK_AMD_anti_lag (added by VulkanSpoofing when the device offers it) does nothing until its antiLag feature is
    // switched on too: vkAntiLagUpdateAMD needs it, and without it Anti-Lag Vk neither waited nor kept the frame cap
    // (AMD tester, dxvk on the Vulkan bridge, XeFG with Anti-Lag 2 as the Output: the same frame rate under every cap).
    // Where the game already chains the feature struct, the flag is set in it for the call and put back after; otherwise
    // ours goes at the head of the chain.
    VkPhysicalDeviceAntiLagFeaturesAMD antiLagFeature {};
    VkBool32* antiLagBorrowedFlag = nullptr;

    if (State::Instance().vkAntiLagSupported &&
        DlssNr::VkExt::ListHas(localCreteInfo.ppEnabledExtensionNames, localCreteInfo.enabledExtensionCount,
                               VK_AMD_ANTI_LAG_EXTENSION_NAME))
    {
        const char* what = nullptr;

        for (auto* node = (VkBaseOutStructure*) localCreteInfo.pNext; node != nullptr && what == nullptr;
             node = node->pNext)
        {
            if (node->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD)
                continue;

            auto* flag = &((VkPhysicalDeviceAntiLagFeaturesAMD*) node)->antiLag;

            if (*flag)
                what = "already on (the game's own)";
            else if (!DlssNr::VkExt::IsWritable(flag))
                what = "left off: the game's feature struct is read-only";
            else
            {
                *flag = VK_TRUE;
                antiLagBorrowedFlag = flag;
                what = "switched on in the game's feature struct for the create call";
            }
        }

        if (what == nullptr)
        {
            antiLagFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD;
            antiLagFeature.antiLag = VK_TRUE;
            antiLagFeature.pNext = (void*) localCreteInfo.pNext;
            localCreteInfo.pNext = &antiLagFeature;
            what = "switched on here";
        }

        LOG_INFO("Vulkan Anti-Lag: antiLag feature {}", what);
    }

    auto result = o_vkCreateDevice(physicalDevice, &localCreteInfo, pAllocator, pDevice);

    if (result == VK_SUCCESS && nativeFg != nullptr)
    {
        nativeFg->OnDeviceCreated(*pDevice);
        HookDevice(*pDevice);
        Hudfix_Vk::HookDevice(*pDevice);
    }

    if (antiLagBorrowedFlag != nullptr)
        *antiLagBorrowedFlag = VK_FALSE;

    // Whether the call had the timeline semaphore feature on (read from the create info, which is the game's own again
    // now that the borrowed Anti-Lag flag above is put back)
    if (result == VK_SUCCESS && pDevice != nullptr && *pDevice != VK_NULL_HANDLE &&
        TimelineFeatureOn(localCreteInfo.pNext))
    {
        std::lock_guard lock(_timelineDevicesMutex);
        _timelineDevices.insert(*pDevice);
    }

    // VK_NV_low_latency2 with its timeline semaphores: the driver's functions for InputCommon's Reflex output
    if (result == VK_SUCCESS && pDevice != nullptr && *pDevice != VK_NULL_HANDLE && lowLatency2Asked &&
        TimelineFeatureOn(localCreteInfo.pNext) && o_vkGetDeviceProcAddr != nullptr)
    {
        auto get = [&](const char* name) { return o_vkGetDeviceProcAddr(*pDevice, name); };

        if (auto setSleepMode = (PFN_vkSetLatencySleepModeNV) get("vkSetLatencySleepModeNV"))
        {
            VulkanHooks::o_vkSetLatencySleepModeNV = setSleepMode;
            VulkanHooks::o_vkLatencySleepNV = (PFN_vkLatencySleepNV) get("vkLatencySleepNV");
            VulkanHooks::o_vkSetLatencyMarkerNV = (PFN_vkSetLatencyMarkerNV) get("vkSetLatencyMarkerNV");
            VulkanHooks::o_vkGetLatencyTimingsNV = (PFN_vkGetLatencyTimingsNV) get("vkGetLatencyTimingsNV");
            VulkanHooks::o_vkQueueNotifyOutOfBandNV = (PFN_vkQueueNotifyOutOfBandNV) get("vkQueueNotifyOutOfBandNV");

            std::lock_guard lock(_lowLatencyMutex);
            _lowLatency2Devices.insert(*pDevice);
            LOG_INFO("Vulkan Reflex: VK_NV_low_latency2 is on for this device");
        }
    }

    if (nativeBorrowedTimeline != nullptr)
        *nativeBorrowedTimeline = VK_FALSE;

    if (result == VK_SUCCESS && pDevice != nullptr && *pDevice != VK_NULL_HANDLE)
    {
        native::VkFrameSource::NoteDevice(physicalDevice, *pDevice, localCreteInfo);
        GenericDepthVk::OnDevice(*pDevice, physicalDevice);
    }

    if (nrBorrowedFlag != nullptr)
        *nrBorrowedFlag = VK_FALSE;

    if (nrBorrowedWriteFlag != nullptr)
        *nrBorrowedWriteFlag = VK_FALSE;

    if (nrBorrowedPushFlag != nullptr)
        *nrBorrowedPushFlag = VK_FALSE;

    if (result == VK_SUCCESS && pDevice != nullptr)
        DlssNr::VkExt::NoteDevice(*pDevice, nrWritesWithoutFormat);

    if (Config::Instance()->DlssNrEnabled.value_or_default())
        LOG_INFO("DLSS-NR Vulkan: vkCreateDevice returned {} with {} extensions requested", (int) result,
                 localCreteInfo.enabledExtensionCount);

    if (result == VK_SUCCESS && Config::Instance()->OverlayMenu.value_or_default())
    {
        if (!ScopedSkipVulkanHooks::Active())
        {
            // Disabled to prevent unnecessary object release
            // MenuOverlayVk::DestroyVulkanObjects(false);

            _PD = physicalDevice;
            LOG_DEBUG("_PD captured: {0:X}", (UINT64) _PD);
            _device = *pDevice;
            LOG_DEBUG("_device captured: {0:X}", (UINT64) _device);
            HookDevice(_device);
            MenuOverlayVk::DeviceCreated(_device, &localCreteInfo);
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
    }
}

VALIDATE_HOOK(hkvkQueuePresentKHR, PFN_vkQueuePresentKHR)
static VkResult hkvkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
    auto nativeFg = State::Instance().currentVkFG;

    // FFX presents the real swapchain from its own queue
    if (FromFGOutput(_ReturnAddress()) || (nativeFg != nullptr && nativeFg->OwnsQueue(queue)))
        return FGOutputQueuePresent(queue, pPresentInfo);

    LOG_FUNC();

    using native::presenttiming::Stage;
    const auto hookStartMs = Util::MillisecondsNow();

    State::Instance().vulkanPresentCount.fetch_add(1, std::memory_order_relaxed);
    NoteDroppedLogLinesOnPresent();

    // The device this present is on: the queue's own, not the last one the game made. A game that makes more devices after
    // its swapchain (probes) leaves _device and _PD on one of those.
    VkDevice presentDevice = _device;
    VkPhysicalDevice presentPhysical = _PD;
    native::VkFrameSource::DeviceOfQueue(queue, &presentDevice, &presentPhysical);

    // get upscaler time (no device when the queue is unknown and the last device made is gone)
    if (presentDevice != VK_NULL_HANDLE)
        UpscalerTimeVk::ReadUpscalingTime(presentDevice);

    // ??? TODO: if we are hooking dxvk's vulkan calls then this present call could be either coming from dxvk or from a
    // native vk game
    if (!VkPresentBridge::DxvkGame())
        State::Instance().swapchainApi = Vulkan;

    // Tick feature to let it know if it's frozen
    if (auto currentFeature = State::Instance().currentFeature; currentFeature != nullptr)
    {
        if (auto currentFg = State::Instance().CurrentFG(); currentFg != nullptr)
            currentFeature->TickFrozenCheck(currentFg->GetInterpolatedFrameCount());
        else
            currentFeature->TickFrozenCheck();
    }

    // Ends the hudless search before the menu renders to the swapchain
    if (nativeFg != nullptr)
        Hudfix_Vk::PresentStart();

    VkPresentInfoKHR localPresentInfo {};
    memcpy(&localPresentInfo, pPresentInfo, sizeof(VkPresentInfoKHR));

    // The game's swapchain is on the bridge's hidden window: the menu and the frame limiter are the D3D12 swapchain's
    const bool bridged = pPresentInfo->swapchainCount == 1 && VkPresentBridge::Active(pPresentInfo->pSwapchains[0]);

    // dxvk with the bridge up: this frame's entry in the ledger the game's D3D11 Present fills (native/NativeLowLatency.h),
    // taken before anything touches the frame: XeFG takes the frame's ID at its first tagged resource, which happens in
    // NativeMotionVk::OnPresent below, and the ID has to be this frame's by then. The same call publishes the devices the
    // game's Present may run the Reflex pair on.
    const bool dxvkBridged = bridged && VkPresentBridge::DxvkThroughVulkan();
    bool pairHere = true;

    if (dxvkBridged)
    {
        pairHere = native::lowlatency::DxvkVulkanPresent(
            presentDevice, static_cast<IUnknown*>(VkPresentBridge::Device()), VkPresentBridge::OutputGeneration());
    }

    // Optical F5Low: the picture through NR before the menu is drawn over it. May replace the wait list.
    const auto motionStartMs = Util::MillisecondsNow();
    NativeMotionVk::OnPresent(queue, &localPresentInfo, presentDevice, presentPhysical);
    _presentTiming.Record(Stage::NativeMotion, Util::MillisecondsNow() - motionStartMs);

    // render menu if needed
    if (!bridged && !MenuOverlayVk::QueuePresent(queue, &localPresentInfo))
    {
        LOG_ERROR("QueuePresent: false!");
        NotePresentTiming(hookStartMs);
        return VK_ERROR_OUT_OF_DATE_KHR;
    }

    // The game's swapchain is the native FG output's (FFX's swapchain): the frame goes to FSR FG before the real present
    const bool nativeFgPresent = !bridged && nativeFg != nullptr && localPresentInfo.swapchainCount == 1 &&
                                 nativeFg->OwnsSwapchain(localPresentInfo.pSwapchains[0]);

    MenuCommon::FGOutputPresents(nativeFgPresent && nativeFg->IsActive() && !nativeFg->IsPaused());

    if (nativeFgPresent)
    {
        auto& state = State::Instance();
        state.fgLastFrame++;

        static double lastPresentTime = 0.0;
        auto now = Util::MillisecondsNow();
        state.lastFGFrameTime = lastPresentTime > 0.0 ? now - lastPresentTime : 0.0;
        lastPresentTime = now;

        if (nativeFg->IsActive() && !nativeFg->IsPaused())
            nativeFg->Present();
    }

    ReflexHooks::presentUpdate(lloutput::PresentSite::Vulkan, false);

    // Optical F5Low's low latency (native/NativeLowLatency.h): Reflex markers around the game's present. This call is the
    // game's own present whether or not the frame-generation bridge is up -- bridged, it is on the hidden window
    // (native/VkPresentBridge.h); the D3D12 swapchain's own present, through FGHooks, is a separate thing frame generation
    // drives on its own thread, which stays out of it. Bridged, the markers and the sleep go through the D3D entry points
    // on the bridge's private D3D12 device -- the same relationship Dx11wDx12SC::Present has to its hidden D3D11 present
    // -- so XeFG's XeLL routing (ReflexHooks) reaches them; the Vulkan NVAPI belongs to a game with no bridge. Not under
    // dxvk unbridged: these are dxvk's presents, not the game's. Bridged (NativeDxvkVulkan), it is the same as a native
    // Vulkan game: the game's wrapped D3D11 present stands aside for dxvk (wrapped_swapchain.cpp), so nothing else sends them.
    // With XeFG's Output on Reflex or Anti-Lag 2 the latency is lowered on the game's VkDevice instead (the Vulkan
    // surface: the driver's Reflex, or Anti-Lag Vk); XeLL on the bridge's device only keeps XeFG running
    // (native/XefgReflexRule.h)
    const bool splitOnVulkan = bridged && native::lowlatency::XefgSide() != native::xefgreflex::XefgLatency::XeLL;
    const auto lowLatencyTarget =
        native::lowlatency::PresentTarget(true, bridged, presentDevice,
                                          bridged ? static_cast<IUnknown*>(VkPresentBridge::Device()) : nullptr,
                                          splitOnVulkan);
    // dxvk calls this present from its own thread, not the game's, so a Sleep here only paces dxvk and the game runs
    // ahead until dxvk's frame latency limit stops it. By default (NativeLowLatencyDxvkGameThread) the game's own D3D11
    // Present runs the pair instead (wrapped/wrapped_swapchain.cpp), and says so in this frame's ledger entry:
    // `pairHere` is false for those frames. It is this frame's own entry, not the current setting, that decides: a
    // frame the game's Present already paired is never paired again here, and a frame it left to this present is never
    // left unpaired. Unverified, to check in a game: with the pair at the game's Present, PRESENT_START/END (and
    // Anti-Lag Vk's present stage call) no longer bracket the real vkQueuePresentKHR, which dxvk makes up to its frame
    // latency later on its own thread, so the driver's Reflex report and the AMD pairing may read differently. Both
    // sites go through InputCommon's output: NvAPI_D3D_* on the bridge's D3D12 device, NvAPI_Vulkan_* and
    // vkWaitSemaphores on the VkDevice, or VK_NV_low_latency2 on the device's swapchain with latency mode. The Vulkan
    // registry (vk.xml) marks no parameter of any of them externally synchronised, so none has to stay on dxvk's
    // thread; the swapchain's destroy waits for a VK_NV_low_latency2 call under way (VulkanHooks::LatencySwapchainUse).
    const bool lowLatency =
        (!VkPresentBridge::DxvkGame() || dxvkBridged) && lowLatencyTarget.device != nullptr && pairHere;

    const auto lowLatencyBegin = [&]
    {
        native::lowlatency::DxvkSiteLock siteLock(dxvkBridged);

        if (lowLatencyTarget.api == native::lowlatency::Api::Vulkan)
            native::lowlatency::OnPresentBeginVulkan(presentDevice);
        else
            native::lowlatency::OnPresentBegin(
                const_cast<IUnknown*>(static_cast<const IUnknown*>(lowLatencyTarget.device)));
    };
    const auto lowLatencyEnd = [&]
    {
        native::lowlatency::DxvkSiteLock siteLock(dxvkBridged);

        if (lowLatencyTarget.api == native::lowlatency::Api::Vulkan)
            native::lowlatency::OnPresentEndVulkan(presentDevice);
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
        if (nativeFgPresent)
        {
            ScopedFGCall fgCall {};
            result = nativeFg->QueuePresent(queue, &localPresentInfo);
        }
        else
        {
            result = o_QueuePresentKHR(queue, &localPresentInfo);
        }
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
    else
    {
        // "NR + frame generation" was switched on after the game made its swapchain: the bridge is only made
        // with a swapchain, so the game is told to make a new one (once).
        if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && pPresentInfo->swapchainCount == 1 &&
            VkPresentBridge::WantsNewSwapchain(pPresentInfo->pSwapchains[0]))
            result = VK_ERROR_OUT_OF_DATE_KHR;

        // Unsure about Vulkan Reflex fps limit and if that could be causing an issue here
        if (!State::Instance().reflexLimitsFps && !native::lowlatency::OwnsFrameCap())
            FrameLimit::sleep(nativeFgPresent
                                  ? native::lowlatency::ShownPerRendered(nativeFg->IsGenerating(),
                                                                         nativeFg->GetInterpolatedFrameCount())
                                  : 1);
    }

    NotePresentTiming(hookStartMs);

    LOG_FUNC_RESULT(result);
    return result;
}

VALIDATE_HOOK(hkvkCreateSwapchainKHR, PFN_vkCreateSwapchainKHR)
static VkResult hkvkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                                       const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
{
    // The swapchain create of FFX or Streamline while it makes the FG one
    if (FromFGOutput(_ReturnAddress()))
        return o_CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);

    LOG_FUNC();

    ScopedVulkanCreatingSC scopedVulkanCreatingSC {};
    VkResult result = VK_SUCCESS;

    // Optical F5Low copies the swapchain's images to and from its D3D12 device: TRANSFER usage is added when the surface
    // allows it (native/VkFrameSource.cpp).
    VkSwapchainCreateInfoKHR localCreateInfo {};

    const auto swapchainPhysicalDevice = [&]
    {
        const VkPhysicalDevice physical = native::VkFrameSource::PhysicalDeviceOf(device);
        return physical != VK_NULL_HANDLE ? physical : _PD;
    };

    // With frame generation chosen the swapchain is made on a hidden window and the real one gets a D3D12 swapchain
    // (native/VkPresentBridge.h). The usage is then asked of the hidden surface.
    bool bridged = false;

    if (pCreateInfo != nullptr)
    {
        localCreateInfo = *pCreateInfo;

        if (!ScopedSkipVulkanHooks::Active())
        {
            // The physical device this swapchain's own device was made on (not the last one the game made)
            const VkPhysicalDevice physical = swapchainPhysicalDevice();
            bridged = VkPresentBridge::OnCreateSwapchain(_instance, physical, device, *pCreateInfo, &localCreateInfo);
            localCreateInfo.imageUsage = native::VkFrameSource::SwapchainUsage(physical, localCreateInfo);
        }
    }

    // The native FG output (FFX's swapchain in place of the game's) when the game does not take the bridge
    IFGFeature_Vk* nativeFg = State::Instance().currentVkFG;
    const bool nativeFgSwapchain = nativeFg != nullptr && pCreateInfo != nullptr && !bridged &&
                                   !ScopedSkipVulkanHooks::Active() && !VkPresentBridge::DxvkGame() &&
                                   !D3DOnVulkanModule(Util::GetCallerModule(_ReturnAddress()));

    // InputCommon's Reflex output on VK_NV_low_latency2 needs the swapchain's latency mode. Not on FFX's swapchain: the
    // game's handle of it is no driver one, and FFX's real one has none.
    VkSwapchainLatencyCreateInfoNV latencyInfo {};
    bool latencyMode = false;

    if (pCreateInfo != nullptr && !ScopedSkipVulkanHooks::Active() && !nativeFgSwapchain)
        latencyMode = ChainLatencyMode(device, localCreateInfo, latencyInfo);

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
        bool fgSwapchain = false;

        if (nativeFgSwapchain)
        {
            ScopedFGCall fgCall {};
            fgSwapchain = nativeFg->CreateSwapchain(device, pCreateInfo, pAllocator, pSwapchain);
            result = VK_SUCCESS;

            // FFX could not make it: the game's own swapchain, as without the output
            if (!fgSwapchain)
                latencyMode = ChainLatencyMode(device, localCreateInfo, latencyInfo);
        }

        if (!fgSwapchain)
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
            localCreateInfo.imageUsage =
                native::VkFrameSource::SwapchainUsage(swapchainPhysicalDevice(), localCreateInfo);

            if (VkPresentBridge::Owns(pCreateInfo->oldSwapchain))
                localCreateInfo.oldSwapchain = VK_NULL_HANDLE;

            latencyMode = ChainLatencyMode(device, localCreateInfo, latencyInfo);

            ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
            result = o_CreateSwapchainKHR(device, &localCreateInfo, pAllocator, pSwapchain);
        }
    }

    if (result == VK_SUCCESS && pCreateInfo != nullptr && pSwapchain != nullptr && *pSwapchain != VK_NULL_HANDLE &&
        !ScopedSkipVulkanHooks::Active())
    {
        native::VkFrameSource::NoteSwapchain(device, *pSwapchain, localCreateInfo);

        std::unique_lock lock(LatencySwapchainMutex());
        _latencySwapchains[device] = { *pSwapchain, latencyMode };
    }

    if (result == VK_SUCCESS && device != VK_NULL_HANDLE && pCreateInfo != nullptr && *pSwapchain != VK_NULL_HANDLE &&
        !ScopedSkipVulkanHooks::Active())
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

        // The menu is drawn on the D3D12 swapchain of the bridge, not on the hidden Vulkan one. Before that swapchain's
        // first present, which is where the D3D12 menu starts.
        if (!bridged)
        {
            const auto surfaceWindow = VulkanHooks::SurfaceWindow(pCreateInfo->surface);
            MenuOverlayVk::CreateSwapchain(device, swapchainPhysicalDevice(), _instance,
                                           surfaceWindow != nullptr ? surfaceWindow : _hwnd, pCreateInfo, pAllocator,
                                           pSwapchain);
        }
        else
            MenuOverlayVk::HandOverToBridge();
    }

    LOG_FUNC_RESULT(result);
    return result;
}

VALIDATE_HOOK(hkvkDestroySwapchainKHR, PFN_vkDestroySwapchainKHR)
static void hkvkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator)
{
    if (FromFGOutput(_ReturnAddress()))
    {
        o_DestroySwapchainKHR(device, swapchain, pAllocator);
        return;
    }

    // Before it goes: InputCommon's Reflex output stops using it, after a call of it that is under way
    // (VulkanHooks::LatencySwapchainUse)
    if (swapchain != VK_NULL_HANDLE)
    {
        std::unique_lock lock(LatencySwapchainMutex());

        if (auto it = _latencySwapchains.find(device);
            it != _latencySwapchains.end() && it->second.swapchain == swapchain)
            it->second = {};
    }

    if (auto fg = State::Instance().currentVkFG; fg != nullptr)
    {
        ScopedFGCall fgCall {};

        if (fg->DestroySwapchain(device, swapchain, pAllocator))
            return;
    }

    o_DestroySwapchainKHR(device, swapchain, pAllocator);

    if (swapchain != VK_NULL_HANDLE)
        VkPresentBridge::OnSwapchainDestroyed(device, swapchain);
}

VALIDATE_HOOK(hkvkGetSwapchainImagesKHR, PFN_vkGetSwapchainImagesKHR)
static VkResult hkvkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount,
                                          VkImage* pSwapchainImages)
{
    if (auto fg = State::Instance().currentVkFG;
        fg != nullptr && fg->OwnsSwapchain(swapchain) && !FromFGOutput(_ReturnAddress()))
    {
        ScopedFGCall fgCall {};
        auto result = fg->GetSwapchainImages(device, swapchain, pSwapchainImageCount, pSwapchainImages);

        if (result == VK_SUCCESS && pSwapchainImages != nullptr)
            Hudfix_Vk::AddSwapchainImages(pSwapchainImages, *pSwapchainImageCount);

        return result;
    }

    return o_GetSwapchainImagesKHR(device, swapchain, pSwapchainImageCount, pSwapchainImages);
}

VALIDATE_HOOK(hkvkAcquireNextImageKHR, PFN_vkAcquireNextImageKHR)
static VkResult hkvkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                        VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex)
{
    if (auto fg = State::Instance().currentVkFG;
        fg != nullptr && fg->OwnsSwapchain(swapchain) && !FromFGOutput(_ReturnAddress()))
    {
        ScopedFGCall fgCall {};
        return fg->AcquireNextImage(device, swapchain, timeout, semaphore, fence, pImageIndex);
    }

    return o_AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

// A game that acquires with this one (RDR2): the FG output's swapchain only has the plain acquire. The device mask is for
// device groups, the FG output runs on one GPU.
VALIDATE_HOOK(hkvkAcquireNextImage2KHR, PFN_vkAcquireNextImage2KHR)
static VkResult hkvkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* pAcquireInfo,
                                         uint32_t* pImageIndex)
{
    if (auto fg = State::Instance().currentVkFG; fg != nullptr && pAcquireInfo != nullptr &&
                                                 fg->OwnsSwapchain(pAcquireInfo->swapchain) &&
                                                 !FromFGOutput(_ReturnAddress()))
    {
        ScopedFGCall fgCall {};
        return fg->AcquireNextImage(device, pAcquireInfo->swapchain, pAcquireInfo->timeout, pAcquireInfo->semaphore,
                                    pAcquireInfo->fence, pImageIndex);
    }

    return o_AcquireNextImage2KHR(device, pAcquireInfo, pImageIndex);
}

// The FG output's swapchain isn't a driver one and the game's present ids don't reach the real presents, the output
// paces the presents itself
VALIDATE_HOOK(hkvkWaitForPresentKHR, PFN_vkWaitForPresentKHR)
static VkResult hkvkWaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t presentId, uint64_t timeout)
{
    if (auto fg = State::Instance().currentVkFG;
        fg != nullptr && fg->OwnsSwapchain(swapchain) && !FromFGOutput(_ReturnAddress()))
    {
        return VK_SUCCESS;
    }

    return o_WaitForPresentKHR(device, swapchain, presentId, timeout);
}

VALIDATE_HOOK(hkvkSetHdrMetadataEXT, PFN_vkSetHdrMetadataEXT)
static void hkvkSetHdrMetadataEXT(VkDevice device, uint32_t swapchainCount, const VkSwapchainKHR* pSwapchains,
                                  const VkHdrMetadataEXT* pMetadata)
{
    auto fg = State::Instance().currentVkFG;

    for (uint32_t i = 0; i < swapchainCount; i++)
    {
        if (fg != nullptr && fg->OwnsSwapchain(pSwapchains[i]) && !FromFGOutput(_ReturnAddress()))
        {
            ScopedFGCall fgCall {};
            fg->SetHdrMetadata(device, pSwapchains[i], &pMetadata[i]);
        }
        else
            o_SetHdrMetadataEXT(device, 1, &pSwapchains[i], &pMetadata[i]);
    }
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
        DlssNr::ExposureScanVk::OnDeviceDestroyed(device);
        native::lowlatency::OnDeviceReleasedVulkan(device);

        // InputCommon's output on it stops while the device still lives (its semaphores are the device's), then the
        // NvAPI Reflex output's state and the game's own semaphore (InputReflex) go
        InputCommon::device_released(reinterpret_cast<IUnknown*>(device), true);
        ReflexNvApiVk::forget_device(device);
        InputReflex::ForgetVulkanDevice(device);

        {
            std::lock_guard lock(_timelineDevicesMutex);
            _timelineDevices.erase(device);
        }

        {
            std::lock_guard lock(_lowLatencyMutex);
            _lowLatency2Devices.erase(device);
        }

        std::unique_lock lock(LatencySwapchainMutex());
        _latencySwapchains.erase(device);
    }

    // The present path falls back to the last device made when its queue is not known (hkvkQueuePresentKHR): that must
    // never be a device that is gone (a probe device made and destroyed after the swapchain)
    if (device != VK_NULL_HANDLE && _device == device)
        _device = VK_NULL_HANDLE;

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

    if (auto direct = StreamlineSwapchainFunction(procName.c_str(), _ReturnAddress()))
        return direct;

    if (auto lowLatency = LowLatencyInputHook(procName, orgFunc, _ReturnAddress()); lowLatency != nullptr)
        return lowLatency;

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

    if (auto direct = StreamlineSwapchainFunction(procName.c_str(), _ReturnAddress()))
        return direct;

    if (auto lowLatency = LowLatencyInputHook(procName, orgFunc, _ReturnAddress()); lowLatency != nullptr)
        return lowLatency;

    auto result = VulkanSpoofing::hkvkGetDeviceProcAddr(orgFunc, pName);
    if (result != VK_NULL_HANDLE)
        return result;

    return orgFunc;
}

PFN_vkCreateWin32SurfaceKHR VulkanHooks::OriginalCreateWin32Surface()
{
    return o_vkCreateWin32SurfaceKHR != nullptr ? o_vkCreateWin32SurfaceKHR : &vkCreateWin32SurfaceKHR;
}

PFN_vkDestroySurfaceKHR VulkanHooks::OriginalDestroySurface()
{
    return o_vkDestroySurfaceKHR != nullptr ? o_vkDestroySurfaceKHR : &vkDestroySurfaceKHR;
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

    if (o_GetSwapchainImagesKHR != nullptr)
        DetourDetach(&(PVOID&) o_GetSwapchainImagesKHR, hkvkGetSwapchainImagesKHR);

    if (o_AcquireNextImageKHR != nullptr)
        DetourDetach(&(PVOID&) o_AcquireNextImageKHR, hkvkAcquireNextImageKHR);

    if (o_AcquireNextImage2KHR != nullptr)
        DetourDetach(&(PVOID&) o_AcquireNextImage2KHR, hkvkAcquireNextImage2KHR);

    if (o_SetHdrMetadataEXT != nullptr)
        DetourDetach(&(PVOID&) o_SetHdrMetadataEXT, hkvkSetHdrMetadataEXT);

    if (o_WaitForPresentKHR != nullptr)
        DetourDetach(&(PVOID&) o_WaitForPresentKHR, hkvkWaitForPresentKHR);

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
        o_GetSwapchainImagesKHR = nullptr;
        o_AcquireNextImageKHR = nullptr;
        o_AcquireNextImage2KHR = nullptr;
        o_SetHdrMetadataEXT = nullptr;
        o_WaitForPresentKHR = nullptr;
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
