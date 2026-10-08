// GPU test for native/VkPresentBridgeCore.cpp, the Vulkan present bridge, no game: a windowed Vulkan "game" whose
// swapchain lives on the bridge's hidden window, and a plain D3D12 swapchain on the visible window.
//   - for each swapchain format the bridge carries (B8G8R8A8 UNORM and SRGB, R8G8B8A8, A2B10G10R10, R16G16B16A16F): a
//     pattern written into the game's swapchain image reaches the D3D12 swapchain's back buffer bit for bit (the same
//     steps the frame source takes: copy into the shared picture, D3D12 waits on the timeline, CopyFrame),
//   - the hidden Vulkan present and the D3D12 present both succeed, the next frame waits on the previous one on the GPU,
//   - ten resizes (the game's new swapchain with oldSwapchain, the bridge's Resize) leave video memory where it was, and
//     every size still carries the pattern,
//   - RealWindowResized sees a window that is not the swapchain's size and stops seeing it after the resize,
//   - the D3D12 debug layer reports no error or warning from the first frame to the last.
//
//   vcvars64, then from the repo root:
//   cl /std:c++20 /EHsc /O2 /Iexternal\vulkan\include tests\nr_vk_present_bridge_gpu.cpp
//      OptiScaler\native\VkPresentBridgeCore.cpp OptiScaler\native\SharedFrameVk.cpp d3d12.lib dxgi.lib user32.lib
//      vulkan-1.lib /link /LIBPATH:OptiScaler\library\vulkan

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../OptiScaler/native/SharedFrameVk.h"
#include "../OptiScaler/native/VkPresentBridgeCore.h"

using Microsoft::WRL::ComPtr;

namespace
{

bool Check(const char* what, bool value)
{
    printf("  %-90s %s\n", what, value ? "ok" : "FAIL");
    return value;
}

uint8_t Pattern(uint32_t x, uint32_t y, uint32_t seed) { return (uint8_t) (x * 7 + y * 13 + seed * 29 + (x >> 8)); }

LRESULT CALLBACK TestProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void Pump()
{
    MSG msg;

    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// The window the "game" runs in; its client area is exactly width x height.
HWND MakeWindow(uint32_t width, uint32_t height)
{
    WNDCLASSEXW wc {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TestProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BridgeTestWindow";
    RegisterClassExW(&wc);

    RECT rect { 0, 0, (LONG) width, (LONG) height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"bridge test", WS_OVERLAPPEDWINDOW, 100, 100,
                                rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    Pump();
    return hwnd;
}

void ResizeClient(HWND hwnd, uint32_t width, uint32_t height)
{
    RECT rect { 0, 0, (LONG) width, (LONG) height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(hwnd, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    Pump();
}

struct Vk
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory {};

    bool Init()
    {
        uint32_t layerless = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &layerless, nullptr);
        std::vector<VkExtensionProperties> available(layerless);
        vkEnumerateInstanceExtensionProperties(nullptr, &layerless, available.data());
        std::vector<const char*> extensions = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };

        for (const auto& e : available)
            if (strcmp(e.extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) == 0)
                extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);

        VkApplicationInfo app {};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo instanceInfo {};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &app;
        instanceInfo.enabledExtensionCount = (uint32_t) extensions.size();
        instanceInfo.ppEnabledExtensionNames = extensions.data();

        if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS)
            return false;

        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());

        for (auto candidate : devices)
        {
            VkPhysicalDeviceProperties properties {};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            LUID luid {};

            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
                native::VkPhysicalDeviceLuid(candidate, luid))
            {
                physical = candidate;
                printf("Vulkan device: %s\n", properties.deviceName);
                break;
            }
        }

