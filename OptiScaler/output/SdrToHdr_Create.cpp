#include "pch.h"
#include "SdrToHdr_Create.h"

#include <Config.h>
#include <Logger.h>
#include <State.h>

#include <misc/IdentifyGpu.h>
#include <proxies/Dxgi_Proxy.h>
#include <native/VkPresentBridge.h>
#include <framegen/dlssg/DlssgTelemetry.h>

#include <mutex>

namespace SdrToHdr
{
namespace
{
thread_local int t_proxiedCreateDepth = 0;
std::mutex g_statusLock;
StatusSnapshot g_status;

// The display the window is on: its output, found through the monitor
bool DisplayForWindow(IDXGIFactory* factory, HWND window, DisplayInfo& info)
{
    if (window == nullptr)
        return false;

    // D3D11CreateDeviceAndSwapChain has no factory yet (the runtime makes one inside it): look on a plain one
    Microsoft::WRL::ComPtr<IDXGIFactory1> ownFactory;

    if (factory == nullptr)
    {
        // The unhooked export (DxgiProxy), not the one this DLL exports and hooks: that would come back through our own factory
        // hooks from inside a swap chain creation
        const auto create = DxgiProxy::CreateDxgiFactory1_();
        IDXGIFactory1* made = nullptr;

        if (create == nullptr || FAILED(create(__uuidof(IDXGIFactory1), &made)) || made == nullptr)
            return false;

        ownFactory.Attach(made);
        factory = ownFactory.Get();
    }

    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);

    if (monitor == nullptr)
        return false;

    for (UINT a = 0;; ++a)
    {
        IDXGIAdapter* adapter = nullptr;

        if (factory->EnumAdapters(a, &adapter) != S_OK || adapter == nullptr)
            return false;

        for (UINT o = 0;; ++o)
        {
            IDXGIOutput* output = nullptr;

            if (adapter->EnumOutputs(o, &output) != S_OK || output == nullptr)
                break;

            DXGI_OUTPUT_DESC desc {};
            const bool match = SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor;

            if (match)
                info = QueryDisplay(output);

            output->Release();

            if (match)
            {
                adapter->Release();
                return true;
            }
        }

        adapter->Release();
    }
}
} // namespace

StatusSnapshot Status()
{
    std::lock_guard lock(g_statusLock);
    return g_status;
}

void RecordStatus(Reason reason, bool active, bool hdr10, bool enabled, float displayMaxLuminance)
{
    std::lock_guard lock(g_statusLock);
    g_status.decided = true;
    g_status.enabledAtDecision = enabled;
    g_status.reason = reason;
    g_status.active = active;
    g_status.hdr10 = hdr10;
    g_status.displayMaxLuminance = displayMaxLuminance;
    g_status.endsAtResize = false;
}

void RecordUpgraded(bool upgraded)
{
    std::lock_guard lock(g_statusLock);
    g_status.upgraded = upgraded;
}

void RecordNote(Reason note)
{
    std::lock_guard lock(g_statusLock);
    g_status.endsAtResize = true;
    g_status.endReason = note;
}

void CheckLateStandDown(ProxyChainBase& proxy, bool upgraded)
{
    // (cheap: two atomics and one state read, every present of a proxied chain)
    const auto& dlssg = DlssgTelemetry::Get();
    const bool on = State::Instance().activeFgOutput == FGOutput::DLSSG ||
                    (dlssg.optionsSeen.load(std::memory_order_acquire) && dlssg.active.load(std::memory_order_relaxed));

    if (!on || proxy.NoteLateStandDownLogged())
        return;

    std::lock_guard lock(g_statusLock);

    // On a chain made flip model for HDR output the next resize does not end it either
    const Reason note = upgraded ? Reason::DlssgKeptUpgraded : Reason::DlssgStartedLater;
    LOG_INFO("{}", ReasonText(note));
    g_status.endsAtResize = true;
    g_status.endReason = note;
}

void FillEnvironment(Inputs& inputs)
{
    inputs.frameGen = inputs.frameGen || State::Instance().currentFG != nullptr;
    inputs.dxvk = inputs.dxvk || VkPresentBridge::DxvkThroughVulkan() || VkPresentBridge::DxvkGame() ||
                  IdentifyGpu::getPrimaryGpu().usesDxvk;
    // OptiScaler's own DLSS-G output, or the game's own Streamline DLSS-G switched on: its last slDLSSGSetOptions, seen by
    // the Streamline hooks (DlssgTelemetry, filled only for the game's own), left it on. Not "sl.dlss_g.dll is loaded":
    // games ship and load it with DLSS-G off (NBA 2K27). Known only once the game sends options, so a game that turns
    // DLSS-G on after its chain is made is caught at its next resize.
    const auto& dlssg = DlssgTelemetry::Get();
    inputs.streamlineDlssg = inputs.streamlineDlssg || State::Instance().activeFgOutput == FGOutput::DLSSG ||
                             (dlssg.optionsSeen.load(std::memory_order_acquire) &&
                              dlssg.active.load(std::memory_order_relaxed));
}

