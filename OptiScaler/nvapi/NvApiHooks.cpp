#include "pch.h"
#include "NvApiHooks.h"
#include <dlssnr/DlssNrNative.h>
#include <NvApiDriverSettings.h>

#include "State.h"
#include <Config.h>
#include <Util.h>

#include <proxies/KernelBase_Proxy.h>

#include <detours/detours.h>
#include <misc/IdentifyGpu.h>
#include <framegen/nvngx/DlssgPresetSupport.h>
#include <low_latency/input/input_reflex.h>

// #define LOG_ALL_DRS_GET_CALLS

#ifdef LOG_ALL_DRS_GET_CALLS
#include <magic_enum.hpp>
#endif

NvAPI_Status __stdcall NvApiHooks::hkNvAPI_GPU_GetArchInfo(NvPhysicalGpuHandle hPhysicalGpu,
                                                           NV_GPU_ARCH_INFO* pGpuArchInfo)
{
    if (!o_NvAPI_GPU_GetArchInfo)
    {
        LOG_DEBUG("nullptr");
        return NVAPI_ERROR;
    }

    const auto status = o_NvAPI_GPU_GetArchInfo(hPhysicalGpu, pGpuArchInfo);

    if (status == NVAPI_OK && pGpuArchInfo)
    {
        if (pGpuArchInfo->architecture_id <= NV_GPU_ARCHITECTURE_GP100)
        {
            // Check if values were volatile, override them if so
            // if (!Config::Instance()->StreamlineSpoofing.value_for_config().has_value())
            //    Config::Instance()->StreamlineSpoofing.set_volatile_value(true);

            if (!Config::Instance()->DisableFlipMetering.value_for_config().has_value())
                Config::Instance()->DisableFlipMetering.set_volatile_value(true);
        }

        LOG_DEBUG("Original arch: {0:X} impl: {1:X} rev: {2:X}!", pGpuArchInfo->architecture,
                  pGpuArchInfo->implementation, pGpuArchInfo->revision);

        // for DLSS on 16xx cards
        // Can't spoof ada for DLSSG here as that breaks DLSS/DLSSD
        if (pGpuArchInfo->architecture == NV_GPU_ARCHITECTURE_TU100 &&
            pGpuArchInfo->implementation > NV_GPU_ARCH_IMPLEMENTATION_TU106)
        {
            pGpuArchInfo->implementation = NV_GPU_ARCH_IMPLEMENTATION_TU106;
            LOG_INFO("Spoofed arch: {0:X} impl: {1:X} rev: {2:X}!", pGpuArchInfo->architecture,
                     pGpuArchInfo->implementation, pGpuArchInfo->revision);
        }
    }

    return status;
}