        if (physical == VK_NULL_HANDLE)
            return false;

        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
        std::vector<VkQueueFamilyProperties> familyList(families);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, familyList.data());

        for (uint32_t i = 0; i < families; ++i)
        {
            if (familyList[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                family = i;
                break;
            }
        }

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo {};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                                           VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };

        VkPhysicalDeviceVulkan12Features features12 {};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        features12.timelineSemaphore = VK_TRUE;

        VkDeviceCreateInfo deviceInfo {};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.pNext = &features12;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 3;
        deviceInfo.ppEnabledExtensionNames = deviceExtensions;

        if (vkCreateDevice(physical, &deviceInfo, nullptr, &device) != VK_SUCCESS)
            return false;

        vkGetDeviceQueue(device, family, 0, &queue);
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);

        VkCommandPoolCreateInfo poolInfo {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        return vkCreateCommandPool(device, &poolInfo, nullptr, &pool) == VK_SUCCESS;
    }

    uint32_t HostMemory(uint32_t bits) const
    {
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & want) == want)
                return i;

        return UINT32_MAX;
    }

    VkCommandBuffer Command()
    {
        VkCommandBufferAllocateInfo allocate {};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(device, &allocate, &cmd);
        VkCommandBufferBeginInfo begin {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
        return cmd;
    }

    VkSemaphore Binary()
    {
        VkSemaphoreCreateInfo info {};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkSemaphore semaphore = VK_NULL_HANDLE;
        vkCreateSemaphore(device, &info, nullptr, &semaphore);
        return semaphore;
    }
};

struct Dx
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter3> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> idle;
    HANDLE event = nullptr;
    UINT64 idleValue = 0;

    bool Init(const LUID& luid)
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            debug->EnableDebugLayer();

        if (FAILED(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&factory))) &&
            FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;

        if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) ||
            FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
            return false;

        D3D12_COMMAND_QUEUE_DESC queueDesc {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
        list->Close();
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&idle));
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return queue != nullptr && list != nullptr && idle != nullptr;
    }

    void WaitIdle()
    {
        queue->Signal(idle.Get(), ++idleValue);
        idle->SetEventOnCompletion(idleValue, event);
        WaitForSingleObject(event, INFINITE);
    }

    ComPtr<ID3D12Resource> Readback(UINT64 size)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> buffer;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&buffer));
        return buffer;
    }

    // Messages the debug layer stored (errors and warnings both: the run is expected to be clean).
    UINT64 DebugMessages()
    {
        ComPtr<ID3D12InfoQueue> info;

        if (FAILED(device.As(&info)))
            return 0;

        UINT64 bad = 0;
        const UINT64 count = info->GetNumStoredMessages();

        for (UINT64 i = 0; i < count; ++i)
        {
            SIZE_T size = 0;
            info->GetMessage(i, nullptr, &size);
            std::vector<char> bytes(size);
            auto* message = (D3D12_MESSAGE*) bytes.data();
            info->GetMessage(i, message, &size);

            if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            {
                printf("    D3D12: %s\n", message->pDescription);
                ++bad;
            }
        }

        info->ClearStoredMessages();
        return bad;
    }

    UINT64 VideoMemoryUsed()
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO info {};
        adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
        return info.CurrentUsage;
    }
};

struct FormatCase
{
    const char* name;
    VkFormat format;
    VkColorSpaceKHR space;
    uint32_t bytes;
};

// The "game": a Vulkan swapchain on the bridge's hidden surface and what a frame does with it.
struct Game
{
    Vk& vk;
    Dx& dx;
    native::VkInterop& interop;
    native::SharedFenceVk& fence;
    native::VkPresentBridgeCore& bridge;
    FormatCase format {};
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    native::SharedImageVk shared;
    VkBuffer upload = VK_NULL_HANDLE;
    VkDeviceMemory uploadMemory = VK_NULL_HANDLE;
    void* uploadMapped = nullptr;
    uint64_t lastDone = 0;
    uint32_t frames = 0;

    Game(Vk& v, Dx& d, native::VkInterop& i, native::SharedFenceVk& f, native::VkPresentBridgeCore& b)
        : vk(v), dx(d), interop(i), fence(f), bridge(b)
    {
    }

    VkSwapchainCreateInfoKHR Info(uint32_t width, uint32_t height) const
    {
        VkSwapchainCreateInfoKHR info {};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.minImageCount = 3;
        info.imageFormat = format.format;
        info.imageColorSpace = format.space;
        info.imageExtent = { width, height };
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        info.clipped = VK_TRUE;
        return info;
    }

