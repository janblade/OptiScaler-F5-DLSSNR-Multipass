#include "pch.h"
#include "SdrToHdr_Create.h"

#include <Config.h>
#include <Logger.h>
#include <State.h>

#include <misc/IdentifyGpu.h>
#include <native/VkPresentBridge.h>

namespace SdrToHdr
{
namespace
{
bool IsD3D12Queue(IUnknown* device)
{
    if (device == nullptr)
        return false;

    ID3D12CommandQueue* queue = nullptr;

    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))) || queue == nullptr)
        return false;

    queue->Release();
    return true;
}

// The display the window is on: its output, found through the monitor
bool DisplayForWindow(IDXGIFactory* factory, HWND window, DisplayInfo& info)
{
    if (factory == nullptr || window == nullptr)
        return false;

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

void FillEnvironment(Inputs& inputs)
{
    inputs.frameGen = inputs.frameGen || State::Instance().currentFG != nullptr;
    inputs.dxvk = inputs.dxvk || VkPresentBridge::DxvkThroughVulkan() || VkPresentBridge::DxvkGame() ||
                  IdentifyGpu::getPrimaryGpu().usesDxvk;
    inputs.streamlineDlssg = inputs.streamlineDlssg || State::Instance().activeFgOutput == FGOutput::DLSSG;
}

Plan PrepareCreate(const CreateRequest& request, DXGI_FORMAT& format)
{
    Plan plan;
    plan.gameFormat = format;

    // Off: nothing is looked at, logged or changed
    if (!Config::Instance()->HdrOutputEnabled.value_or_default())
        return plan;

    Inputs in;
    in.enabled = true;
    in.gameFormat = format;
    in.flipModel = request.flipModel;
    in.frameGen = request.frameGen;
    in.composition = request.composition;
    in.multiNode = request.multiNode;
    in.d3d11 = !IsD3D12Queue(request.device);
    FillEnvironment(in);

    plan.displayKnown = DisplayForWindow(request.factory, request.hwnd, plan.display);
    in.displayKnown = plan.displayKnown;
    in.displayHdr = plan.display.hdrOn;

    const Decision decision = Decide(in);
    plan.reason = decision.reason;
    LOG_INFO("{}", ReasonText(decision.reason));

    if (!decision.use)
        return plan;

    plan.proxied = true;
    plan.hdr10 = Config::Instance()->UseHDR10.value_or_default();
    plan.realFormat = RealFormat(plan.hdr10);
    format = plan.realFormat;

    LOG_INFO("HDR output: the swap chain is made as {} ({}), the game keeps back buffers of its own format ({})",
             plan.hdr10 ? "R10G10B10A2 / PQ" : "R16G16B16A16_FLOAT / scRGB", plan.hdr10 ? "HDR10" : "scRGB",
             (UINT) plan.gameFormat);
    return plan;
}

void Unproxy(Plan& plan, DXGI_FORMAT& format, HRESULT failure)
{
    LOG_WARN("HDR output: the swap chain could not be made as HDR ({:X}), making it as the game asked", (UINT) failure);
    plan.proxied = false;
    plan.reason = Reason::UnsupportedFormat;
    format = plan.gameFormat;
}

void NoteStandDown(Reason reason)
{
    if (Config::Instance()->HdrOutputEnabled.value_or_default())
        LOG_INFO("{}", ReasonText(reason));
}
} // namespace SdrToHdr
