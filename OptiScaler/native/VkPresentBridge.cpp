#include "pch.h"

#include "VkPresentBridge.h"
#include "VkPresentBridgeCore.h"

#include <native/NativeDriverVk.h>

#include <Config.h>
#include <State.h>
#include <hooks/DxgiFactory_Hooks.h>
#include <hooks/FG_Hooks.h>
#include <hooks/Vulkan_Hooks.h>
#include <menu/menu_overlay_dx.h>
#include <menu/menu_overlay_vk.h>
#include <misc/IdentifyGpu.h>
#include <proxies/DXGI_Proxy.h>
#include <with_dx12/with_dx12.h>

#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace VkPresentBridge
{

namespace
{

// VK_EXT_full_screen_exclusive: the game asks for exclusive full screen on the surface it passes, which is not the one the
// picture ends up on. A bridged swapchain is made with DISALLOWED, and the game's own acquire and release of the mode
// succeed without doing anything (Vulkan_Hooks.cpp), so a game that controls it (Detroit: Become Human) runs borderless
// through the D3D12 swapchain.
constexpr int32_t kFullScreenExclusiveInfo = 1000255000;

// VkSurfaceFullScreenExclusiveInfoEXT, without the Win32 platform header.
struct FullScreenExclusiveInfo
{
    int32_t sType;
    const void* pNext;
    int32_t fullScreenExclusive;
};

struct Bridge
{
    native::VkPresentBridgeCore core;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSurfaceKHR realSurface = VK_NULL_HANDLE;
    HWND window = nullptr;
    ID3D12Device* device12 = nullptr;
    ID3D12CommandQueue* queue12 = nullptr;
    std::set<VkSwapchainKHR> swapchains; // the game's, made on the hidden surface and not yet destroyed
    bool outputUp = false;
    bool realFG = false;
    bool copied = false; // CopyToOutput queued this frame's picture; PresentOutput follows
    uint64_t presents = 0;
};

std::mutex g_mutex;
std::unordered_map<VkSurfaceKHR, HWND> g_windows;
std::unique_ptr<Bridge> g_bridge;
std::vector<std::unique_ptr<Bridge>> g_retired; // output gone, hidden surface kept until their swapchains are destroyed

void LogOnce(const std::string& text)
{
    static std::string last;

    if (text != last)
    {
        last = text;
        LOG_WARN("Vulkan bridge: {}", text);
    }
}


bool IsBridgedSwapchain(VkSwapchainKHR swapchain)
{
    if (swapchain == VK_NULL_HANDLE)
        return false;

    if (g_bridge != nullptr && g_bridge->swapchains.contains(swapchain))
        return true;

    for (const auto& retired : g_retired)
    {
        if (retired->swapchains.contains(swapchain))
            return true;
    }

    return false;
}

// Whether this swapchain is one to bridge. An empty reason: not a case worth a line in the log (frame generation is not
// chosen at all). Called with g_mutex held.
bool Wanted(const VkSwapchainCreateInfoKHR& in, HWND window, std::string& why)
{
    auto& state = State::Instance();
    auto* config = Config::Instance();

    if (state.vulkanSkipHooks || IdentifyGpu::getPrimaryGpu().usesDxvk)
        return false;

    if (state.activeFgInput != FGInput::Upscaler || state.activeFgOutput == FGOutput::NoFG)
        return false;

    // OptiFG's Upscaler input is fed by Optical F5Low's virtual upscaler: without the mode there is nothing for it to read.
    if (!config->DlssNrEnabled.value_or_default() || !config->DlssNrNativeMotion.value_or_default() ||
        !config->DlssNrNativeUpscaler.value_or_default())
    {
        why = "frame generation is chosen, but the \"NR + upscaler & frame generation\" mode is not on: the game "
              "presents as it is";
        return false;
    }

    if (window == nullptr)
    {
        why = "the swapchain's surface is not one OptiScaler saw being made on a window";
        return false;
    }

    DXGI_FORMAT dxgi;

    if (!native::BridgeSwapchainFormat(in.imageFormat, &dxgi))
    {
        why = "the swapchain's format (" + std::to_string((int) in.imageFormat) + ") is not one the bridge carries";
        return false;
    }

    if (in.imageArrayLayers != 1 || in.imageExtent.width == 0 || in.imageExtent.height == 0)
    {
        why = "the swapchain has array layers or no size";
        return false;
    }

    return true;
}

bool MakeFactory(ComPtr<IDXGIFactory2>& factory)
{
    ScopedSkipSpoofingGlobal skipSpoofing {};

    const HRESULT hr = DxgiProxy::Module() == nullptr
                           ? CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))
                           : DxgiProxy::CreateDxgiFactory2_()(0, __uuidof(IDXGIFactory2), &factory);
    return SUCCEEDED(hr) && factory != nullptr;
}