    // The game's vkCreateSwapchainKHR, patched by the bridge. The old swapchain is destroyed afterwards, as a game does.
    bool MakeSwapchain(const VkSwapchainCreateInfoKHR& wanted, std::string& why)
    {
        VkSwapchainCreateInfoKHR patched {};

        if (!bridge.Patch(wanted, &patched, why))
            return false;

        patched.oldSwapchain = swapchain;
        VkSwapchainKHR made = VK_NULL_HANDLE;

        if (const VkResult result = vkCreateSwapchainKHR(vk.device, &patched, nullptr, &made); result != VK_SUCCESS)
        {
            why = "vkCreateSwapchainKHR failed: " + std::to_string((int) result);
            return false;
        }

        if (swapchain != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(vk.device);
            vkDestroySwapchainKHR(vk.device, swapchain, nullptr);
        }

        swapchain = made;
        uint32_t count = 0;
        vkGetSwapchainImagesKHR(vk.device, swapchain, &count, nullptr);
        images.resize(count);
        vkGetSwapchainImagesKHR(vk.device, swapchain, &count, images.data());
        return true;
    }

    bool EnsureBuffers(uint32_t width, uint32_t height)
    {
        const VkDeviceSize size = (VkDeviceSize) width * height * format.bytes;

        if (upload != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(vk.device, upload, nullptr);
            vkFreeMemory(vk.device, uploadMemory, nullptr);
            upload = VK_NULL_HANDLE;
        }

        VkBufferCreateInfo bufferInfo {};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        if (vkCreateBuffer(vk.device, &bufferInfo, nullptr, &upload) != VK_SUCCESS)
            return false;

        VkMemoryRequirements requirements {};
        vkGetBufferMemoryRequirements(vk.device, upload, &requirements);
        VkMemoryAllocateInfo allocate {};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = vk.HostMemory(requirements.memoryTypeBits);
        return vkAllocateMemory(vk.device, &allocate, nullptr, &uploadMemory) == VK_SUCCESS &&
               vkBindBufferMemory(vk.device, upload, uploadMemory, 0) == VK_SUCCESS &&
               vkMapMemory(vk.device, uploadMemory, 0, VK_WHOLE_SIZE, 0, &uploadMapped) == VK_SUCCESS;
    }

    // One frame through the bridge. Returns the bytes of the D3D12 swapchain's back buffer after the copy (rows packed),
    // or empty on a failure.
    std::vector<uint8_t> Frame(uint32_t width, uint32_t height, uint32_t seed, std::string& why)
    {
        std::vector<uint8_t> result;
        const uint32_t rowBytes = width * format.bytes;
        auto* pattern = (uint8_t*) uploadMapped;

        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < rowBytes; ++x)
                pattern[y * rowBytes + x] = Pattern(x, y, seed);

        VkSemaphore acquired = vk.Binary();
        VkSemaphore rendered = vk.Binary();
        VkSemaphore presentWait = vk.Binary();
        uint32_t index = 0;

        if (vkAcquireNextImageKHR(vk.device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index) != VK_SUCCESS)
        {
            why = "vkAcquireNextImageKHR failed";
            return result;
        }

        const VkImage image = images[index];

