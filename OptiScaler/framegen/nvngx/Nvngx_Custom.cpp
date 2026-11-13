#include "pch.h"
#include "Nvngx_Custom.h"

#include <proxies/Ntdll_Proxy.h>

#include <filesystem>

void Nvngx_Custom::LoadLibraries()
{
    const std::string& name = Config::Instance()->FGNvngxCustomName;

    if (name.empty())
    {
        LOG_WARN("Custom Nvngx FG provider selected without a name");
        return;
    }

    const std::filesystem::path folder =
        std::filesystem::path(Config::Instance()->MainDllPath.value()) / L"custom" / string_to_wstring(name);
    const std::filesystem::path candidates[] = { folder / L"dlssg" / L"nvngx_dlssg.dll", folder / L"nvngx_dlssg.dll" };

    for (const auto& candidate : candidates)
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(candidate, ec))
            continue;

        // LOAD_WITH_ALTERED_SEARCH_PATH: the provider's own DLLs next to it load from its folder, not the game's
        dll = NtdllProxy::LoadLibraryExW_Ldr(candidate.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);

        if (dll != nullptr)
        {
            _path = candidate.wstring();
            break;
        }

        LOG_WARN("Custom Nvngx FG provider {}: {} did not load (error {})", name, candidate.string(), GetLastError());
    }

    if (dll == nullptr)
    {
        LOG_WARN("Custom Nvngx FG provider {}: no nvngx_dlssg.dll in {}\\dlssg or {}", name, folder.string(),
                 folder.string());
        return;
    }

    _DLSSG_D3D12_Init = (PFN_D3D12_Init) GetProcAddress(dll, "NVSDK_NGX_D3D12_Init");
    _DLSSG_D3D12_Init_Ext = (PFN_D3D12_Init_Ext) GetProcAddress(dll, "NVSDK_NGX_D3D12_Init_Ext");
    _DLSSG_D3D12_Shutdown = (PFN_D3D12_Shutdown) GetProcAddress(dll, "NVSDK_NGX_D3D12_Shutdown");
    _DLSSG_D3D12_Shutdown1 = (PFN_D3D12_Shutdown1) GetProcAddress(dll, "NVSDK_NGX_D3D12_Shutdown1");
    _DLSSG_D3D12_GetScratchBufferSize =
        (PFN_D3D12_GetScratchBufferSize) GetProcAddress(dll, "NVSDK_NGX_D3D12_GetScratchBufferSize");
    _DLSSG_D3D12_CreateFeature = (PFN_D3D12_CreateFeature) GetProcAddress(dll, "NVSDK_NGX_D3D12_CreateFeature");
    _DLSSG_D3D12_ReleaseFeature = (PFN_D3D12_ReleaseFeature) GetProcAddress(dll, "NVSDK_NGX_D3D12_ReleaseFeature");
    _DLSSG_D3D12_GetFeatureRequirements =
        (PFN_D3D12_GetFeatureRequirements) GetProcAddress(dll, "NVSDK_NGX_D3D12_GetFeatureRequirements");
    _DLSSG_D3D12_EvaluateFeature = (PFN_D3D12_EvaluateFeature) GetProcAddress(dll, "NVSDK_NGX_D3D12_EvaluateFeature");
    _DLSSG_D3D12_PopulateParameters_Impl =
        (PFN_D3D12_PopulateParameters_Impl) GetProcAddress(dll, "NVSDK_NGX_D3D12_PopulateParameters_Impl");

    _DLSSG_VULKAN_Init = (PFN_VULKAN_Init) GetProcAddress(dll, "NVSDK_NGX_VULKAN_Init");
    _DLSSG_VULKAN_Init_Ext = (PFN_VULKAN_Init_Ext) GetProcAddress(dll, "NVSDK_NGX_VULKAN_Init_Ext");
    _DLSSG_VULKAN_Init_Ext2 = (PFN_VULKAN_Init_Ext2) GetProcAddress(dll, "NVSDK_NGX_VULKAN_Init_Ext2");
    _DLSSG_VULKAN_Shutdown = (PFN_VULKAN_Shutdown) GetProcAddress(dll, "NVSDK_NGX_VULKAN_Shutdown");
    _DLSSG_VULKAN_Shutdown1 = (PFN_VULKAN_Shutdown1) GetProcAddress(dll, "NVSDK_NGX_VULKAN_Shutdown1");
    _DLSSG_VULKAN_GetScratchBufferSize =
        (PFN_VULKAN_GetScratchBufferSize) GetProcAddress(dll, "NVSDK_NGX_VULKAN_GetScratchBufferSize");
    _DLSSG_VULKAN_CreateFeature = (PFN_VULKAN_CreateFeature) GetProcAddress(dll, "NVSDK_NGX_VULKAN_CreateFeature");
    _DLSSG_VULKAN_CreateFeature1 = (PFN_VULKAN_CreateFeature1) GetProcAddress(dll, "NVSDK_NGX_VULKAN_CreateFeature1");
    _DLSSG_VULKAN_ReleaseFeature = (PFN_VULKAN_ReleaseFeature) GetProcAddress(dll, "NVSDK_NGX_VULKAN_ReleaseFeature");
    _DLSSG_VULKAN_GetFeatureRequirements =
        (PFN_VULKAN_GetFeatureRequirements) GetProcAddress(dll, "NVSDK_NGX_VULKAN_GetFeatureRequirements");
    _DLSSG_VULKAN_EvaluateFeature =
        (PFN_VULKAN_EvaluateFeature) GetProcAddress(dll, "NVSDK_NGX_VULKAN_EvaluateFeature");
    _DLSSG_VULKAN_PopulateParameters_Impl =
        (PFN_VULKAN_PopulateParameters_Impl) GetProcAddress(dll, "NVSDK_NGX_VULKAN_PopulateParameters_Impl");

    version_t fileVersion {};
    if (Util::GetFileVersion(_path, &fileVersion))
        _version = fileVersion;

    LOG_INFO("Custom Nvngx FG provider {} loaded from {} (D3D12 {}, Vulkan {})", name, wstring_to_string(_path),
             _DLSSG_D3D12_Init != nullptr, _DLSSG_VULKAN_Init != nullptr);
}