Plan PrepareCreate(const CreateRequest& request, DXGI_FORMAT& format)
{
    Plan plan;
    plan.gameFormat = format;

    // Off: nothing is looked at, logged or changed
    if (!Config::Instance()->HdrOutputEnabled.value_or_default())
        return plan;

    // Inside the creation of a chain an outer hook already decided to make HDR: pass through
    if (t_proxiedCreateDepth > 0)
        return plan;

    Inputs in;
    in.enabled = true;
    in.gameFormat = format;
    in.flipModel = request.flipModel;
    in.windowed = request.windowed;
    in.canUpgradeBitBlt = true; // a chain being created
    in.frameGen = request.frameGen;
    in.composition = request.composition;
    in.coreWindow = request.coreWindow;
    in.multiNode = request.multiNode;
    in.multisampled = request.sampleCount > 1;
    FillEnvironment(in);

    plan.displayKnown = DisplayForWindow(request.factory, request.hwnd, plan.display);
    in.displayKnown = plan.displayKnown;
    in.displayHdr = plan.display.hdrOn;

    const Decision decision = Decide(in);
    plan.reason = decision.reason;
    LOG_INFO("{}", ReasonText(decision.reason));
    plan.hdr10 = Config::Instance()->UseHDR10.value_or_default();
    RecordStatus(decision.reason, decision.use, plan.hdr10, true, plan.display.maxLuminance);

    if (!decision.use)
        return plan;

    plan.proxied = true;
    plan.realFormat = RealFormat(plan.hdr10);
    format = plan.realFormat;

    MakeUpgradePlan(request, decision.upgrade, plan);
    RecordUpgraded(plan.upgraded);

    if (plan.editDesc)
        LOG_INFO("HDR output: the game asked for an older swap chain type; made it flip model for HDR");
    else if (plan.upgraded)
        LOG_INFO("HDR output: the game asked for an older swap chain type; the V-Sync override already made it flip model");

    LOG_INFO("HDR output: the swap chain is made as {} ({}), the game keeps back buffers of its own format ({})",
             plan.hdr10 ? "R10G10B10A2 / PQ" : "R16G16B16A16_FLOAT / scRGB", plan.hdr10 ? "HDR10" : "scRGB",
             (UINT) plan.gameFormat);
    return plan;
}

void Unproxy(Plan& plan, DXGI_FORMAT& format, HRESULT failure)
{
    plan.upgraded = false; // (the site restored the desc first, RestoreSwapDesc)
    plan.editDesc = false;
    LOG_WARN("HDR output: the swap chain could not be made as HDR ({:X}), making it as the game asked", (UINT) failure);
    plan.proxied = false;
    plan.reason = Reason::StartFailed;
    RecordStatus(plan.reason, false, plan.hdr10, true, plan.display.maxLuminance);
    RecordUpgraded(false);
    format = plan.gameFormat;
}

void NoteFrameGenChain()
{
    if (!Config::Instance()->HdrOutputEnabled.value_or_default())
        return;

    // The inner chain a frame generation library makes through the factory already logged and recorded it
    const auto before = Status();

    if (!(before.decided && before.enabledAtDecision && before.reason == Reason::FrameGen))
        LOG_INFO("{}", ReasonText(Reason::FrameGen));

    RecordStatus(Reason::FrameGen, false, Config::Instance()->UseHDR10.value_or_default(), true, 0.0f);
}

void NoteStandDown(Reason reason)
{
    if (Config::Instance()->HdrOutputEnabled.value_or_default())
        LOG_INFO("{}", ReasonText(reason));
}
} // namespace SdrToHdr

namespace SdrToHdr
{
ScopedProxiedCreate::ScopedProxiedCreate(bool on) : _on(on)
{
    if (_on)
        ++t_proxiedCreateDepth;
}

ScopedProxiedCreate::~ScopedProxiedCreate()
{
    if (_on)
        --t_proxiedCreateDepth;
}
} // namespace SdrToHdr