bool Tearing(IDXGIFactory2* factory)
{
    ComPtr<IDXGIFactory5> factory5;
    BOOL supported = FALSE;

    return SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory5))) &&
           SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supported, sizeof(supported))) &&
           supported == TRUE;
}

// What a D3D11 game's bridge does when its swapchain goes (Dx11wDx12SC::Release), for the D3D12 swapchain of a Vulkan game.
void LetGoOfOutput(Bridge& bridge, IDXGISwapChain4* output)
{
    auto& state = State::Instance();
    HWND window = bridge.window;

    MenuOverlayDx::CleanupRenderTarget(true, window);

    if (state.currentSwapchain == output)
        state.currentSwapchain = nullptr;

    if (state.currentWrappedSwapchain == output)
        state.currentWrappedSwapchain = nullptr;

    FGHooks::ClearDx12InteropPresentSC(output);

    if (state.currentFGSwapchain == output)
        state.currentFGSwapchain = nullptr;

    state.currentRealSwapchain = nullptr;
    state.swapchainInteropApi = SwapchainInteropApi::None;

    auto fg = state.currentFG;

    if (fg != nullptr && fg->Mutex.getOwner() != 1 && fg->SwapchainContext() != nullptr)
    {
        fg->Deactivate();
        fg->ReleaseSwapchain(window);
    }

    bridge.outputUp = false;
}

// The D3D12 swapchain goes; the hidden surface with it unless a game swapchain still lives on it.
void Retire(std::unique_ptr<Bridge>& slot)
{
    if (slot == nullptr)
        return;

    slot->core.ReleaseOutput();

    if (slot->swapchains.empty())
    {
        slot->core.ReleaseSurface();
        slot.reset();
        return;
    }

    g_retired.push_back(std::move(slot));
}

