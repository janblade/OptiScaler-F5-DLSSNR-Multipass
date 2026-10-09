#pragma once

#include "SysUtils.h"

#include "IFeature_Vk.h"

#include <inputs/NVNGX_DLSS.h>

class FeatureProvider_Vk
{
  public:
    static bool GetFeature(Upscaler upscaler, UINT handleId, NVSDK_NGX_Parameter* parameters,
                           std::unique_ptr<IFeature_Vk>* feature);

    static bool ChangeFeature(Upscaler upscaler, VkInstance instance, VkPhysicalDevice pd, VkDevice device,
                              VkCommandBuffer cmdBuffer, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa,
                              UINT handleId, NVSDK_NGX_Parameter* parameters, ContextData<IFeature_Vk>* contextData);

    // "FG only (game's upscaler)" ([DlssNr] NativeFrameGenerationOnly) feeds frame generation from a Vulkan-on-D3D12
    // feature only (upscalers/IFeature_VkwDx12.h).
    static bool IsOn12(Upscaler upscaler);
    // The Vulkan-on-D3D12 backend standing in for `upscaler`: DLSS -> DLSS_on12 where DLSS can run, FSR 2.1 -> its
    // on12, anything else (FSR 2.2, FFX, XeSS: no XeSS on12 for Vulkan yet) -> FFX_on12. An on12 one is kept.
    static Upscaler On12For(Upscaler upscaler);
    // The backend to build: `upscaler`, or On12For(upscaler) while FG only is on.
    static Upscaler ForFrameGenerationOnly(Upscaler upscaler);
};