NvAPI_Status __stdcall NvApiHooks::hkNvAPI_DRS_GetSetting(NvDRSSessionHandle hSession, NvDRSProfileHandle hProfile,
                                                          NvU32 settingId, NVDRS_SETTING* pSetting)
{
    if (!o_NvAPI_DRS_GetSetting)
        return NVAPI_ERROR;

    auto result = o_NvAPI_DRS_GetSetting(hSession, hProfile, settingId, pSetting);

    // DLSS-G frame generation preset. nvngx_dlssg (310.9.16+) and sl.dlss_g read this once at start, app profile
    // first; A/B/C select UI recomposition / FG Lite (candidate blending). Answered in this process only, also when
    // the profile has no value, so the driver profile itself is never written.
    constexpr NvU32 dlssgPresetId = 0x10E41DF1;
    // Only the struct layout this was written for; the caller's version is checked before anything is read or written
    if (pSetting && settingId == dlssgPresetId && pSetting->version == NVDRS_SETTING_VER &&
        (result == NVAPI_OK || result == NVAPI_SETTING_NOT_FOUND))
    {
        if (result == NVAPI_OK && pSetting->settingType == NVDRS_DWORD_TYPE)
            State::Instance().dlssgPresetExternal = pSetting->u32CurrentValue;

        State::Instance().dlssgPresetRead = true;

        // Which DLSS-G asks (nvngx_dlssg reads it itself; Streamline's sl.dlss_g asks through sl.common)
        HMODULE caller = nullptr;
        wchar_t callerPath[MAX_PATH] {};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR) _ReturnAddress(), &caller) &&
            GetModuleFileNameW(caller, callerPath, MAX_PATH) != 0 &&
            _wcsnicmp(std::filesystem::path(callerPath).filename().c_str(), L"nvngx_dlssg", 11) == 0)
        {
            version_t version {};
            if (Util::GetFileVersion(callerPath, &version))
            {
                auto& v = State::Instance().dlssgPresetReaderVersion;
                v[0] = (uint16_t) version.major;
                v[1] = (uint16_t) version.minor;
                v[2] = (uint16_t) version.patch;
                v[3] = (uint16_t) version.reserved;
            }
        }

        const auto preset = Config::Instance()->FGDLSSGPreset.value_or_default();

        // A custom DLSS-G replacement makes the frames: only a preset it can take is answered, else it keeps what the
        // driver profile says (logged once)
        auto& state = State::Instance();
        const bool customMakesFrames =
            state.activeFgNvngx == FGNvngxReplacement::Custom &&
            (state.activeFgOutput == FGOutput::DLSSG || state.activeFgInput == FGInput::NvngxFG);
        bool customTakes = true;

        if (customMakesFrames && preset >= 1 && preset <= 3)
        {
            customTakes = DlssgPresetSupport::Takes(DlssgPresetSupport::ForCustom(state.activeFgNvngxCustom), preset);

            static bool logged = false;
            if (!customTakes && !logged)
            {
                logged = true;
                LOG_WARN("DLSS-G preset: {} can't take preset {}, left to the driver profile",
                         state.activeFgNvngxCustom, (char) ('A' + preset - 1));
            }
        }

        if (preset >= 1 && preset <= 3 && customTakes)
        {
            pSetting->settingId = dlssgPresetId;
            pSetting->settingType = NVDRS_DWORD_TYPE;
            pSetting->settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
            pSetting->isCurrentPredefined = 0;
            pSetting->isPredefinedValid = 0;
            pSetting->u32PredefinedValue = (NvU32) preset;
            pSetting->u32CurrentValue = (NvU32) preset;
            State::Instance().dlssgPresetAnswered = (uint32_t) preset;
            LOG_INFO("DLSS-G preset: answered {} (driver profile {}: 0x{:X})", (char) ('A' + preset - 1),
                     result == NVAPI_OK ? "value" : "none", State::Instance().dlssgPresetExternal.load());
            return NVAPI_OK;
        }
    }

    if (pSetting && result == NVAPI_OK)
    {
        constexpr NvU32 streamlineOverrideId = 0x10E41E06;
        if (settingId == streamlineOverrideId && State::Instance().gameName == "KCD2" &&
            State::Instance().activeFgOutput == FGOutput::DLSSG && !State::Instance().externalFrameGeneration)
        {
            // Keep the tested local Streamline stack. This changes the query
            // result for this process only, not the saved NVIDIA driver profile.
            pSetting->u32CurrentValue = 0;
            LOG_INFO("KCD2: use installed Streamline instead of OTA override");
        }
#ifdef LOG_ALL_DRS_GET_CALLS
        LOG_TRACE("settingId: {:X}, settingLocation: {}, isCurrentPredefined: {}", settingId,
                  magic_enum::enum_name(pSetting->settingLocation), pSetting->isCurrentPredefined,
                  pSetting->isPredefinedValid);

        switch (pSetting->settingType)
        {
        case NVDRS_DWORD_TYPE:
            LOG_TRACE("    u32CurrentValue: {}, u32PredefinedValue: {}", pSetting->u32CurrentValue,
                      pSetting->u32PredefinedValue);
            break;
        case NVDRS_BINARY_TYPE:
            LOG_TRACE("    binary data");
            break;
        case NVDRS_STRING_TYPE:
            LOG_TRACE("    NVDRS_STRING_TYPE");
            break;
        case NVDRS_WSTRING_TYPE:
        {
            std::wstring wstrCurrentValue(reinterpret_cast<const wchar_t*>(pSetting->wszCurrentValue));
            std::wstring wstrPredefinedValue(reinterpret_cast<const wchar_t*>(pSetting->wszPredefinedValue));

            LOG_TRACE(L"    wszCurrentValue: {}, wszPredefinedValue: {}", wstrCurrentValue, wstrPredefinedValue);
            break;
        }
        }
#endif

        // TODO: maybe check those values and inform if they are being overridden externally

        // const auto dmfgFpsTarget = Config::Instance()->FGDLSSGFramerateTargetDMFG.value_or_default();
        // if (settingId == NGX_DLSSG_MODE_ID && dmfgFpsTarget != 0)
        //{
        //     pSetting->settingId = settingId;
        //     // constexpr auto name = L"NGX_DLSSG_MODE_ID";
        //     // memcpy_s(pSetting->settingName, sizeof(pSetting->settingName), name, sizeof(*name) * wcslen(name));
        //     pSetting->settingType = NVDRS_DWORD_TYPE;
        //     pSetting->isCurrentPredefined = 0;
        //     pSetting->u32CurrentValue = NGX_DLSSG_MODE_DEFAULT;

        //    LOG_DEBUG("Set NGX_DLSSG_MODE_ID to {}", pSetting->u32CurrentValue);
        //}

        // if (settingId == NGX_DLSSG_DYNAMIC_TARGET_FRAME_RATE_ID && dmfgFpsTarget != 0)
        //{
        //     pSetting->settingId = settingId;
        //     // constexpr auto name = L"NGX_DLSSG_DYNAMIC_TARGET_FRAME_RATE_ID";
        //     // memcpy_s(pSetting->settingName, sizeof(pSetting->settingName), name, sizeof(*name) * wcslen(name));
        //     pSetting->settingType = NVDRS_DWORD_TYPE;
        //     pSetting->isCurrentPredefined = 0;
        //     pSetting->u32CurrentValue = dmfgFpsTarget;

        //    LOG_DEBUG("Set NGX_DLSSG_DYNAMIC_TARGET_FRAME_RATE_ID to {}", pSetting->u32CurrentValue);
        //}

        // if (settingId == NGX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_MAX_ID && dmfgFpsTarget != 0)
        //{
        //     pSetting->settingId = settingId;
        //     // constexpr auto name = L"NGX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_MAX_ID";
        //     // memcpy_s(pSetting->settingName, sizeof(pSetting->settingName), name, sizeof(*name) * wcslen(name));
        //     pSetting->settingType = NVDRS_DWORD_TYPE;
        //     pSetting->isCurrentPredefined = 0;
        //     pSetting->u32CurrentValue = NGX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_MAX_DEFAULT;

        //    LOG_DEBUG("Set NGX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_MAX_ID to {}", pSetting->u32CurrentValue);
        //}

        // Making sure DLSSG is not set to force off
        if (settingId == NGX_DLSSG_MODE_ID)
        {
            if (State::Instance().activeFgOutput == FGOutput::DLSSG)
            {
                pSetting->u32CurrentValue = NGX_DLSSG_MODE_DISABLED;
            }
        }

        if (settingId == NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION_ID)
        {
            State::Instance().dlssRenderPresetExternal = pSetting->u32CurrentValue;

            State::Instance().dlssPresetsOverriddenExternally =
                pSetting->u32CurrentValue != NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION_OFF;

            // Report no override, we will handle presets from now on
            pSetting->u32CurrentValue = NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION_OFF;

            LOG_DEBUG("DLSS External override: {}", State::Instance().dlssRenderPresetExternal);
        }

        if (settingId == NGX_DLSS_RR_OVERRIDE_RENDER_PRESET_SELECTION_ID)
        {
            State::Instance().dlssdRenderPresetExternal = pSetting->u32CurrentValue;

            State::Instance().dlssdPresetsOverriddenExternally =
                pSetting->u32CurrentValue != NGX_DLSS_RR_OVERRIDE_RENDER_PRESET_SELECTION_OFF;

            // Report no override, we will handle presets from now on
            pSetting->u32CurrentValue = NGX_DLSS_RR_OVERRIDE_RENDER_PRESET_SELECTION_OFF;

            LOG_DEBUG("DLSSD External override: {}", State::Instance().dlssdRenderPresetExternal);
        }

        if (settingId == NGX_DLSS_RR_OVERRIDE_SCALING_RATIO_ID || settingId == NGX_DLSS_SR_OVERRIDE_SCALING_RATIO_ID)
        {
            if (Config::Instance()->UpscaleRatioOverrideEnabled.value_or_default())
            {
                auto ratio = Config::Instance()->UpscaleRatioOverrideValue.value_or_default();
                auto ratioPercentage = (uint32_t) std::round(100.f / ratio);

                // Uses the clamp from SR for RR but it should be fine
                ratioPercentage = std::clamp(ratioPercentage, (uint32_t) NGX_DLSS_SR_OVERRIDE_SCALING_RATIO_MIN,
                                             (uint32_t) NGX_DLSS_SR_OVERRIDE_SCALING_RATIO_MAX);

                pSetting->u32CurrentValue = ratioPercentage;
            }
        }
    }

    return result;
}