bool CreateBridge(Bridge& bridge, const VkSwapchainCreateInfoKHR& in, std::string& why)
{
    if (!NativeMotionVk::AcquireBridgeDevice(bridge.physical, &bridge.device12, &bridge.queue12, why))
        return false;

    ComPtr<IDXGIFactory2> probe;

    if (!MakeFactory(probe))
    {
        why = "could not make a DXGI factory";
        return false;
    }

    native::BridgeTarget target;
    target.instance = bridge.instance;
    target.physical = bridge.physical;
    target.createSurface = VulkanHooks::OriginalCreateWin32Surface();
    target.realWindow = bridge.window;
    target.device12 = bridge.device12;
    target.queue12 = bridge.queue12;
    target.tearing = Tearing(probe.Get());

    Bridge* self = &bridge;

    const native::MakeOutputFn make = [self](DXGI_SWAP_CHAIN_DESC& desc, ComPtr<IDXGISwapChain4>& out, std::string& reason)
    {
        auto& state = State::Instance();
        ComPtr<IDXGIFactory2> factory;

        if (!MakeFactory(factory))
        {
            reason = "could not make a DXGI factory";
            return false;
        }

        // Frame generation reads these to make its swapchain and context, as it does for a D3D11 game.
        WithDx12::SetD3D12Objects(self->device12, self->queue12, D3D12_COMMAND_LIST_TYPE_DIRECT);

        // The game's vkCreateSwapchainKHR (this runs inside it) marks DXGI swapchains as the Vulkan driver's own.
        const bool creatingSC = state.vulkanCreatingSC;
        state.vulkanCreatingSC = false;

        bool realFG = false;
        const HRESULT hr =
            DxgiFactoryHooks::CreateDx12BridgeSwapChain(factory.Get(), self->queue12, &desc, &out, &realFG);

        state.vulkanCreatingSC = creatingSC;

        if (FAILED(hr) || out == nullptr)
        {
            reason = std::format("the D3D12 swapchain could not be made (0x{:X})", (unsigned) hr);
            return false;
        }

        self->realFG = realFG;
        state.currentSwapchainDesc = desc;
        state.currentD3D12Device = self->device12;
        state.currentCommandQueue = self->queue12;
        state.swapchainInteropApi = SwapchainInteropApi::VkwDx12;

        if (!realFG)
            FGHooks::SetDx12InteropPresentSC(out.Get(), self->window);

        self->outputUp = true;
        LOG_INFO("Vulkan bridge: D3D12 swapchain {:X} on window {:X} ({}x{}, format {}, {} buffers, {})",
                 (size_t) out.Get(), (size_t) self->window, desc.BufferDesc.Width, desc.BufferDesc.Height,
                 (int) desc.BufferDesc.Format, desc.BufferCount, realFG ? "frame generation's own" : "plain");
        return true;
    };

    const native::ReleaseOutputFn release = [self](IDXGISwapChain4* output) { LetGoOfOutput(*self, output); };

    return bridge.core.Create(target, in, make, release, why);
}

} // namespace

int32_t* FullScreenExclusiveMode(const void* chain)
{
    for (auto* header = static_cast<const VkBaseInStructure*>(chain); header != nullptr; header = header->pNext)
    {
        if ((int32_t) header->sType == kFullScreenExclusiveInfo)
            return &reinterpret_cast<FullScreenExclusiveInfo*>(const_cast<VkBaseInStructure*>(header))->fullScreenExclusive;
    }

    return nullptr;
}

void NoteSurface(VkSurfaceKHR surface, HWND window)
{
    std::lock_guard lock(g_mutex);
    g_windows[surface] = window;
}

void ForgetSurface(VkSurfaceKHR surface)
{
    std::lock_guard lock(g_mutex);
    g_windows.erase(surface);
}

bool OnCreateSwapchain(VkInstance instance, VkPhysicalDevice physical, VkDevice device, const VkSwapchainCreateInfoKHR& in,
                       VkSwapchainCreateInfoKHR* out)
{
    *out = in;
    std::lock_guard lock(g_mutex);

    const auto window = g_windows.find(in.surface);
    HWND hwnd = window != g_windows.end() ? window->second : nullptr;
    std::string why;

    // An old swapchain that was made on the hidden surface cannot be continued on the game's own.
    const auto dropBridgedOld = [&]
    {
        if (IsBridgedSwapchain(in.oldSwapchain))
            out->oldSwapchain = VK_NULL_HANDLE;
    };

    if (g_bridge != nullptr)
    {
        // A new surface on the same window, once the game's swapchains on the old one are gone: the bridge carries on.
        if (g_bridge->realSurface != in.surface && g_bridge->device == device && g_bridge->swapchains.empty() &&
            hwnd != nullptr && hwnd == g_bridge->window)
            g_bridge->realSurface = in.surface;

        if (g_bridge->realSurface != in.surface || g_bridge->device != device)
        {
            LogOnce("a second window or device is making a swapchain: not bridged");
            return false;
        }

        if (!Wanted(in, hwnd, why) || !g_bridge->core.Resize(in, why))
        {
            LogOnce(why.empty() ? "the bridge ends here: the swapchain is made on the game's window" : why);
            Retire(g_bridge);
            dropBridgedOld();
            return false;
        }
    }
    else
    {
        if (!Wanted(in, hwnd, why))
        {
            if (!why.empty())
                LogOnce(why);

            dropBridgedOld();
            return false;
        }

        auto bridge = std::make_unique<Bridge>();
        bridge->instance = instance;
        bridge->physical = physical;
        bridge->device = device;
        bridge->realSurface = in.surface;
        bridge->window = hwnd;

        if (!CreateBridge(*bridge, in, why))
        {
            LogOnce("not bridged: " + why);
            bridge->core.Release();
            dropBridgedOld();
            return false;
        }

        g_bridge = std::move(bridge);
    }

    if (!g_bridge->core.Patch(in, out, why))
    {
        LogOnce("not bridged: " + why);
        Retire(g_bridge);
        *out = in;
        dropBridgedOld();
        return false;
    }

    // A swapchain of this bridge continues on the hidden surface; any other old one has nothing to continue.
    out->oldSwapchain = g_bridge->swapchains.contains(in.oldSwapchain) ? in.oldSwapchain : VK_NULL_HANDLE;
    return true;
}

