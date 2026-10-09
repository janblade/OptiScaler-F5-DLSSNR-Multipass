#pragma once
#include <d3d12.h>
#include <atomic>
#include <mutex>
#include <nvapi/NvApiTypes.h>

#include "Hook_Utils.h"
#include "low_latency/ll_util.h"

enum TimingType : uint32_t
{
    TimeRange, // in ns, value stored in length
    Simulation,
    RenderSubmit,
    Present,
    Driver,
    OsRenderQueue,
    GpuRender,

    TimingTypeCOUNT
};

class ReflexHooks
{
    inline static bool _inited = false;
    inline static uint32_t _minimumIntervalUs = 0;
    inline static NV_SET_SLEEP_MODE_PARAMS _lastSleepParams {};
    inline static IUnknown* _lastSleepDev = nullptr;
    inline static uint8_t _FgNumFramesToGenerate = 0;
    inline static uint64_t _lastAsyncMarkerFrameId = 0;
    inline static uint64_t _updatesWithoutMarker = 0;

    inline static uint64_t _lastMarkerFrame = 0;

    inline static NV_VULKAN_SET_SLEEP_MODE_PARAMS _lastVkSleepParams {};
    inline static HANDLE _lastVkSleepDev = nullptr;

    inline static std::thread::id _lastSetSleepThread {};

    // The game's own Reflex calls versus OptiScaler's (ownSetSleepMode and friends, Optical F5Low's low latency): ours
    // go through the same wrappers, so they take the same routing, but they are not "the game sends markers".
    inline static thread_local bool _ownCall = false;
    inline static std::atomic<bool> _gameCalledReflex = false;
    inline static std::atomic<bool> _gameCalledSetSleepMode = false;
    // SetSleepMode and the stored device: the game's call and ours never interleave, and a released device is dropped
    inline static std::recursive_mutex _sleepModeMutex;

    // D3D
    inline static decltype(&NvAPI_D3D_SetSleepMode) o_NvAPI_D3D_SetSleepMode = nullptr;
    inline static decltype(&NvAPI_D3D_GetSleepStatus) o_NvAPI_D3D_GetSleepStatus = nullptr;
    inline static decltype(&NvAPI_D3D_Sleep) o_NvAPI_D3D_Sleep = nullptr;
    inline static decltype(&NvAPI_D3D_GetLatency) o_NvAPI_D3D_GetLatency = nullptr;
    inline static decltype(&NvAPI_D3D_SetLatencyMarker) o_NvAPI_D3D_SetLatencyMarker = nullptr;
    inline static decltype(&NvAPI_D3D12_SetAsyncFrameMarker) o_NvAPI_D3D12_SetAsyncFrameMarker = nullptr;