void* __stdcall NvApiHooks::hkNvAPI_QueryInterface(unsigned int InterfaceId)
{
    // Native Reflex, flip metering, architecture/capability queries and driver
    // presets belong to the external FG owner in this mode. Returning null for
    // a Reflex query would disable it, so forward to the real function table.
    if (State::Instance().externalFrameGeneration)
        return DlssNrNative::WrapNvapi(InterfaceId,o_NvAPI_QueryInterface ? o_NvAPI_QueryInterface(InterfaceId) : nullptr);

    if (!o_NvAPI_QueryInterface)
        if (Config::Instance()->UseFakenvapi.value_or_default())
            o_NvAPI_QueryInterface = (PFN_NvApi_QueryInterface) fakenvapi::queryInterface;
        else
            return nullptr;

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();

    // Disable flip metering
    if (InterfaceId == GET_ID(NvAPI_D3D12_SetFlipConfig) &&
        Config::Instance()->DisableFlipMetering.value_or(primaryGpu.vendorId != VendorId::Nvidia))
    {
        LOG_INFO("FlipMetering is disabled!");
        return nullptr;
    }

    // The game's Vulkan Reflex is the Reflex input on InputCommon (InputReflex), whatever the nvapi: the game gets a
    // semaphore of ours, and the output is the driver's VK_NV_low_latency2 or NvAPI Vulkan Reflex, Anti-Lag Vk or
    // LatencyFlex (low_latency/input/OutputRule.h)
    if (InterfaceId == GET_ID(NvAPI_Vulkan_InitLowLatencyDevice) ||
        InterfaceId == GET_ID(NvAPI_Vulkan_DestroyLowLatencyDevice) ||
        InterfaceId == GET_ID(NvAPI_Vulkan_GetSleepStatus) || InterfaceId == GET_ID(NvAPI_Vulkan_SetSleepMode) ||
        InterfaceId == GET_ID(NvAPI_Vulkan_Sleep) || InterfaceId == GET_ID(NvAPI_Vulkan_GetLatency) ||
        InterfaceId == GET_ID(NvAPI_Vulkan_SetLatencyMarker) ||
        InterfaceId == GET_ID(NvAPI_Vulkan_NotifyOutOfBandVkQueue))
    {
        ReflexHooks::hookReflex(o_NvAPI_QueryInterface);
        return InputReflex::ForInterface(InterfaceId);
    }

    if (InterfaceId == GET_ID(NvAPI_D3D_SetSleepMode) || InterfaceId == GET_ID(NvAPI_D3D_Sleep) ||
        InterfaceId == GET_ID(NvAPI_D3D_GetLatency) || InterfaceId == GET_ID(NvAPI_D3D_SetLatencyMarker) ||
        InterfaceId == GET_ID(NvAPI_D3D12_SetAsyncFrameMarker))
    {
        // LOG_DEBUG("counter: {}, hookReflex()", qiCounter);
        ReflexHooks::hookReflex(o_NvAPI_QueryInterface);
        auto hooked = ReflexHooks::getHookedReflex(InterfaceId);

        // The game's calls enter as the Reflex low latency input, which hands Streamline's to the same hooks
        if (hooked != nullptr)
        {
            if (auto input = InputReflex::ForInterface(InterfaceId))
                return input;
        }

        return hooked;
    }

    ReflexHooks::hookReflex(o_NvAPI_QueryInterface);

    const auto functionPointer = o_NvAPI_QueryInterface(InterfaceId);

    if (functionPointer)
    {
        if (InterfaceId == GET_ID(NvAPI_GPU_GetArchInfo))
        {
            o_NvAPI_GPU_GetArchInfo = reinterpret_cast<decltype(&NvAPI_GPU_GetArchInfo)>(functionPointer);
            return &hkNvAPI_GPU_GetArchInfo;
        }
        if (InterfaceId == GET_ID(NvAPI_DRS_GetSetting))
        {
            o_NvAPI_DRS_GetSetting = reinterpret_cast<decltype(&NvAPI_DRS_GetSetting)>(functionPointer);
            return &hkNvAPI_DRS_GetSetting;
        }
    }

    // LOG_DEBUG("counter: {} functionPointer: {:X}", qiCounter, (size_t)functionPointer);

    return DlssNrNative::WrapNvapi(InterfaceId,functionPointer);
}