        // The game's render: the pattern into the swapchain image, left in PRESENT_SRC.
        VkCommandBuffer render = vk.Command();
        VkImageMemoryBarrier barrier {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(render, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
        VkBufferImageCopy region {};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { width, height, 1 };
        vkCmdCopyBufferToImage(render, upload, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(render, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        vkEndCommandBuffer(render);

        const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired;
        submit.pWaitDstStageMask = &transfer;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &render;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &rendered;
        vkQueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE);

        // The frame source's copy in: waits on the game's semaphore and on D3D12 having finished with the last picture.
        DXGI_FORMAT dxgi;
        VkFormat sharedFormat;
        native::VkSharedPictureFormat(format.format, &dxgi, &sharedFormat);

        if (!shared.Matches(width, height, sharedFormat))
        {
            vkDeviceWaitIdle(vk.device);
            dx.WaitIdle();

            if (!shared.Create(dx.device.Get(), interop, width, height, dxgi, sharedFormat, true))
            {
                why = shared.Error();
                return result;
            }
        }

        VkCommandBuffer copyIn = vk.Command();
        native::RecordCopyToShared(copyIn, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, shared, vk.family);
        vkEndCommandBuffer(copyIn);

        const uint64_t copied = fence.Next();
        const VkSemaphore waits[2] = { rendered, fence.Semaphore() };
        const uint64_t waitValues[2] = { 0, lastDone };
        const VkPipelineStageFlags stages[2] = { VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT };
        VkTimelineSemaphoreSubmitInfo values {};
        values.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        values.waitSemaphoreValueCount = 2;
        values.pWaitSemaphoreValues = waitValues;
        values.signalSemaphoreValueCount = 1;
        values.pSignalSemaphoreValues = &copied;
        const VkSemaphore timeline = fence.Semaphore();
        submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.pNext = &values;
        submit.waitSemaphoreCount = 2;
        submit.pWaitSemaphores = waits;
        submit.pWaitDstStageMask = stages;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &copyIn;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &timeline;
        vkQueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE);

        // The bridge: the shared picture into the D3D12 swapchain's back buffer, on the D3D12 queue.
        const uint64_t done = fence.Next();

        if (!bridge.CopyFrame(shared.Res12(), fence.Fence12(), copied, nullptr, 0, fence.Fence12(), done, why))
            return result;

        lastDone = done;

        // What the back buffer holds now, read through the same queue before the present.
        ComPtr<ID3D12Resource> back;
        bridge.Output()->GetBuffer(bridge.Output()->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back));
        const D3D12_RESOURCE_DESC desc = back->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT64 total = 0;
        dx.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
        auto readback = dx.Readback(total);

        dx.allocator->Reset();
        dx.list->Reset(dx.allocator.Get(), nullptr);
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = back.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        dx.list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION src { back.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        D3D12_TEXTURE_COPY_LOCATION dst { readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        dst.PlacedFootprint = footprint;
        dx.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        dx.list->ResourceBarrier(1, &b);
        dx.list->Close();
        ID3D12CommandList* lists[] = { dx.list.Get() };
        dx.queue->ExecuteCommandLists(1, lists);
        dx.WaitIdle();

        // The list holds the back buffer until it is reset, and ResizeBuffers needs every reference gone.
        dx.allocator->Reset();
        dx.list->Reset(dx.allocator.Get(), nullptr);
        dx.list->Close();
        back.Reset();

        uint8_t* read = nullptr;
        readback->Map(0, nullptr, (void**) &read);
        result.resize((size_t) rowBytes * height);

        for (uint32_t y = 0; y < height; ++y)
            memcpy(result.data() + (size_t) y * rowBytes, read + y * footprint.Footprint.RowPitch, rowBytes);

        readback->Unmap(0, nullptr);

        // The return: the hidden present waits only on the copy in; both presents must succeed.
        VkSubmitInfo empty {};
        empty.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        empty.signalSemaphoreCount = 1;
        empty.pSignalSemaphores = &presentWait;
        vkQueueSubmit(vk.queue, 1, &empty, VK_NULL_HANDLE);

        VkPresentInfoKHR present {};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &presentWait;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &index;
        const VkResult vkPresent = vkQueuePresentKHR(vk.queue, &present);
        const HRESULT dxPresent = bridge.Output()->Present(bridge.SyncInterval(), bridge.PresentFlags());

        if (vkPresent != VK_SUCCESS && vkPresent != VK_SUBOPTIMAL_KHR)
            why = "the hidden Vulkan present failed: " + std::to_string((int) vkPresent);
        else if (FAILED(dxPresent))
            why = "the D3D12 present failed";

        // The binary semaphores are consumed (by the submits and the present); this wait makes it safe to destroy them.
        vkQueueWaitIdle(vk.queue);
        vkDestroySemaphore(vk.device, acquired, nullptr);
        vkDestroySemaphore(vk.device, rendered, nullptr);
        vkDestroySemaphore(vk.device, presentWait, nullptr);
        vkResetCommandPool(vk.device, vk.pool, 0);
        ++frames;
        Pump();

        if (!why.empty())
            result.clear();

        return result;
    }

    bool Matches(const std::vector<uint8_t>& got, uint32_t width, uint32_t height, uint32_t seed) const
    {
        const uint32_t rowBytes = width * format.bytes;

        if (got.size() != (size_t) rowBytes * height)
            return false;

        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < rowBytes; ++x)
                if (got[(size_t) y * rowBytes + x] != Pattern(x, y, seed))
                    return false;

        return true;
    }

    void Destroy()
    {
        vkDeviceWaitIdle(vk.device);
        dx.WaitIdle();
        shared.Reset();

        if (swapchain != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(vk.device, swapchain, nullptr);

        if (upload != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(vk.device, upload, nullptr);
            vkFreeMemory(vk.device, uploadMemory, nullptr);
        }

        swapchain = VK_NULL_HANDLE;
        upload = VK_NULL_HANDLE;
        images.clear();
    }
};

// A plain D3D12 swapchain on the real window: what the glue does through frame generation.
native::MakeOutputFn PlainOutput(Dx& dx)
{
    return [&dx](DXGI_SWAP_CHAIN_DESC& desc, ComPtr<IDXGISwapChain4>& out, std::string& why)
    {
        ComPtr<IDXGISwapChain> sc;

        if (FAILED(dx.factory->CreateSwapChain(dx.queue.Get(), &desc, &sc)) || FAILED(sc.As(&out)))
        {
            why = "CreateSwapChain on the real window failed";
            return false;
        }

        return true;
    };
}

bool RunFormat(Vk& vk, Dx& dx, native::VkInterop& interop, native::SharedFenceVk& fence, const FormatCase& f)
{
    bool ok = true;
    constexpr uint32_t kBaseW = 640;
    constexpr uint32_t kBaseH = 360;
    HWND window = MakeWindow(kBaseW, kBaseH);

    // Which colour spaces the hidden surface offers is the bridge's to handle; a format the driver does not list for a
    // surface at all cannot be tested.
    native::VkPresentBridgeCore bridge;
    Game game(vk, dx, interop, fence, bridge);
    game.format = f;

    native::BridgeTarget target;
    target.instance = vk.instance;
    target.physical = vk.physical;
    target.createSurface = vkCreateWin32SurfaceKHR;
    target.realWindow = window;
    target.device12 = dx.device.Get();
    target.queue12 = dx.queue.Get();
    target.tearing = true;

    std::string why;
    const auto first = game.Info(kBaseW, kBaseH);

    if (!bridge.Create(target, first, PlainOutput(dx), {}, why))
    {
        if (why.find("does not offer the swapchain's format") != std::string::npos)
        {
            printf("  SKIP: %s\n", why.c_str());
            DestroyWindow(window);
            return true;
        }

        printf("    %s\n", why.c_str());
        DestroyWindow(window);
        return Check("the bridge is made (hidden window, surface, D3D12 swapchain)", false);
    }

    ok &= Check("the bridge is made (hidden window, surface, D3D12 swapchain)", true);

    VkSwapchainCreateInfoKHR patched {};
    bridge.Patch(first, &patched, why);
    printf("    hidden swapchain: %ux%u, present mode %d, min images %u, colour space %d\n", patched.imageExtent.width,
           patched.imageExtent.height, (int) patched.presentMode, patched.minImageCount, (int) patched.imageColorSpace);
    ok &= Check("the hidden swapchain does not wait for a vsync", patched.presentMode != VK_PRESENT_MODE_FIFO_KHR);

    ok &= Check("the game's swapchain on the hidden surface", game.MakeSwapchain(first, why));
    ok &= Check("the game's upload buffer", game.EnsureBuffers(kBaseW, kBaseH));

    if (!ok)
    {
        printf("    %s\n", why.c_str());
        game.Destroy();
        bridge.Release();
        DestroyWindow(window);
        return false;
    }

    // A few frames: every one must arrive bit for bit (the third swapchain image and the first reuse included).
    bool same = true;
    std::string frameError;

    for (uint32_t i = 0; i < 6 && same; ++i)
    {
        const auto got = game.Frame(kBaseW, kBaseH, i + 1, frameError);
        same = frameError.empty() && game.Matches(got, kBaseW, kBaseH, i + 1);
    }

    if (!frameError.empty())
        printf("    %s\n", frameError.c_str());

    ok &= Check("six frames reach the D3D12 back buffer bit for bit", same);
    ok &= Check("no D3D12 debug layer error or warning", dx.DebugMessages() == 0);
    ok &= Check("the real window is the swapchain's size", !bridge.RealWindowResized());

    // Ten resizes: the window changes, the bridge and the game's swapchain follow; the sizes cycle through three, so the
    // memory after the first cycle is the memory after the last.
    const uint32_t sizes[3][2] = { { 800, 450 }, { 480, 270 }, { 640, 360 } };
    UINT64 afterFirstCycle = 0;
    bool resized = true;
    bool detected = true;
    bool carried = true;

    for (uint32_t i = 0; i < 10 && resized && carried; ++i)
    {
        const uint32_t w = sizes[i % 3][0];
        const uint32_t h = sizes[i % 3][1];
        ResizeClient(window, w, h);
        detected &= bridge.RealWindowResized();

        const auto wanted = game.Info(w, h);
        resized &= bridge.Resize(wanted, why) && game.MakeSwapchain(wanted, why) && game.EnsureBuffers(w, h);
        detected &= !bridge.RealWindowResized();

        frameError.clear();
        const auto got = resized ? game.Frame(w, h, 50 + i, frameError) : std::vector<uint8_t> {};
        carried = resized && frameError.empty() && game.Matches(got, w, h, 50 + i);

        if (i == 2)
            afterFirstCycle = dx.VideoMemoryUsed();
    }

    if (!why.empty() || !frameError.empty())
        printf("    %s %s\n", why.c_str(), frameError.c_str());

    ok &= Check("ten resizes: Resize, the game's new swapchain with oldSwapchain, a frame", resized && carried);
    ok &= Check("the window is seen as resized before the bridge follows, and not after", detected);

    game.Destroy();
    dx.WaitIdle();
    const UINT64 end = dx.VideoMemoryUsed();
    const long long grown = (long long) end - (long long) afterFirstCycle;
    printf("    video memory after the first cycle %llu MB, at the end %llu MB\n", afterFirstCycle >> 20, end >> 20);
    ok &= Check("video memory did not grow over the last seven resizes (within 4 MB)", grown < (4ll << 20));

    bridge.Release();
    ok &= Check("the bridge lets go of the swapchain, the surface and the window", !bridge.Created());
    ok &= Check("no D3D12 debug layer error or warning after the resizes and the release", dx.DebugMessages() == 0);
    DestroyWindow(window);
    return ok;
}

} // namespace

LONG WINAPI Crashed(EXCEPTION_POINTERS* info)
{
    printf("CRASH: exception 0x%lX at %p\n", info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);

    ComPtr<IDXGIInfoQueue> queue;

    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&queue))))
    {
        const UINT64 count = queue->GetNumStoredMessages(DXGI_DEBUG_ALL);

        for (UINT64 i = 0; i < count; ++i)
        {
            SIZE_T size = 0;
            queue->GetMessage(DXGI_DEBUG_ALL, i, nullptr, &size);
            std::vector<char> bytes(size);
            auto* message = (DXGI_INFO_QUEUE_MESSAGE*) bytes.data();
            queue->GetMessage(DXGI_DEBUG_ALL, i, message, &size);
            printf("    DXGI: %s\n", message->pDescription);
        }
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter(Crashed);
    Vk vk;

    if (!vk.Init())
    {
        printf("SKIP: no Vulkan 1.2 discrete GPU with surface, swapchain and external memory support\n");
        return 0;
    }

    LUID luid {};
    native::VkPhysicalDeviceLuid(vk.physical, luid);
    Dx dx;

    if (!dx.Init(luid))
    {
        printf("FAIL: no D3D12 device on the Vulkan device's adapter\n");
        return 1;
    }

    bool ok = true;
    std::string error;
    native::VkInterop interop;
    ok &= Check("Vulkan external memory and semaphore functions", interop.Load(vk.physical, vk.device, error));
    native::SharedFenceVk fence;
    ok &= Check("D3D12 fence opened as a Vulkan timeline semaphore", fence.Create(dx.device.Get(), interop));

    if (!ok)
    {
        printf("    %s %s\n", error.c_str(), fence.Error().c_str());
        return 1;
    }

    const FormatCase cases[] = {
        { "B8G8R8A8_UNORM", VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, 4 },
        { "B8G8R8A8_SRGB", VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, 4 },
        { "R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, 4 },
        { "A2B10G10R10_UNORM", VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, 4 },
        { "R16G16B16A16_SFLOAT", VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, 8 },
    };

    dx.DebugMessages();

    for (const auto& f : cases)
    {
        printf("%s:\n", f.name);
        ok &= RunFormat(vk, dx, interop, fence, f);
    }

    fence.Reset();
    vkDestroyCommandPool(vk.device, vk.pool, nullptr);
    vkDestroyDevice(vk.device, nullptr);
    vkDestroyInstance(vk.instance, nullptr);

    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