void OnSwapchainCreated(bool bridged, VkResult result, VkSwapchainKHR created)
{
    if (!bridged)
        return;

    std::lock_guard lock(g_mutex);

    if (g_bridge == nullptr)
        return;

    if (result == VK_SUCCESS && created != VK_NULL_HANDLE)
    {
        g_bridge->swapchains.insert(created);
        LOG_INFO("Vulkan bridge: the game's swapchain {:X} is on the hidden window ({}x{})", (size_t) created,
                 g_bridge->core.Width(), g_bridge->core.Height());
        return;
    }

    LogOnce("the swapchain could not be made on the hidden window (" + std::to_string((int) result) +
            "): the game's own window gets it");
    Retire(g_bridge);
}

void OnSwapchainDestroyed(VkDevice device, VkSwapchainKHR swapchain)
{
    (void) device;
    std::lock_guard lock(g_mutex);

    if (g_bridge != nullptr && g_bridge->swapchains.erase(swapchain) != 0)
    {
        // The game's last swapchain. The D3D12 swapchain and frame generation stay, for the swapchain the game makes next
        // (a game commonly destroys the old one first, Detroit: Become Human at its first resize): making frame
        // generation's swapchain again on the same window while its old one is being let go crashed the game.
        if (g_bridge->swapchains.empty())
            LOG_INFO("Vulkan bridge: the game destroyed its swapchain; the D3D12 swapchain is kept for its next one");

        return;
    }

    for (auto it = g_retired.begin(); it != g_retired.end(); ++it)
    {
        if ((*it)->swapchains.erase(swapchain) != 0)
        {
            if ((*it)->swapchains.empty())
            {
                (*it)->core.ReleaseSurface();
                g_retired.erase(it);
            }

            return;
        }
    }
}

void OnDeviceDestroying(VkDevice device)
{
    std::lock_guard lock(g_mutex);

    if (g_bridge != nullptr && g_bridge->device == device)
    {
        g_bridge->core.ReleaseOutput();
        g_retired.push_back(std::move(g_bridge));
    }

    for (auto& retired : g_retired)
    {
        if (retired->device == device)
            retired->core.ReleaseOutput();
    }

    auto& state = State::Instance();

    if (state.swapchainInteropApi == SwapchainInteropApi::VkwDx12)
        state.swapchainInteropApi = SwapchainInteropApi::None;

    NativeMotionVk::ReleaseBridgeDevice();
}

void OnDeviceDestroyed(VkDevice device)
{
    std::lock_guard lock(g_mutex);

    // The game's swapchains went with the device: the hidden surfaces can go now.
    for (auto it = g_retired.begin(); it != g_retired.end();)
    {
        if ((*it)->device == device)
        {
            (*it)->core.ReleaseSurface();
            it = g_retired.erase(it);
        }
        else
            ++it;
    }
}

bool Owns(VkSwapchainKHR swapchain)
{
    std::lock_guard lock(g_mutex);
    return IsBridgedSwapchain(swapchain);
}