    static NvAPI_Status hkNvAPI_D3D_SetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams);
    static NvAPI_Status hkNvAPI_D3D_Sleep(IUnknown* pDev);
    static NvAPI_Status hkNvAPI_D3D_GetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pGetLatencyParams);
    static NvAPI_Status hkNvAPI_D3D_SetLatencyMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams);
    static NvAPI_Status hkNvAPI_D3D12_SetAsyncFrameMarker(ID3D12CommandQueue* pCommandQueue,
                                                          NV_ASYNC_FRAME_MARKER_PARAMS* pSetAsyncFrameMarkerParams);

    // Vulkan
    inline static decltype(&NvAPI_Vulkan_SetLatencyMarker) o_NvAPI_Vulkan_SetLatencyMarker = nullptr;
    inline static decltype(&NvAPI_Vulkan_SetSleepMode) o_NvAPI_Vulkan_SetSleepMode = nullptr;
    inline static decltype(&NvAPI_Vulkan_Sleep) o_NvAPI_Vulkan_Sleep = nullptr;
    inline static decltype(&NvAPI_Vulkan_GetLatency) o_NvAPI_Vulkan_GetLatency = nullptr;
    // Not part of the hooked table (only our own low latency calls it); resolved with the rest, and optional
    inline static decltype(&NvAPI_Vulkan_InitLowLatencyDevice) o_NvAPI_Vulkan_InitLowLatencyDevice = nullptr;

    static NvAPI_Status hkNvAPI_Vulkan_SetLatencyMarker(HANDLE vkDevice,
                                                        NV_VULKAN_LATENCY_MARKER_PARAMS* pSetLatencyMarkerParams);
    static NvAPI_Status hkNvAPI_Vulkan_SetSleepMode(HANDLE vkDevice,
                                                    NV_VULKAN_SET_SLEEP_MODE_PARAMS* pSetSleepModeParams);
    static NvAPI_Status hkNvAPI_Vulkan_Sleep(HANDLE vkDevice, NvU64 signalValue);
    static NvAPI_Status hkNvAPI_Vulkan_GetLatency(HANDLE vkDevice, NV_VULKAN_LATENCY_RESULT_PARAMS* pGetLatencyParams);

    VALIDATE_MEMBER_HOOK(hkNvAPI_D3D_SetSleepMode, decltype(&NvAPI_D3D_SetSleepMode))
    VALIDATE_MEMBER_HOOK(hkNvAPI_D3D_Sleep, decltype(&NvAPI_D3D_Sleep))
    VALIDATE_MEMBER_HOOK(hkNvAPI_D3D_GetLatency, decltype(&NvAPI_D3D_GetLatency))
    VALIDATE_MEMBER_HOOK(hkNvAPI_D3D_SetLatencyMarker, decltype(&NvAPI_D3D_SetLatencyMarker))
    VALIDATE_MEMBER_HOOK(hkNvAPI_D3D12_SetAsyncFrameMarker, decltype(&NvAPI_D3D12_SetAsyncFrameMarker))
    VALIDATE_MEMBER_HOOK(hkNvAPI_Vulkan_SetLatencyMarker, decltype(&NvAPI_Vulkan_SetLatencyMarker))
    VALIDATE_MEMBER_HOOK(hkNvAPI_Vulkan_SetSleepMode, decltype(&NvAPI_Vulkan_SetSleepMode))
    VALIDATE_MEMBER_HOOK(hkNvAPI_Vulkan_Sleep, decltype(&NvAPI_Vulkan_Sleep))
    VALIDATE_MEMBER_HOOK(hkNvAPI_Vulkan_GetLatency, decltype(&NvAPI_Vulkan_GetLatency))

  public:
    static std::optional<TimingEntry> timingData[TimingType::TimingTypeCOUNT];

    static void hookReflex(PFN_NvApi_QueryInterface& queryInterface);
    static uint8_t dlssgFrameCountToGenerate();
    static void setDlssgFrameCount(uint8_t count);
    static bool isReflexHooked();
    static void* getHookedReflex(unsigned int InterfaceId);
    static bool updateTimingData();
    static bool gameIsSendingMarkers();

    // Calls OptiScaler makes itself (native/NativeLowLatency.cpp). Call ensureTable first. They run the hooked
    // wrappers, so the fps cap, XeFG's routing and fakenvapi's modes apply as for the game's calls, without counting as
    // the game's.
    static bool ensureTable(PFN_NvApi_QueryInterface queryInterface);
    static NvAPI_Status ownSetSleepMode(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pParams);
    static NvAPI_Status ownSleep(IUnknown* pDev);
    static NvAPI_Status ownSetLatencyMarker(IUnknown* pDev, NV_LATENCY_MARKER_PARAMS* pParams);
    static NvAPI_Status ownGetLatency(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pParams);
    // Same four, for a native Vulkan game (native/NativeDriverVk.cpp): vkDevice is the game's VkDevice, reinterpreted as
    // the HANDLE the Vulkan Reflex entry points take.
    static NvAPI_Status ownSetSleepModeVulkan(HANDLE vkDevice, NV_VULKAN_SET_SLEEP_MODE_PARAMS* pParams);
    static NvAPI_Status ownSleepVulkan(HANDLE vkDevice, NvU64 signalValue);
    static NvAPI_Status ownSetLatencyMarkerVulkan(HANDLE vkDevice, NV_VULKAN_LATENCY_MARKER_PARAMS* pParams);
    static NvAPI_Status ownGetLatencyVulkan(HANDLE vkDevice, NV_VULKAN_LATENCY_RESULT_PARAMS* pParams);
    // NvAPI_Vulkan_InitLowLatencyDevice: initialises the device as a low latency device and returns the timeline
    // semaphore its Sleep signals (the app has to wait for it). NVAPI_NO_IMPLEMENTATION when the interface is missing.
    static NvAPI_Status ownInitLowLatencyDeviceVulkan(HANDLE vkDevice, HANDLE* signalSemaphore);
    // The game calls Reflex through VK_NV_low_latency2 (vkSetLatencySleepModeNV, vkLatencySleepNV, vkSetLatencyMarkerNV):
    // the game's own Reflex, as far as our low latency is concerned (it stands aside). Not for our own calls.
    static void noteGameVulkanLowLatency2(bool setSleepMode);
    // The device is gone, or our Reflex mode is off: the fps cap no longer goes through it (setFPSLimit, update)
    static void forgetSleepDevice(IUnknown* pDev);
    static void forgetSleepDeviceVulkan(HANDLE vkDevice);

    // A Reflex call made by the game: not one of ours, and not from the Streamline OptiScaler loaded for its own DLSS
    // frame generation (`returnAddress`: the hooked function's caller)
    static bool isGameCall(void* returnAddress);

    // The game itself has called Reflex (SetSleepMode, Sleep, a marker or an async marker), ever; and SetSleepMode
    // in particular (a game that only sends markers has Reflex off).
    static bool gameCalledReflex() { return _gameCalledReflex.load(std::memory_order_relaxed); }
    static bool gameCalledSetSleepMode() { return _gameCalledSetSleepMode.load(std::memory_order_relaxed); }

    // For updating information about Reflex hooks
    static void update(bool optiFg_FgState, bool isVulkan);

    // 0 - disables the fps cap
    static void setFPSLimit(float fps);
};
