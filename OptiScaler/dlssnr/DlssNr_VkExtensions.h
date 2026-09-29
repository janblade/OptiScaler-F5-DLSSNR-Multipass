#pragma once

// What the Neural Rendering model needs from a Vulkan device, and whether it can be arranged.
//
// The model ships a complete native Vulkan surface -- fourteen entry points, more than either D3D
// interface has -- so there is no reason for the pass to go through a D3D12 bridge on Vulkan. What
// stops it is not the model, it is the device: NGX loads its kernels through two NVIDIA vendor
// extensions that no game enables, and a Vulkan device's extension list is fixed at creation. Ask
// afterwards and the answer is no, permanently.
//
// OptiScaler already hooks vkCreateInstance and vkCreateDevice and already hands the real call a
// mutable copy of the create info, so appending to that list is what the hook is shaped for. This
// header is the list and the appending, kept in the module so it leaves with it.
//
// The names below come from the model binary itself: scanning nvngx_dlssnr.dll 310.8 for VK_*_* yields exactly
// these. They are the floor. NGX's own answer for the feature (NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements,
// feature 18) is asked as well and merged in, so a model version that needs more is not missed.

#include <vulkan/vulkan.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace DlssNr::VkExt
{

// Instance level. get_physical_device_properties2 is core from Vulkan 1.1 and every game enables it
// anyway; it is listed because the model names it and a 1.0 instance would still need it.
inline const char* const kInstance[] = {
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
};

// Device level. The two NVX entries are the ones that matter and the reason this file exists --
// binary_import is how NGX hands the driver its cubins, and image_view_handle is how it addresses
// the textures it was given. Neither appears in a game's own list, ever.
inline const char* const kDevice[] = {
    "VK_NVX_binary_import",
    "VK_NVX_image_view_handle",
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
};

// Holds the merged list for as long as the create call needs it. VkDeviceCreateInfo keeps a bare
// pointer, so the storage has to outlive the call rather than the statement.
struct Merged
{
    std::vector<const char*> names;
    std::vector<std::string> owned;
};

// Everything the physical device is willing to offer, by name.
inline std::vector<std::string> SupportedDeviceExtensions(PFN_vkGetInstanceProcAddr getInstanceProcAddr,
                                                          VkInstance instance, VkPhysicalDevice physicalDevice)
{
    std::vector<std::string> out;

    if (getInstanceProcAddr == nullptr || physicalDevice == VK_NULL_HANDLE)
        return out;

    auto enumerate = (PFN_vkEnumerateDeviceExtensionProperties) getInstanceProcAddr(
        instance, "vkEnumerateDeviceExtensionProperties");

    if (enumerate == nullptr)
        return out;

    uint32_t count = 0;

    if (enumerate(physicalDevice, nullptr, &count, nullptr) != VK_SUCCESS || count == 0)
        return out;

    std::vector<VkExtensionProperties> props(count);

    if (enumerate(physicalDevice, nullptr, &count, props.data()) != VK_SUCCESS)
        return out;

    out.reserve(count);

    for (const auto& p : props)
        out.emplace_back(p.extensionName);

    return out;
}

inline bool Contains(const std::vector<std::string>& haystack, const char* needle)
{
    for (const auto& h : haystack)
    {
        if (h == needle)
            return true;
    }

    return false;
}

// What NGX says Neural Rendering (feature 18) needs on this device, or nothing when it cannot say (no instance, the
// core is not loaded, DLSS is off in OptiScaler, or it does not know the feature). `result` is NGX's answer, 0 if
// never asked (NGX itself never answers 0). Defined in NVNGX_DLSS_Vk.cpp, with the rest of the NGX proxy.
std::vector<std::string> NgxDeviceExtensions(VkInstance instance, VkPhysicalDevice physicalDevice, int& result);

// Whether the game's NGX core is up on Vulkan, so a parameter block it allocated may still be handed back to it.
bool NgxCoreUp();

// Whether `p` may be written. A create info's chain is the game's memory and may sit in a read-only section.
inline bool IsWritable(const void* p)
{
    MEMORY_BASIC_INFORMATION info {};

    if (VirtualQuery(p, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) != 0)
        return false;

    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (info.Protect & writable) != 0;
}

// Devices created with shaderStorageImageWriteWithoutFormat switched on (the create hook, Vulkan_Hooks.cpp), which
// detail reuse's shader needs. Forgotten when the device is destroyed, so a new device with the same handle value is not
// mistaken for it.
inline std::mutex g_writeWithoutFormatMutex;
inline std::vector<VkDevice> g_writeWithoutFormatDevices;

inline void NoteDevice(VkDevice device, bool writesWithoutFormat)
{
    std::lock_guard<std::mutex> lock(g_writeWithoutFormatMutex);
    auto& list = g_writeWithoutFormatDevices;
    list.erase(std::remove(list.begin(), list.end(), device), list.end());
    if (writesWithoutFormat)
        list.push_back(device);
}

inline void ForgetDevice(VkDevice device) { NoteDevice(device, false); }

inline bool WritesWithoutFormat(VkDevice device)
{
    std::lock_guard<std::mutex> lock(g_writeWithoutFormatMutex);
    const auto& list = g_writeWithoutFormatDevices;
    return std::find(list.begin(), list.end(), device) != list.end();
}

inline bool ListHas(const char* const* list, uint32_t count, const char* needle)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        if (list[i] != nullptr && std::string(list[i]) == needle)
            return true;
    }

    return false;
}

} // namespace DlssNr::VkExt