bool Active(VkSwapchainKHR swapchain)
{
    std::lock_guard lock(g_mutex);
    return g_bridge != nullptr && g_bridge->outputUp && g_bridge->swapchains.contains(swapchain);
}

bool IsUp()
{
    std::lock_guard lock(g_mutex);
    return g_bridge != nullptr && g_bridge->outputUp;
}

bool CopyToOutput(ID3D12Resource* picture, ID3D12Fence* fence, uint64_t copied, ID3D12Fence* doneFence,
                  uint64_t doneValue, uint64_t signalValue)
{
    std::lock_guard lock(g_mutex);

    if (g_bridge == nullptr || !g_bridge->outputUp)
        return false;

    g_bridge->copied = false;

    if (picture == nullptr)
    {
        LogOnce("no picture for the D3D12 swapchain this frame");
        return false;
    }

    std::string why;

    if (!g_bridge->core.CopyFrame(picture, fence, copied, doneFence, doneValue, fence, signalValue, why))
    {
        LogOnce("the picture could not be put into the D3D12 swapchain: " + why);
        return false;
    }

    g_bridge->copied = true;
    return true;
}

void PresentOutput()
{
    ComPtr<IDXGISwapChain4> output;
    ID3D12CommandQueue* queue12 = nullptr;
    ID3D12Fence* copyFence = nullptr;
    uint64_t copyValue = 0;
    HWND window = nullptr;
    UINT sync = 0;
    UINT flags = 0;

    {
        std::lock_guard lock(g_mutex);

        if (g_bridge == nullptr || !g_bridge->outputUp)
            return;

        output = g_bridge->core.Output();
        queue12 = g_bridge->queue12;
        copyFence = g_bridge->core.CopyFence();
        copyValue = g_bridge->copied ? g_bridge->core.LastCopyValue() : 0;
        window = g_bridge->window;
        sync = g_bridge->core.SyncInterval();
        flags = g_bridge->core.PresentFlags();
        g_bridge->copied = false;
        ++g_bridge->presents;
    }

    // Nothing was put into the back buffer (the copy failed, or another thread held the frame source): a flip-model back
    // buffer is undefined after its last present, so the last frame stays on screen instead.
    if (output == nullptr || copyValue == 0)
        return;

    auto& state = State::Instance();
    auto fg = state.currentFG;

    // Frame generation presents from its own queue: the picture has to be in the back buffer before it reads it.
    auto fgQueue = fg != nullptr ? fg->GetCommandQueue() : nullptr;

    if (fgQueue != nullptr && fgQueue != queue12 && copyFence != nullptr && copyValue != 0)
        fgQueue->Wait(copyFence, copyValue);

    // A frame generation swapchain draws the menu itself; a plain one gets it here, as a D3D11 game's bridge does.
    const bool fgHookedPresenter = state.currentFGSwapchain == output.Get() && !FGHooks::IsDx12InteropPresentSC(output.Get());

    if (!fgHookedPresenter)
        MenuOverlayDx::Present(output.Get(), sync, flags, nullptr, queue12, window, false);

    const HRESULT hr = output->Present(sync, flags);

    if (FAILED(hr) && hr != DXGI_ERROR_WAS_STILL_DRAWING)
    {
        static int logged = 0;

        if (logged++ < 5)
            LOG_ERROR("Vulkan bridge: the D3D12 swapchain's Present failed: 0x{:X}", (unsigned) hr);

        // The D3D12 device is gone (a driver reset): the window cannot be presented to any more. Said once, with the reason.
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        {
            static bool said = false;

            if (!said && state.currentD3D12Device != nullptr)
            {
                said = true;
                Util::GetDeviceRemovedReason(state.currentD3D12Device);
            }
        }
    }
}

bool WindowResized()
{
    std::lock_guard lock(g_mutex);
    return g_bridge != nullptr && g_bridge->outputUp && g_bridge->core.RealWindowResized();
}

} // namespace VkPresentBridge
