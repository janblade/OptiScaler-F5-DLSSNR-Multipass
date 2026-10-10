#include "pch.h"

#include "VkPresentBridge.h"
#include "VkPresentBridgeCore.h"
#include "VkPresentCopyRule.h"

#include <native/NativeDriverVk.h>
#include <native/NativeLowLatency.h>

#include <Config.h>
#include <State.h>
#include <dlssnr/DlssNr_VkExtensions.h>
#include <hooks/DxgiFactory_Hooks.h>
#include <hooks/FG_Hooks.h>
#include <hooks/Vulkan_Hooks.h>
#include <menu/menu_overlay_dx.h>
#include <menu/menu_overlay_vk.h>
#include <misc/IdentifyGpu.h>
#include <proxies/DXGI_Proxy.h>
#include <with_dx12/dx11_with_dx12_sync.h>
#include <with_dx12/with_dx12.h>

#include <atomic>
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
    // The D3D12 swapchain came from Windows' own DXGI beside dxvk's (MakeFactory). XeFG's inner swapchain is then not
    // wrapped (that factory is not hooked), so nothing draws the menu on it: PresentOutput does.
    bool systemFactory = false;
    bool copied = false; // CopyToOutput queued this frame's picture; PresentOutput follows
    native::VkCopyOutcome outcome = native::VkCopyOutcome::NoPicture; // what CopyToOutput did this frame
    native::VkCopyFailureRule copyRule;
    uint64_t presents = 0;
};

std::mutex g_mutex;
// Outputs that are up or being let go, without the mutex: XeFG's present thread asks (wrapped_swapchain.cpp LocalPresent)
// while LetGoOfOutput waits for that thread
std::atomic<int> g_outputsActive { 0 };
std::unordered_map<VkSurfaceKHR, HWND> g_windows;
std::unique_ptr<Bridge> g_bridge;
std::vector<std::unique_ptr<Bridge>> g_retired; // output gone, hidden surface kept until their swapchains are destroyed

// "FG only (game's upscaler)": the game's upscaler call, on a Vulkan-on-D3D12 feature, copied this frame's motion
// vectors and depth for frame generation on its own queue; frame generation's queue waits for that before it reads them.
ComPtr<ID3D12Fence> g_feedFence;
uint64_t g_feedValue = 0;

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

// The game runs on dxvk (IdentifyGpu's usesDxvk), asked once the GPU is identified: getAllGpus builds its list on every
// call, and this is asked on every present. -1: not known yet (nothing identified, e.g. too early in the process).
std::atomic<int> g_dxvkGame { -1 };

// Whether this swapchain is one to bridge. An empty reason: not a case worth a line in the log (frame generation is not
// chosen at all). Called with g_mutex held.
// Frame generation is chosen and nothing rules the bridge out before a swapchain is looked at.
bool FrameGenerationChosen()
{
    auto& state = State::Instance();

    if (state.vulkanSkipHooks)
        return false;

    // A dxvk game's Vulkan swapchain is dxvk's, for its D3D11 one. Bridged only with NativeDxvkVulkan on, where it is
    // that game's frame generation (the D3D11 bridge cannot work on dxvk, hooks/DxgiFactory_Hooks.cpp).
    if (DxvkGame() && !DxvkThroughVulkan())
        return false;

    return state.activeFgInput == FGInput::Upscaler && state.activeFgOutput != FGOutput::NoFG;
}

// OptiFG's Upscaler input is fed by Optical F5Low's virtual upscaler ("NR + upscaler & frame generation"), or by the
// game's own upscaler call through a Vulkan-on-D3D12 backend ("FG only (game's upscaler)",
// upscalers/IFeature_VkwDx12.cpp): without one of the two there is nothing for it to read.
bool ModeOn()
{
    auto* config = Config::Instance();

    // Not for a dxvk game: its upscaler calls are D3D11 ones, so nothing would feed OptiFG (the menu does not offer it;
    // this covers a NativeFrameGenerationOnly left in the ini).
    if (config->DlssNrNativeFrameGenerationOnly.value_or_default() && !DxvkGame())
        return true;

    return config->DlssNrEnabled.value_or_default() && config->DlssNrNativeMotion.value_or_default() &&
           config->DlssNrNativeUpscaler.value_or_default();
}

