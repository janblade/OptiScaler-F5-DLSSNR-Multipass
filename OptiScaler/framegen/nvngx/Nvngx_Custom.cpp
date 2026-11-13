#include "pch.h"
#include "Nvngx_Custom.h"

#include <NVNGX_Parameter.h>

void Nvngx_Custom::LoadLibraries()
{
    if (_name.empty())
    {
        LOG_WARN("Custom Nvngx FG provider selected without a name");
        return;
    }

    _exports = CustomProviders::Load(_name, CustomProviders::Kind::Dlssg);
    if (!_exports)
        return;

    dll = _exports.dll;

    auto get = [this](const char* exportName) { return _exports(exportName); };

    _DLSSG_D3D12_Init = (PFN_D3D12_Init) get("NVSDK_NGX_D3D12_Init");
    _DLSSG_D3D12_Init_Ext = (PFN_D3D12_Init_Ext) get("NVSDK_NGX_D3D12_Init_Ext");
    _DLSSG_D3D12_Shutdown = (PFN_D3D12_Shutdown) get("NVSDK_NGX_D3D12_Shutdown");
    _DLSSG_D3D12_Shutdown1 = (PFN_D3D12_Shutdown1) get("NVSDK_NGX_D3D12_Shutdown1");
    _DLSSG_D3D12_GetScratchBufferSize = (PFN_D3D12_GetScratchBufferSize) get("NVSDK_NGX_D3D12_GetScratchBufferSize");
    _DLSSG_D3D12_CreateFeature = (PFN_D3D12_CreateFeature) get("NVSDK_NGX_D3D12_CreateFeature");
    _DLSSG_D3D12_ReleaseFeature = (PFN_D3D12_ReleaseFeature) get("NVSDK_NGX_D3D12_ReleaseFeature");
    _DLSSG_D3D12_GetFeatureRequirements =
        (PFN_D3D12_GetFeatureRequirements) get("NVSDK_NGX_D3D12_GetFeatureRequirements");
    _DLSSG_D3D12_EvaluateFeature = (PFN_D3D12_EvaluateFeature) get("NVSDK_NGX_D3D12_EvaluateFeature");
    _DLSSG_D3D12_PopulateParameters_Impl =
        (PFN_D3D12_PopulateParameters_Impl) get("NVSDK_NGX_D3D12_PopulateParameters_Impl");

    _DLSSG_VULKAN_Init = (PFN_VULKAN_Init) get("NVSDK_NGX_VULKAN_Init");
    _DLSSG_VULKAN_Init_Ext = (PFN_VULKAN_Init_Ext) get("NVSDK_NGX_VULKAN_Init_Ext");
    _DLSSG_VULKAN_Init_Ext2 = (PFN_VULKAN_Init_Ext2) get("NVSDK_NGX_VULKAN_Init_Ext2");
    _DLSSG_VULKAN_Shutdown = (PFN_VULKAN_Shutdown) get("NVSDK_NGX_VULKAN_Shutdown");
    _DLSSG_VULKAN_Shutdown1 = (PFN_VULKAN_Shutdown1) get("NVSDK_NGX_VULKAN_Shutdown1");
    _DLSSG_VULKAN_GetScratchBufferSize = (PFN_VULKAN_GetScratchBufferSize) get("NVSDK_NGX_VULKAN_GetScratchBufferSize");
    _DLSSG_VULKAN_CreateFeature = (PFN_VULKAN_CreateFeature) get("NVSDK_NGX_VULKAN_CreateFeature");
    _DLSSG_VULKAN_CreateFeature1 = (PFN_VULKAN_CreateFeature1) get("NVSDK_NGX_VULKAN_CreateFeature1");
    _DLSSG_VULKAN_ReleaseFeature = (PFN_VULKAN_ReleaseFeature) get("NVSDK_NGX_VULKAN_ReleaseFeature");
    _DLSSG_VULKAN_GetFeatureRequirements =
        (PFN_VULKAN_GetFeatureRequirements) get("NVSDK_NGX_VULKAN_GetFeatureRequirements");
    _DLSSG_VULKAN_EvaluateFeature = (PFN_VULKAN_EvaluateFeature) get("NVSDK_NGX_VULKAN_EvaluateFeature");
    _DLSSG_VULKAN_PopulateParameters_Impl =
        (PFN_VULKAN_PopulateParameters_Impl) get("NVSDK_NGX_VULKAN_PopulateParameters_Impl");

    if (!_exports.path.empty())
    {
        version_t fileVersion {};
        if (Util::GetFileVersion(_exports.path.wstring(), &fileVersion))
            _version = fileVersion;
    }

    QueryReportedParameters();

    // A provider that reports FrameGeneration.Available 0 cannot run here: fall back instead of creating features that
    // fail (upstream 91ab9fe3). The dll stays loaded; without its init exports isDx12Available/isVulkanAvailable are
    // false.
    if (_reportedAvailable == 0)
    {
        LOG_WARN("Custom Nvngx FG provider {} reports no frame generation (max fake frames {}), not using it", _name,
                 _reportedMaxFakeFrames);
        _DLSSG_D3D12_Init = nullptr;
        _DLSSG_D3D12_Init_Ext = nullptr;
        _DLSSG_VULKAN_Init = nullptr;
        _DLSSG_VULKAN_Init_Ext = nullptr;
        _DLSSG_VULKAN_Init_Ext2 = nullptr;
        return;
    }

    LOG_INFO("Custom Nvngx FG provider {} ready (D3D12 {}, Vulkan {}, max fake frames {})", _name,
             _DLSSG_D3D12_Init != nullptr, _DLSSG_VULKAN_Init != nullptr, getMaxFakeFramesCount());
}

// What the provider reports before init, through its PopulateParameters (D3D12's, else Vulkan's: the parameters are not
// tied to an API)
void Nvngx_Custom::QueryReportedParameters()
{
    auto populate = _DLSSG_D3D12_PopulateParameters_Impl != nullptr
                        ? _DLSSG_D3D12_PopulateParameters_Impl
                        : (PFN_D3D12_PopulateParameters_Impl) _DLSSG_VULKAN_PopulateParameters_Impl;
    if (populate == nullptr)
        return;

    NVNGX_Parameters params(API::DX12, false);
    if (populate(&params) != NVSDK_NGX_Result_Success)
        return;

    int countMax = 0;
    if (params.Get("DLSSG.MultiFrameCountMax", &countMax) == NVSDK_NGX_Result_Success && countMax > 0)
        _reportedMaxFakeFrames = countMax;

    int available = 1;
    if (params.Get("FrameGeneration.Available", &available) == NVSDK_NGX_Result_Success)
        _reportedAvailable = available;
}
