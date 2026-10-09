#include "pch.h"

#include "NativeLowLatency.h"
#include "DepthFinderCore.h"

#include <Config.h>
#include <State.h>

#include <dlssnr/DlssNr_NativeMode.h>
#include <framegen/IFGFeature_Dx12.h>
#include <hooks/LibraryLoad_Hooks.h>
#include <hooks/Reflex_Hooks.h>
#include <nvapi/NvApiHooks.h>
#include <nvapi/fakenvapi.h>

#include <atomic>

namespace native::lowlatency
{

namespace
{
enum class Table
{
    Unknown,
    Found,
    Missing
};

std::atomic<bool> g_running = false;    // markers are being sent
std::atomic<bool> g_submitSent = false; // this frame's SIMULATION_END / RENDERSUBMIT_START went out
// `device` is the game's D3D11/D3D12 device, or -- g_isVulkan -- its VkDevice reinterpreted as the same pointer-sized
// token: never dereferenced as a COM interface here, only ever passed back to whichever API's own Reflex calls stored
// it (ReflexHooks::own*/own*Vulkan). A game is one API for its whole life, so this never flips once set.
std::atomic<bool> g_isVulkan = false;
std::atomic<IUnknown*> g_device = nullptr;
std::atomic<IUnknown*> g_sleepModeDevice = nullptr;   // the device our Reflex mode was set on
std::atomic<const void*> g_gameQueue = nullptr;       // the queue the game's frame is submitted to (D3D12)
std::atomic<double> g_frameGenerationPresentAt = 0.0; // last PresentSource::FrameGeneration call, ms
std::atomic<Decision> g_decision = Decision::NoF5Low;

// Only the game's present thread touches these.
Table g_table = Table::Unknown;
uint64_t g_frameId = 0;
bool g_presentStarted = false; // PRESENT_START of the frame in flight went out, PRESENT_END is owed
bool g_sleepModeSent = false;  // we turned Reflex on

// GetStatus
double g_latencyAskedAt = 0.0;
bool g_hasLatency = false;
float g_latencyMs = 0.0f;

// NV_LATENCY_MARKER_TYPE and NV_VULKAN_LATENCY_MARKER_TYPE share the same ordinals for every value used here, but are
// unrelated enum types in the SDK: translated explicitly rather than cast, so a future SDK change cannot silently
// mismatch them.
NV_VULKAN_LATENCY_MARKER_TYPE VulkanMarkerType(NV_LATENCY_MARKER_TYPE type)
{
    switch (type)
    {
    case SIMULATION_START:
        return VULKAN_SIMULATION_START;
    case SIMULATION_END:
        return VULKAN_SIMULATION_END;
    case RENDERSUBMIT_START:
        return VULKAN_RENDERSUBMIT_START;
    case RENDERSUBMIT_END:
        return VULKAN_RENDERSUBMIT_END;
    case PRESENT_START:
        return VULKAN_PRESENT_START;
    case PRESENT_END:
    default:
        return VULKAN_PRESENT_END;
    }
}

void Marker(IUnknown* device, NV_LATENCY_MARKER_TYPE type)
{
    if (g_isVulkan.load(std::memory_order_relaxed))
    {
        NV_VULKAN_LATENCY_MARKER_PARAMS params {};
        params.version = NV_VULKAN_LATENCY_MARKER_PARAMS_VER;
        params.frameID = g_frameId;
        params.markerType = VulkanMarkerType(type);
        ReflexHooks::ownSetLatencyMarkerVulkan(reinterpret_cast<HANDLE>(device), &params);
        return;
    }

    NV_LATENCY_MARKER_PARAMS params {};
    params.version = NV_LATENCY_MARKER_PARAMS_VER;
    params.frameID = g_frameId;
    params.markerType = type;
    ReflexHooks::ownSetLatencyMarker(device, &params);
}

void SetSleepMode(IUnknown* device, bool on)
{
    if (g_isVulkan.load(std::memory_order_relaxed))
    {
        NV_VULKAN_SET_SLEEP_MODE_PARAMS params {};
        params.version = NV_VULKAN_SET_SLEEP_MODE_PARAMS_VER;
        params.bLowLatencyMode = on;
        params.bLowLatencyBoost = false;
        params.minimumIntervalUs = 0;
        ReflexHooks::ownSetSleepModeVulkan(reinterpret_cast<HANDLE>(device), &params);
        return;
    }

    NV_SET_SLEEP_MODE_PARAMS params {};
    params.version = NV_SET_SLEEP_MODE_PARAMS_VER;
    params.bLowLatencyMode = on;
    params.bLowLatencyBoost = false;
    // 0: OptiScaler's own limiter keeps the fps cap (ReflexHooks does not hand it to our calls)
    params.minimumIntervalUs = 0;
    // The markers are approximate (no engine hooks), so the driver must not tune itself to them
    params.bUseMarkersToOptimize = false;
    params.bUseMinQueueTime = false;
    ReflexHooks::ownSetSleepMode(device, &params);
}

// The Reflex function table, from the nvapi the game would have loaded: Hook loads it like the game's LoadLibrary
// would (the real nvapi64.dll, or OptiScaler's own fakenvapi when there is no NVIDIA card or no real nvapi) and hooks
// its query function, so a game that loads nvapi later finds it already done. NvAPI_Initialize is counted, so ours is
// harmless next to the game's.
bool FindTable()
{
    if (ReflexHooks::isReflexHooked())
        return true;

    if (NvApiHooks::o_NvAPI_QueryInterface == nullptr)
        LibraryLoadHooks::LoadNvApi();

    auto queryInterface = NvApiHooks::o_NvAPI_QueryInterface;

    if (queryInterface == nullptr)
        return false;

    if (auto init = GET_INTERFACE(NvAPI_Initialize, queryInterface))
        init();

    return ReflexHooks::ensureTable(queryInterface);
}

Inputs GatherInputs()
{
    auto config = Config::Instance();
    auto& state = State::Instance();

    Inputs in;
    in.setting = config->DlssNrNativeLowLatency.value_or_default();

    const auto shown = DlssNrNativeMode::FromKeys(
        { config->DlssNrNativeDepthFinder.value_or_default(), config->DlssNrNativeMotion.value_or_default(),
          config->DlssNrNativeInput.value_or_default(), config->DlssNrNativeUpscaler.value_or_default() });
    in.f5lowRunning = shown != DlssNrNativeMode::Shown::Off && !GameUpscalerCalledRecently();

    in.gameCallsReflex = ReflexHooks::gameCalledReflex();
    in.forceReflexDisabled = config->FN_ForceReflex.value_or_default() == ForceReflex::ForceDisable;
    in.forceXell = config->ForceXeLL.value_or_default() || state.activeFgInput == FGInput::ForceXeLL;
    in.otherFrameGenerationOwner = state.externalFrameGeneration || state.activeFgOutput == FGOutput::DLSSG;
    in.apiAvailable = g_table != Table::Missing;
    return in;
}

Decision Evaluate()
{
    auto decision = Decide(GatherInputs());

    if (decision == Decision::Run && g_table == Table::Unknown)
    {
        g_table = FindTable() ? Table::Found : Table::Missing;

        if (g_table == Table::Missing)
            LOG_WARN("Optical F5Low low latency: no Reflex or fakenvapi interface found, it stays off");

        decision = Decide(GatherInputs());
    }

    return decision;
}

// Our Reflex mode is turned off again, unless the game has set its own (its call is the later word)
void Stop(IUnknown* device)
{
    g_running = false;

    if (g_presentStarted && device != nullptr && !ReflexHooks::gameCalledReflex())
        Marker(device, PRESENT_END);

    g_presentStarted = false;

    if (auto sleepModeDevice = g_sleepModeDevice.exchange(nullptr);
        g_sleepModeSent && sleepModeDevice != nullptr && !ReflexHooks::gameCalledSetSleepMode())
        SetSleepMode(sleepModeDevice, false);

    g_sleepModeSent = false;
}

// With frame generation's swap chain in place, its presents are the game's and the wrapped swap chain's are frame
// generation's own (real and generated frames, on its thread)
bool Ignored(PresentSource source)
{
    const auto now = Util::MillisecondsNow();

    if (source == PresentSource::FrameGeneration)
    {
        g_frameGenerationPresentAt = now;
        return false;
    }

    return SwapChainPresentIgnored(now, g_frameGenerationPresentAt.load(std::memory_order_relaxed));
}

const void* GameQueue()
{
    auto& state = State::Instance();

    // Frame generation's swap chain can leave State::currentCommandQueue on its own queue; it knows the game's
    if (auto* fg12 = dynamic_cast<IFGFeature_Dx12*>(state.currentFG);
        fg12 != nullptr && fg12->GameCommandQueue() != nullptr)
        return fg12->GameCommandQueue();

    return state.currentCommandQueue;
}

const char* PathText()
{
    const bool viaFakenvapi = fakenvapi::isUsingAsMainNvapi() || State::Instance().activeFgOutput == FGOutput::XeFG;

    if (!viaFakenvapi)
        return "NVIDIA Reflex";

    switch (fakenvapi::getCurrentMode())
    {
    case LowLatencyMode::AntiLag2:
        return "Anti-Lag 2 (through fakenvapi)";
    case LowLatencyMode::LatencyFlex:
        return "LatencyFlex (through fakenvapi)";
    case LowLatencyMode::XeLL:
        return "XeLL (through fakenvapi)";
    case LowLatencyMode::AntiLagVk:
        return "Anti-Lag (through fakenvapi)";
    default:
        return fakenvapi::isUsingAsMainNvapi() ? "fakenvapi (no method active yet)" : "NVIDIA Reflex";
    }
}
} // namespace

void OnPresentBegin(IUnknown* device, PresentSource source)
{
    // Ignored first: a FrameGeneration call must be noted even before we run, ahead of frame generation's own presents
    if (Ignored(source) || !g_running.load(std::memory_order_relaxed) || device == nullptr ||
        ReflexHooks::gameCalledReflex())
    {
        return;
    }

    g_device = device;

    // No submit hook fired (D3D12 sees it only where the queue hook is installed): the frame's work is all behind us
    if (!g_submitSent.exchange(true))
    {
        Marker(device, SIMULATION_END);
        Marker(device, RENDERSUBMIT_START);
    }

    Marker(device, RENDERSUBMIT_END);
    Marker(device, PRESENT_START);
    g_presentStarted = true;
}

void OnPresentEnd(IUnknown* device, PresentSource source)
{
    if (device == nullptr || State::Instance().isShuttingDown || Ignored(source))
        return;

    const auto decision = Evaluate();
    g_decision = decision;

    if (decision != Decision::Run)
    {
        if (g_running.load(std::memory_order_relaxed))
        {
            LOG_INFO("Optical F5Low low latency: stopped ({})", DecisionText(decision));
            Stop(device);
        }

        return;
    }

    if (!g_running.load(std::memory_order_relaxed) || g_sleepModeDevice.load() != device)
    {
        // Once, and again when the device is a new one
        SetSleepMode(device, true);
        g_sleepModeSent = true;
        g_sleepModeDevice = device;

        if (!g_running.load(std::memory_order_relaxed))
            LOG_INFO("Optical F5Low low latency: started ({})", PathText());
    }

    if (g_presentStarted)
        Marker(device, PRESENT_END);

    g_presentStarted = false;
    g_device = device;
    g_gameQueue = GameQueue();

    if (g_isVulkan.load(std::memory_order_relaxed))
        ReflexHooks::ownSleepVulkan(reinterpret_cast<HANDLE>(device), g_frameId);
    else
        ReflexHooks::ownSleep(device);

    ++g_frameId;
    g_submitSent = false;
    g_running = true;
    Marker(device, SIMULATION_START);
}

void OnPresentBeginVulkan(VkDevice device)
{
    g_isVulkan = true;
    OnPresentBegin(reinterpret_cast<IUnknown*>(device));
}

void OnPresentEndVulkan(VkDevice device)
{
    g_isVulkan = true;
    OnPresentEnd(reinterpret_cast<IUnknown*>(device));
}

void OnFirstSubmit(const void* queue)
{
    if (!g_running.load(std::memory_order_relaxed) || g_submitSent.load(std::memory_order_relaxed))
        return;

    // OptiScaler's own queues and frame generation's do not start the game's frame
    if (queue != nullptr)
    {
        if (auto gameQueue = g_gameQueue.load(std::memory_order_relaxed); gameQueue != nullptr && queue != gameQueue)
            return;
    }

    if (g_submitSent.exchange(true))
        return;

    auto device = g_device.load();

    if (device == nullptr || ReflexHooks::gameCalledReflex())
        return;

    Marker(device, SIMULATION_END);
    Marker(device, RENDERSUBMIT_START);
}

void OnDeviceReleased(IUnknown* device)
{
    if (device == nullptr)
        return;

    auto expected = device;
    g_device.compare_exchange_strong(expected, nullptr);

    // The next present on a new device sets our Reflex mode again (OnPresentEnd)
    expected = device;
    g_sleepModeDevice.compare_exchange_strong(expected, nullptr);

    ReflexHooks::forgetSleepDevice(device);
}

void OnDeviceReleasedVulkan(VkDevice device)
{
    if (device == nullptr)
        return;

    auto* token = reinterpret_cast<IUnknown*>(device);
    auto expected = token;
    g_device.compare_exchange_strong(expected, nullptr);

    expected = token;
    g_sleepModeDevice.compare_exchange_strong(expected, nullptr);

    ReflexHooks::forgetSleepDeviceVulkan(device);
}

Status GetStatus()
{
    Status status;
    status.decision = g_decision.load();

    if (status.decision != Decision::Run || !g_running.load(std::memory_order_relaxed))
        return status;

    status.path = PathText();

    auto device = g_device.load();

    if (device == nullptr)
        return status;

    if (const auto now = Util::MillisecondsNow(); now - g_latencyAskedAt >= 500.0)
    {
        g_latencyAskedAt = now;
        g_hasLatency = false;

        if (g_isVulkan.load(std::memory_order_relaxed))
        {
            static NV_VULKAN_LATENCY_RESULT_PARAMS results {};
            results.version = NV_VULKAN_LATENCY_RESULT_PARAMS_VER;

            if (ReflexHooks::ownGetLatencyVulkan(reinterpret_cast<HANDLE>(device), &results) == NVAPI_OK)
            {
                // The newest report is the last; times are in microseconds
                const auto& report = results.frameReport[63];

                if (report.frameID != 0 && report.simStartTime != 0 && report.gpuRenderEndTime > report.simStartTime)
                {
                    g_latencyMs = (float) (report.gpuRenderEndTime - report.simStartTime) / 1000.0f;
                    g_hasLatency = true;
                }
            }
        }
        else
        {
            static NV_LATENCY_RESULT_PARAMS results {};
            results.version = NV_LATENCY_RESULT_PARAMS_VER;

            if (ReflexHooks::ownGetLatency(device, &results) == NVAPI_OK)
            {
                // The newest report is the last; times are in microseconds
                const auto& report = results.frameReport[63];

                if (report.frameID != 0 && report.simStartTime != 0 && report.gpuRenderEndTime > report.simStartTime)
                {
                    g_latencyMs = (float) (report.gpuRenderEndTime - report.simStartTime) / 1000.0f;
                    g_hasLatency = true;
                }
            }
        }
    }

    status.hasLatency = g_hasLatency;
    status.latencyMs = g_latencyMs;
    return status;
}

} // namespace native::lowlatency
