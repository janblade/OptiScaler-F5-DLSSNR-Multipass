#pragma once

#include <windows.h>
#include <d3d12.h>
#include <mutex>
#include <set>

namespace DlssNr
{
// Vendor NGX caches COM devices beyond ReleaseFeature. ReShade's Vulkan layer
// can unload at vkDestroyInstance before those caches are destroyed. Pin only
// an identified ReShade proxy implementation so its COM vtables remain mapped
// until process termination. Call on a LIVE device, before NGX retains it.
inline bool RetainDeviceProxyModule(ID3D12Device* device)
{
    if (!device) return false;
    const auto implementation = reinterpret_cast<LPCWSTR>((*reinterpret_cast<void***>(device))[0]);
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, implementation, &module)) return false;
    bool pinned = false;
    if (GetProcAddress(module, "ReShadeRegisterAddon") != nullptr)
    {
        static std::mutex mutex;
        static std::set<HMODULE> retained;
        std::lock_guard lock(mutex);
        HMODULE pinnedModule = nullptr;
        if (!retained.contains(module) &&
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               implementation, &pinnedModule))
        {
            retained.insert(module);
            pinned = true;
        }
    }
    FreeLibrary(module); // balance the temporary reference; a successful pin remains
    return pinned;
}
} // namespace DlssNr
