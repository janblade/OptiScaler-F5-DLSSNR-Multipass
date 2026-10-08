// Not built with the precompiled header: Vulkan and Direct3D only, so tests/nr_vk_present_bridge_gpu.cpp compiles it alone.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "VkPresentBridgeCore.h"

#include <algorithm>
#include <format>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace native
{

namespace
{

constexpr wchar_t kHiddenClass[] = L"OptiVkPresentBridgeHost";

LRESULT CALLBACK HiddenProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool WaitFence(ID3D12Fence* fence, uint64_t value)
{
    if (fence == nullptr || fence->GetCompletedValue() >= value)
        return true;

    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (event == nullptr || FAILED(fence->SetEventOnCompletion(value, event)))
    {
        if (event != nullptr)
            CloseHandle(event);

        return false;
    }

    const bool done = WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
    CloseHandle(event);
    return done;
}

} // namespace

bool BridgeSwapchainFormat(VkFormat format, DXGI_FORMAT* dxgi)
{
    switch (format)
    {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        *dxgi = DXGI_FORMAT_B8G8R8A8_UNORM;
        return true;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        *dxgi = DXGI_FORMAT_R8G8B8A8_UNORM;
        return true;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        *dxgi = DXGI_FORMAT_R10G10B10A2_UNORM;
        return true;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        *dxgi = DXGI_FORMAT_R16G16B16A16_FLOAT;
        return true;
    default:
        return false;
    }
}

bool BridgeColorSpace(VkColorSpaceKHR space, DXGI_COLOR_SPACE_TYPE* dxgi)
{
    switch (space)
    {
    case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
        *dxgi = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        return true;
    case VK_COLOR_SPACE_HDR10_ST2084_EXT:
        *dxgi = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        return true;
    case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
        *dxgi = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        return true;
    default:
        return false;
    }
}

void VkPresentBridgeCore::SetHiddenSize(uint32_t width, uint32_t height)
{
    if (_hidden != nullptr)
        SetWindowPos(_hidden, nullptr, 0, 0, (int) width, (int) height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool VkPresentBridgeCore::Create(const BridgeTarget& target, const VkSwapchainCreateInfoKHR& game, MakeOutputFn make,
                                 ReleaseOutputFn release, std::string& why)
{
    Release();

    if (target.instance == VK_NULL_HANDLE || target.physical == VK_NULL_HANDLE || target.createSurface == nullptr ||
        target.realWindow == nullptr || target.device12 == nullptr || target.queue12 == nullptr || !make)
    {
        why = "the bridge was given no window or device";
        return false;
    }

    if (!BridgeSwapchainFormat(game.imageFormat, &_dxgi))
    {
        why = std::format("the swapchain's format ({}) is not one the bridge carries", (int) game.imageFormat);
        return false;
    }

    _target = target;
    _make = std::move(make);
    _release = std::move(release);
    _width = game.imageExtent.width;
    _height = game.imageExtent.height;
    _format = game.imageFormat;
    _space = game.imageColorSpace;

    // The hidden window sits on the real window's monitor, so the surface and the real swapchain agree on the display.
    int x = 0;
    int y = 0;
    MONITORINFO monitor {};
    monitor.cbSize = sizeof(monitor);

    if (GetMonitorInfoW(MonitorFromWindow(target.realWindow, MONITOR_DEFAULTTONEAREST), &monitor))
    {
        x = monitor.rcMonitor.left;
        y = monitor.rcMonitor.top;
    }

    WNDCLASSEXW wc {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HiddenProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kHiddenClass;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        why = "could not register the hidden window's class";
        Release();
        return false;
    }

    _hidden = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kHiddenClass, L"", WS_POPUP, x, y, (int) _width,
                              (int) _height, nullptr, nullptr, wc.hInstance, nullptr);

    if (_hidden == nullptr)
    {
        why = "could not make the hidden window";
        Release();
        return false;
    }

    ShowWindow(_hidden, SW_HIDE);

    VkWin32SurfaceCreateInfoKHR surfaceInfo {};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.hinstance = wc.hInstance;
    surfaceInfo.hwnd = _hidden;

    if (const VkResult result = target.createSurface(target.instance, &surfaceInfo, nullptr, &_surface);
        result != VK_SUCCESS)
    {
        why = std::format("could not make a Vulkan surface on the hidden window ({})", (int) result);
        _surface = VK_NULL_HANDLE;
        Release();
        return false;
    }

    // A queue family that can present to it.
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(target.physical, &families, nullptr);
    bool presentable = false;

    for (uint32_t family = 0; family < families && !presentable; ++family)
    {
        VkBool32 supported = VK_FALSE;
        presentable = vkGetPhysicalDeviceSurfaceSupportKHR(target.physical, family, _surface, &supported) ==
                          VK_SUCCESS &&
                      supported == VK_TRUE;
    }

    if (!presentable)
    {
        why = "no queue family can present to the hidden window's surface";
        Release();
        return false;
    }

    // Surface capabilities and the rest are checked by Patch, with the info the game's swapchain is made from.
    VkSwapchainCreateInfoKHR probe {};

    if (!Patch(game, &probe, why))
    {
        Release();
        return false;
    }

    if (!MakeOutput(game, why) || !MakeRing(why))
    {
        Release();
        return false;
    }

    return true;
}

bool VkPresentBridgeCore::MakeOutput(const VkSwapchainCreateInfoKHR& game, std::string& why)
{
    DXGI_SWAP_CHAIN_DESC desc {};
    desc.BufferDesc.Width = _width;
    desc.BufferDesc.Height = _height;
    desc.BufferDesc.Format = _dxgi;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = std::clamp<UINT>(game.minImageCount, 2, 8);
    desc.OutputWindow = _target.realWindow;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = _target.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    if (!_make(desc, _output, why) || _output == nullptr)
    {
        if (why.empty())
            why = "the D3D12 swapchain on the game's window could not be made";

        _output.Reset();
        return false;
    }

    DXGI_SWAP_CHAIN_DESC made {};
    _output->GetDesc(&made);
    _outputFlags = made.Flags;
    _tearingSwapchain = (made.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;

    // A float swapchain in SRGB_NONLINEAR holds sRGB-encoded values on Vulkan, which DXGI would read as scRGB unless told.
    DXGI_COLOR_SPACE_TYPE space;
    UINT support = 0;

    if (BridgeColorSpace(game.imageColorSpace, &space) &&
        SUCCEEDED(_output->CheckColorSpaceSupport(space, &support)) &&
        (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0)
        _output->SetColorSpace1(space);

    _syncInterval = game.presentMode == VK_PRESENT_MODE_FIFO_KHR || game.presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR
                        ? 1
                        : 0;
    return true;
}

bool VkPresentBridgeCore::MakeRing(std::string& why)
{
    ID3D12Device* device = _target.device12;

    for (auto& allocator : _allocators)
    {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
        {
            why = "could not make the copy command allocators";
            return false;
        }
    }

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _allocators[0].Get(), nullptr,
                                         IID_PPV_ARGS(&_list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_copyFence))))
    {
        why = "could not make the copy command list";
        return false;
    }

    _list->Close();
    _copyValue = 0;
    _slot = 0;

    for (auto& value : _slotValue)
        value = 0;

    return true;
}

bool VkPresentBridgeCore::Patch(const VkSwapchainCreateInfoKHR& game, VkSwapchainCreateInfoKHR* out,
                                std::string& why) const
{
    *out = game;
    out->surface = _surface;
    out->imageExtent = { _width, _height };

    VkSurfaceCapabilitiesKHR caps {};

    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_target.physical, _surface, &caps) != VK_SUCCESS)
    {
        why = "the hidden surface reports no capabilities";
        return false;
    }

    // The images are copied one to one: the hidden window must be exactly the size the game asked for (a process that
    // is not DPI aware can get a different one).
    if (caps.currentExtent.width != 0xFFFFFFFFu &&
        (caps.currentExtent.width != _width || caps.currentExtent.height != _height))
    {
        why = std::format("the hidden surface is {}x{}, the game's swapchain {}x{}", caps.currentExtent.width,
                          caps.currentExtent.height, _width, _height);
        return false;
    }

    if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0 ||
        (caps.supportedUsageFlags & game.imageUsage) != game.imageUsage)
    {
        why = std::format("the hidden surface does not allow the swapchain's usage (supported 0x{:X}, wanted 0x{:X})",
                          (unsigned) caps.supportedUsageFlags, (unsigned) game.imageUsage);
        return false;
    }

    out->minImageCount = std::max(game.minImageCount, caps.minImageCount);

    if (caps.maxImageCount != 0)
        out->minImageCount = std::min(out->minImageCount, caps.maxImageCount);

    out->preTransform = caps.currentTransform;

    if ((caps.supportedCompositeAlpha & game.compositeAlpha) == 0)
    {
        out->compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0
                                  ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                  : (VkCompositeAlphaFlagBitsKHR) (caps.supportedCompositeAlpha &
                                                                   -(int) caps.supportedCompositeAlpha);
    }

    // Never shown, so never paced: the game's own present mode only matters for the D3D12 swapchain.
    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(_target.physical, _surface, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(_target.physical, _surface, &modeCount, modes.data());
    out->presentMode = VK_PRESENT_MODE_FIFO_KHR;

    for (const auto wanted : { VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR })
    {
        if (std::find(modes.begin(), modes.end(), wanted) != modes.end())
        {
            out->presentMode = wanted;
            break;
        }
    }

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(_target.physical, _surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(_target.physical, _surface, &formatCount, formats.data());

    const VkSurfaceFormatKHR* sameFormat = nullptr;

    for (const auto& entry : formats)
    {
        if (entry.format != game.imageFormat)
            continue;

        if (entry.colorSpace == game.imageColorSpace)
        {
            sameFormat = &entry;
            break;
        }

        if (sameFormat == nullptr)
            sameFormat = &entry;
    }

    if (sameFormat == nullptr)
    {
        why = std::format("the hidden surface does not offer the swapchain's format ({})", (int) game.imageFormat);
        return false;
    }

    // The images hold the same bits in any colour space; the D3D12 swapchain is told the game's.
    out->imageColorSpace = sameFormat->colorSpace;
    return true;
}

bool VkPresentBridgeCore::Resize(const VkSwapchainCreateInfoKHR& game, std::string& why)
{
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;

    if (!BridgeSwapchainFormat(game.imageFormat, &dxgi))
    {
        why = std::format("the swapchain's format ({}) is not one the bridge carries", (int) game.imageFormat);
        return false;
    }

    _syncInterval = game.presentMode == VK_PRESENT_MODE_FIFO_KHR || game.presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR
                        ? 1
                        : 0;

    if (game.imageExtent.width == _width && game.imageExtent.height == _height && game.imageFormat == _format &&
        game.imageColorSpace == _space)
        return true;

    if (_output == nullptr)
    {
        why = "the bridge has no D3D12 swapchain to resize";
        return false;
    }

    WaitIdle();
    DropBackBufferReferences();
    SetHiddenSize(game.imageExtent.width, game.imageExtent.height);

    // The flags stay the swapchain's own (DXGI requires it); count 0 keeps the buffer count.
    const HRESULT hr = _output->ResizeBuffers(0, game.imageExtent.width, game.imageExtent.height, dxgi, _outputFlags);

    if (FAILED(hr))
    {
        why = std::format("ResizeBuffers on the D3D12 swapchain failed: 0x{:X}", (unsigned) hr);
        return false;
    }

    _width = game.imageExtent.width;
    _height = game.imageExtent.height;
    _format = game.imageFormat;
    _space = game.imageColorSpace;
    _dxgi = dxgi;

    DXGI_COLOR_SPACE_TYPE space;
    UINT support = 0;

    if (BridgeColorSpace(game.imageColorSpace, &space) && SUCCEEDED(_output->CheckColorSpaceSupport(space, &support)) &&
        (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0)
        _output->SetColorSpace1(space);

    return true;
}

bool VkPresentBridgeCore::CopyFrame(ID3D12Resource* source, ID3D12Fence* waitFence, uint64_t waitValue,
                                    ID3D12Fence* afterFence, uint64_t afterValue, ID3D12Fence* signalFence,
                                    uint64_t signalValue, std::string& why)
{
    if (_output == nullptr || source == nullptr || _list == nullptr)
    {
        why = "the bridge has no D3D12 swapchain";
        return false;
    }

    const uint32_t slot = _slot % kRing;

    if (!WaitFence(_copyFence.Get(), _slotValue[slot]))
    {
        why = "the previous copy into the D3D12 swapchain did not finish";
        return false;
    }

    ComPtr<ID3D12Resource> back;

    if (FAILED(_output->GetBuffer(_output->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back))) || back == nullptr)
    {
        why = "could not get the D3D12 swapchain's back buffer";
        return false;
    }

    const D3D12_RESOURCE_DESC from = source->GetDesc();
    const D3D12_RESOURCE_DESC to = back->GetDesc();

    if (from.Width != to.Width || from.Height != to.Height)
    {
        why = std::format("the picture is {}x{}, the D3D12 swapchain {}x{}", (unsigned) from.Width, from.Height,
                          (unsigned) to.Width, to.Height);
        return false;
    }

    if (FAILED(_allocators[slot]->Reset()) || FAILED(_list->Reset(_allocators[slot].Get(), nullptr)))
    {
        why = "could not reset the copy command list";
        return false;
    }

    D3D12_RESOURCE_BARRIER barriers[2] {};
    barriers[0].Type = barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = source;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.Subresource = barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Transition.pResource = back.Get();
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    _list->ResourceBarrier(2, barriers);

    _list->CopyResource(back.Get(), source);

    std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
    std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
    _list->ResourceBarrier(2, barriers);

    if (FAILED(_list->Close()))
    {
        why = "could not close the copy command list";
        return false;
    }

    ID3D12CommandQueue* queue = _target.queue12;

    if (waitFence != nullptr)
        queue->Wait(waitFence, waitValue);

    if (afterFence != nullptr)
        queue->Wait(afterFence, afterValue);

    ID3D12CommandList* lists[] = { _list.Get() };
    queue->ExecuteCommandLists(1, lists);

    _slotValue[slot] = ++_copyValue;
    queue->Signal(_copyFence.Get(), _copyValue);

    if (signalFence != nullptr)
        queue->Signal(signalFence, signalValue);

    ++_slot;
    return true;
}

bool VkPresentBridgeCore::RealWindowResized() const
{
    RECT rect {};

    if (_target.realWindow == nullptr || IsIconic(_target.realWindow) || !GetClientRect(_target.realWindow, &rect))
        return false;

    const LONG width = rect.right - rect.left;
    const LONG height = rect.bottom - rect.top;

    return width > 0 && height > 0 && ((uint32_t) width != _width || (uint32_t) height != _height);
}

void VkPresentBridgeCore::WaitIdle()
{
    if (_copyFence == nullptr || _target.queue12 == nullptr)
        return;

    // A signal behind everything queued so far, the swapchain's presents included: ResizeBuffers and the swapchain's
    // release need the back buffers out of the queue.
    _target.queue12->Signal(_copyFence.Get(), ++_copyValue);
    WaitFence(_copyFence.Get(), _copyValue);
}

void VkPresentBridgeCore::DropBackBufferReferences()
{
    // A recorded command list keeps the back buffer it copied into alive until it is reset, and ResizeBuffers (or the
    // swapchain's release) needs every reference gone. Nothing is in flight here (the caller waited).
    if (_list == nullptr || _allocators[0] == nullptr)
        return;

    for (auto& allocator : _allocators)
        allocator->Reset();

    if (SUCCEEDED(_list->Reset(_allocators[0].Get(), nullptr)))
        _list->Close();
}

void VkPresentBridgeCore::ReleaseOutput()
{
    if (_output == nullptr)
        return;

    WaitIdle();
    DropBackBufferReferences();

    if (_release)
        _release(_output.Get());

    _output.Reset();
}

void VkPresentBridgeCore::ReleaseSurface()
{
    if (_surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(_target.instance, _surface, nullptr);
        _surface = VK_NULL_HANDLE;
    }

    if (_hidden != nullptr)
    {
        DestroyWindow(_hidden);
        _hidden = nullptr;
    }
}

void VkPresentBridgeCore::Release()
{
    ReleaseOutput();
    ReleaseSurface();

    for (auto& allocator : _allocators)
        allocator.Reset();

    _list.Reset();
    _copyFence.Reset();
    _copyValue = 0;
}

} // namespace native
