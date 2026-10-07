#include "pch.h"

#include "NativeLowLatency.h"
#include "DepthFinderCore.h"

#include <Config.h>
#include <State.h>

#include <dlssnr/DlssNr_NativeMode.h>
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
std::atomic<IUnknown*> g_device = nullptr;
std::atomic<Decision> g_decision = Decision::NoF5Low;

// Only the game's present thread touches these.
Table g_table = Table::Unknown;
uint64_t g_frameId = 0;
bool g_presentStarted = false; // PRESENT_START of the frame in flight went out, PRESENT_END is owed
bool g_sleepModeSent = false;  // we turned Reflex on
IUnknown* g_sleepModeDevice = nullptr;

// GetStatus
double g_latencyAskedAt = 0.0;
bool g_hasLatency = false;
float g_latencyMs = 0.0f;

void Marker(IUnknown* device, NV_LATENCY_MARKER_TYPE type)
{
    NV_LATENCY_MARKER_PARAMS params {};
    params.version = NV_LATENCY_MARKER_PARAMS_VER;
    params.frameID = g_frameId;
    params.markerType = type;
    ReflexHooks::ownSetLatencyMarker(device, &params);
}

void SetSleepMode(IUnknown* device, bool on)
{
    NV_SET_SLEEP_MODE_PARAMS params {};
    params.version = NV_SET_SLEEP_MODE_PARAMS_VER;
    params.bLowLatencyMode = on;
    params.bLowLatencyBoost = false;
    // 0: ReflexHooks puts OptiScaler's fps cap in here (setFPSLimit) whenever it has one
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
            LOG_WARN("F5Low low latency: no Reflex or fakenvapi interface found, it stays off");

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

    if (g_sleepModeSent && g_sleepModeDevice != nullptr && !ReflexHooks::gameCalledSetSleepMode())
        SetSleepMode(g_sleepModeDevice, false);

    g_sleepModeSent = false;
    g_sleepModeDevice = nullptr;
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

void OnPresentBegin(IUnknown* device)
{
    if (!g_running.load(std::memory_order_relaxed) || device == nullptr || ReflexHooks::gameCalledReflex())
        return;

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

void OnPresentEnd(IUnknown* device)
{
    if (device == nullptr || State::Instance().isShuttingDown)
        return;

    const auto decision = Evaluate();
    g_decision = decision;

    if (decision != Decision::Run)
    {
        if (g_running.load(std::memory_order_relaxed))
        {
            LOG_INFO("F5Low low latency: stopped ({})", DecisionText(decision));
            Stop(device);
        }

        return;
    }

    if (!g_running.load(std::memory_order_relaxed) || g_sleepModeDevice != device)
    {
        // Once, and again when the device is a new one
        SetSleepMode(device, true);
        g_sleepModeSent = true;
        g_sleepModeDevice = device;

        if (!g_running.load(std::memory_order_relaxed))
            LOG_INFO("F5Low low latency: started ({})", PathText());
    }

    if (g_presentStarted)
        Marker(device, PRESENT_END);

    g_presentStarted = false;
    g_device = device;

    ReflexHooks::ownSleep(device);

    ++g_frameId;
    g_submitSent = false;
    g_running = true;
    Marker(device, SIMULATION_START);
}

void OnFirstSubmit()
{
    if (!g_running.load(std::memory_order_relaxed))
        return;

    if (g_submitSent.exchange(true))
        return;

    auto device = g_device.load();

    if (device == nullptr || ReflexHooks::gameCalledReflex())
        return;

    Marker(device, SIMULATION_END);
    Marker(device, RENDERSUBMIT_START);
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

        static NV_LATENCY_RESULT_PARAMS results {};
        results.version = NV_LATENCY_RESULT_PARAMS_VER;
        g_hasLatency = false;

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

    status.hasLatency = g_hasLatency;
    status.latencyMs = g_latencyMs;
    return status;
}

} // namespace native::lowlatency