// Requires HMODULE to make sure nvapi is loaded before calling this function
void NvApiHooks::Hook(HMODULE nvapiModule)
{
    if (o_NvAPI_QueryInterface != nullptr)
        return;

    if (nvapiModule == nullptr)
    {
        LOG_ERROR("Hook called with a nullptr nvapi module");
        return;
    }

    LOG_DEBUG("Trying to hook NvApi");

    o_NvAPI_QueryInterface =
        (PFN_NvApi_QueryInterface) KernelBaseProxy::GetProcAddress_()(nvapiModule, "nvapi_QueryInterface");

    LOG_DEBUG("OriginalNvAPI_QueryInterface = {0:X}", (unsigned long long) o_NvAPI_QueryInterface);

    if (o_NvAPI_QueryInterface != nullptr)
    {
        LOG_INFO("NvAPI_QueryInterface found, hooking!");

        constexpr bool leanMode = true;
        if (fakenvapi::isUsingAsMainNvapi())
            fakenvapi::init(!leanMode);
        else if (UsesXeFGApi(State::Instance().activeFgOutput))
            fakenvapi::init(leanMode);

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&) o_NvAPI_QueryInterface, hkNvAPI_QueryInterface);
        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook NvAPI_QueryInterface: {:X}", detourResult);
            o_NvAPI_QueryInterface = nullptr;
        }
    }
}

void NvApiHooks::Unhook()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_NvAPI_QueryInterface != nullptr)
        DetourDetach(&(PVOID&) o_NvAPI_QueryInterface, hkNvAPI_QueryInterface);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to unhook NvAPI_QueryInterface: {:X}", detourResult);
    }
    else
    {
        o_NvAPI_QueryInterface = nullptr;
    }
}