// The game was told once to make a new swapchain for the bridge since the mode was last switched on.
bool g_newSwapchainAsked = false;

bool Wanted(const VkSwapchainCreateInfoKHR& in, HWND window, std::string& why)
{
    if (!FrameGenerationChosen())
        return false;

    if (!ModeOn())
    {
        why = "frame generation is chosen, but neither \"NR + upscaler & frame generation\" nor \"FG only\" is on: "
              "the game presents as it is";
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

// *system: the factory is Windows' own beside dxvk's DXGI, which makes no swapchain for a real D3D12 queue
// (DXGI_ERROR_UNSUPPORTED). It is not hooked (DxgiFactoryHooks::CreateDx12BridgeSwapChain).
bool MakeFactory(ComPtr<IDXGIFactory2>& factory, bool* system = nullptr)
{
    ScopedSkipSpoofingGlobal skipSpoofing {};

    if (system != nullptr)
        *system = false;

    if (DxvkThroughVulkan())
    {
        factory.Attach(DxgiProxy::CreateSystemFactory2());

        if (factory != nullptr)
        {
            if (system != nullptr)
                *system = true;

            return true;
        }

        LOG_WARN("Vulkan bridge: no system DXGI factory beside dxvk's; trying dxvk's");
    }

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

    // The private device and queue were made current for frame generation (the make function below): they must not stay
    // so once the swapchain is gone, whatever uses the state next would reach a device that is about to be released.
    // After frame generation let go of its swapchain: its present thread reads them until then, and (with the output
    // still counted as active) LocalPresent does not take the queue back into the state in the meantime.
    if (bridge.device12 != nullptr)
    {
        native::lowlatency::OnDeviceReleased(bridge.device12);
        WithDx12::ForgetD3D12Device(bridge.device12);

        if (state.currentD3D12Device == bridge.device12)
            state.currentD3D12Device = nullptr;
    }

    if (bridge.queue12 != nullptr && state.currentCommandQueue == bridge.queue12)
        state.currentCommandQueue = nullptr;

    if (bridge.outputUp)
        --g_outputsActive;

    bridge.outputUp = false;
}

// A resize of the D3D12 swapchain is the same hazard as the D3D11 bridge's (Dx11wDx12SC::ResizeBuffers): frame
// generation is switched off, and XeFG's presents on its own thread are waited for, before ResizeBuffers releases the back
// buffers they use. Held until the resize is done; when it is let go frame generation is told the swapchain changed. The
// caller takes it BEFORE the bridge's own mutex: XeFG's thread holds the shared side while it draws the menu, which asks
// IsUp().
struct ResizeHold
{
    std::unique_lock<std::shared_mutex> lock { Dx11wDx12Sync::PresentResizeMutex(), std::defer_lock };

    ~ResizeHold()
    {
        auto& state = State::Instance();

        if (Config::Instance()->FGEnabled.value_or_default())
        {
            state.fgResetCapturedResources = true;
            state.fgOnlyUseCapturedResources = false;
            state.fgChanged = true;
        }

        state.scChanged = true;
    }
};

std::shared_ptr<ResizeHold> BeginResize()
{
    auto& state = State::Instance();

    if (auto fg = state.currentFG; fg != nullptr && fg->FrameGenerationContext() != nullptr && fg->IsActive())
    {
        state.fgChanged = true;
        fg->UpdateTarget();
        fg->Deactivate();
    }

    auto hold = std::make_shared<ResizeHold>();

    // The wrapped swapchain's present takes the shared side for XeFG only
    if (state.activeFgOutput == FGOutput::XeFG)
        hold->lock.lock();

    return hold;
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
    target.destroySurface = VulkanHooks::OriginalDestroySurface();
    target.realWindow = bridge.window;
    target.device12 = bridge.device12;
    target.queue12 = bridge.queue12;
    target.tearing = Tearing(probe.Get());

    Bridge* self = &bridge;

    const native::MakeOutputFn make = [self](DXGI_SWAP_CHAIN_DESC& desc, ComPtr<IDXGISwapChain4>& out, std::string& reason)
    {
        auto& state = State::Instance();
        ComPtr<IDXGIFactory2> factory;
        bool systemFactory = false;

        if (!MakeFactory(factory, &systemFactory))
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
            DxgiFactoryHooks::CreateDx12BridgeSwapChain(factory.Get(), self->queue12, &desc, &out, &realFG, systemFactory);

        state.vulkanCreatingSC = creatingSC;

        if (FAILED(hr) || out == nullptr)
        {
            reason = std::format("the D3D12 swapchain could not be made (0x{:X})", (unsigned) hr);
            return false;
        }

        self->realFG = realFG;
        self->systemFactory = systemFactory;
        state.currentSwapchainDesc = desc;
        state.currentD3D12Device = self->device12;
        state.currentCommandQueue = self->queue12;
        state.swapchainInteropApi = SwapchainInteropApi::VkwDx12;

        if (!realFG)
            FGHooks::SetDx12InteropPresentSC(out.Get(), self->window);

        self->outputUp = true;
        ++g_outputsActive;
        LOG_INFO("Vulkan bridge: D3D12 swapchain {:X} on window {:X} ({}x{}, format {}, {} buffers, {})",
                 (size_t) out.Get(), (size_t) self->window, desc.BufferDesc.Width, desc.BufferDesc.Height,
                 (int) desc.BufferDesc.Format, desc.BufferCount, realFG ? "frame generation's own" : "plain");
        return true;
    };

    const native::ReleaseOutputFn release = [self](IDXGISwapChain4* output) { LetGoOfOutput(*self, output); };

    return bridge.core.Create(target, in, make, release, why);
}

} // namespace

bool DxvkGame()
{
    int known = g_dxvkGame.load(std::memory_order_relaxed);

    if (known < 0)
    {
        if (IdentifyGpu::getAllGpus().empty())
            return false;

        known = IdentifyGpu::getPrimaryGpu().usesDxvk ? 1 : 0;
        g_dxvkGame.store(known, std::memory_order_relaxed);
    }

    return known == 1;
}

bool DxvkThroughVulkan()
{
    return DxvkGame() && Config::Instance()->DlssNrNativeDxvkVulkan.value_or_default();
}

int32_t* FullScreenExclusiveMode(const void* chain, bool* readOnly)
{
    if (readOnly != nullptr)
        *readOnly = false;

    for (auto* header = static_cast<const VkBaseInStructure*>(chain); header != nullptr; header = header->pNext)
    {
        if ((int32_t) header->sType != kFullScreenExclusiveInfo)
            continue;

        // The chain is the game's memory, and may sit in a read-only section: only a field that can be written is handed
        // out
        auto* mode =
            &reinterpret_cast<FullScreenExclusiveInfo*>(const_cast<VkBaseInStructure*>(header))->fullScreenExclusive;

        if (DlssNr::VkExt::IsWritable(mode))
            return mode;

        if (readOnly != nullptr)
            *readOnly = true;

        return nullptr;
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

    // The bridge resizes its D3D12 swapchain for this one: frame generation is out of it first (before g_mutex, see
    // ResizeHold)
    bool needsHold = false;
    {
        std::lock_guard lock(g_mutex);
        needsHold = g_bridge != nullptr && g_bridge->outputUp && g_bridge->device == device &&
                    g_bridge->core.ResizeNeeded(in);
    }

    std::shared_ptr<ResizeHold> resizeHold = needsHold ? BeginResize() : nullptr;

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

        // Another window, once the game's swapchains on the old one are gone: the game moved (RDR2 makes a new window).
        // The D3D12 swapchain on the old window goes, and the bridge is made again below on the new one.
        if (g_bridge->realSurface != in.surface && g_bridge->device == device && g_bridge->swapchains.empty() &&
            hwnd != nullptr && hwnd != g_bridge->window)
        {
            LOG_INFO("Vulkan bridge: the game moved to window {:X} (was {:X}): the bridge follows",
                     (size_t) hwnd, (size_t) g_bridge->window);
            resizeHold.reset();
            Retire(g_bridge);
        }
    }

    if (g_bridge != nullptr)
    {
        if (g_bridge->realSurface != in.surface || g_bridge->device != device)
        {
            LogOnce("a second window or device is making a swapchain: not bridged");
            return false;
        }

        const bool resized = Wanted(in, hwnd, why) && g_bridge->core.Resize(in, why);
        resizeHold.reset(); // the swapchain has its new size: frame generation may come back

        if (!resized)
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

bool OutputActive()
{
    return g_outputsActive.load(std::memory_order_acquire) > 0;
}

ID3D12Device* Device()
{
    std::lock_guard lock(g_mutex);
    return g_bridge != nullptr && g_bridge->outputUp ? g_bridge->device12 : nullptr;
}

bool CopyToOutput(ID3D12Resource* picture, ID3D12Fence* fence, uint64_t copied, ID3D12Fence* doneFence,
                  uint64_t doneValue, uint64_t signalValue)
{
    std::lock_guard lock(g_mutex);

    if (g_bridge == nullptr || !g_bridge->outputUp)
        return false;

    g_bridge->copied = false;
    g_bridge->outcome = native::VkCopyOutcome::NoPicture;

    if (picture == nullptr)
    {
        LogOnce("no picture for the D3D12 swapchain this frame");
        return false;
    }

    std::string why;

    if (!g_bridge->core.CopyFrame(picture, fence, copied, doneFence, doneValue, fence, signalValue, why))
    {
        g_bridge->outcome = native::VkCopyOutcome::CopyFailed;
        LogOnce("the picture could not be put into the D3D12 swapchain: " + why);
        return false;
    }

    g_bridge->outcome = native::VkCopyOutcome::Copied;
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
    native::VkCopyFailureRule::Decision copy;
    ComPtr<ID3D12Fence> feedFence;
    uint64_t feedValue = 0;
    bool menuHere = false;

    {
        std::lock_guard lock(g_mutex);

        // Taken whether or not this present goes ahead: the next one is the next frame's
        feedFence = std::move(g_feedFence);
        feedValue = g_feedValue;
        g_feedValue = 0;

        if (g_bridge == nullptr || !g_bridge->outputUp)
            return;

        output = g_bridge->core.Output();
        queue12 = g_bridge->queue12;
        copyFence = g_bridge->core.CopyFence();
        copyValue = g_bridge->copied ? g_bridge->core.LastCopyValue() : 0;
        window = g_bridge->window;
        sync = g_bridge->core.SyncInterval();
        flags = g_bridge->core.PresentFlags();
        menuHere = g_bridge->systemFactory;
        copy = g_bridge->copyRule.OnFrame(g_bridge->copied ? native::VkCopyOutcome::Copied : g_bridge->outcome);
        g_bridge->copied = false;
        g_bridge->outcome = native::VkCopyOutcome::NoPicture;
        ++g_bridge->presents;
    }

    if (output == nullptr)
        return;

    // Nothing was put into the back buffer (the copy failed, or another thread held the frame source). The swapchain
    // is presented all the same after a few frames (native/VkPresentCopyRule.h): this present is what paces the game and
    // keeps the window alive, and a game whose copy keeps failing would otherwise freeze on its last frame with one line
    // in the log. The first few are skipped so the last good frame stays on screen, not a flip-discard buffer's undefined
    // contents (which frame generation would also interpolate from). Said with a count, at 1, 10, 100 ...
    if (copy.warn)
        LOG_WARN("Vulkan bridge: no picture was put into the D3D12 swapchain ({} presents so far); {}", copy.failures,
                 copy.present ? "presenting the swapchain as it is" : "keeping the last frame on screen");
    else if (copy.recovered)
        LOG_INFO("Vulkan bridge: pictures reach the D3D12 swapchain again (after {} presents without)", copy.failures);

    if (!copy.present)
        return;

    auto& state = State::Instance();
    auto fg = state.currentFG;

    // Frame generation presents from its own queue: the picture has to be in the back buffer before it reads it.
    auto fgQueue = fg != nullptr ? fg->GetCommandQueue() : nullptr;

    if (fgQueue != nullptr && fgQueue != queue12 && copyFence != nullptr && copyValue != 0)
        fgQueue->Wait(copyFence, copyValue);

    // The game's motion vectors and depth, copied for frame generation by its upscaler call (FG only)
    if (feedFence != nullptr && feedValue != 0)
        (fgQueue != nullptr ? fgQueue : queue12)->Wait(feedFence.Get(), feedValue);

    // A frame generation swapchain draws the menu itself; a plain one gets it here, as a D3D11 game's bridge does. So does
    // frame generation's swapchain from the system DXGI (a dxvk game), whose inner swapchain is not wrapped: the menu is
    // then in the picture frame generation reads.
    const bool fgHookedPresenter = state.currentFGSwapchain == output.Get() && !FGHooks::IsDx12InteropPresentSC(output.Get());

    if (!fgHookedPresenter || menuHere)
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

bool OutputActiveOn(ID3D12Device* device)
{
    std::lock_guard lock(g_mutex);
    return device != nullptr && g_bridge != nullptr && g_bridge->outputUp && g_bridge->device12 == device;
}

void FrameGenerationInputsQueued(ID3D12Fence* fence, uint64_t value)
{
    std::lock_guard lock(g_mutex);
    g_feedFence = fence;
    g_feedValue = fence != nullptr ? value : 0;
}

bool WantsNewSwapchain(VkSwapchainKHR swapchain)
{
    std::lock_guard lock(g_mutex);

    if (!FrameGenerationChosen() || !ModeOn())
    {
        g_newSwapchainAsked = false;
        return false;
    }

    // Already on the bridge, or the game was told and made one the bridge would not take (the reason is in the log):
    // asked again only once the mode is switched off and on
    if (g_newSwapchainAsked || (g_bridge != nullptr && g_bridge->swapchains.contains(swapchain)) ||
        IsBridgedSwapchain(swapchain))
        return false;

    g_newSwapchainAsked = true;
    LOG_INFO("Vulkan bridge: \"NR + upscaler & frame generation\" was switched on after the game made its swapchain: "
             "the game is told its swapchain is out of date, so it makes a new one for the bridge");
    return true;
}

bool WindowResized()
{
    std::lock_guard lock(g_mutex);

    if (g_bridge == nullptr || !g_bridge->outputUp)
        return false;

    const bool report = g_bridge->core.ShouldReportOutOfDate();

    // The window never matches the extent the game asks for (a windowed game with a fixed or clamped extent): the game is
    // left alone after a few presents rather than recreating its swapchain on every one
    if (!report && g_bridge->core.OutOfDateGivenUp())
    {
        RECT client {};
        GetClientRect(g_bridge->core.RealWindow(), &client);
        LogOnce(std::format("the window is {}x{} and the game's swapchain stays {}x{}: it is no longer told to recreate it",
                            client.right - client.left, client.bottom - client.top, g_bridge->core.Width(),
                            g_bridge->core.Height()));
    }

    return report;
}

} // namespace VkPresentBridge
