// GPU test for native/SharedFrameVk.cpp, the Vulkan adapter's transport, no game: a headless Vulkan device and a D3D12
// device on the same adapter, with one D3D12 fence opened in Vulkan as a timeline semaphore.
//   - for every picture format the producer takes (B8G8R8A8, R8G8B8A8, A2B10G10R10, R16G16B16A16F): a Vulkan image is
//     copied into the shared image with RecordCopyToShared, and D3D12 reads exactly its bytes,
//   - D3D12 writes another pattern into the shared image, Vulkan waits for its signal, copies it back with
//     RecordCopyFromShared and reads exactly those bytes,
//   - sharing made and dropped ten times at changing sizes leaves video memory where it was,
//   - the fence's values only rise, and both directions are ordered on the GPU with no CPU wait between the two.
//
//   vcvars64, then from the repo root:
//   cl /std:c++20 /EHsc /O2 /Iexternal\vulkan\include tests\nr_shared_frame_vk_gpu.cpp OptiScaler\native\SharedFrameVk.cpp
//      d3d12.lib dxgi.lib vulkan-1.lib /link /LIBPATH:OptiScaler\library\vulkan

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../OptiScaler/native/SharedFrameVk.h"

using Microsoft::WRL::ComPtr;

namespace
{

bool Check(const char* what, bool value)
{
    printf("  %-90s %s\n", what, value ? "ok" : "FAIL");
    return value;
}

uint8_t Pattern(uint32_t x, uint32_t y, uint32_t c, uint32_t seed)
{
    return (uint8_t) (x * 7 + y * 13 + c * 31 + seed);
}

struct Vk
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory {};

    bool Init()
    {
        VkApplicationInfo app {};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo instanceInfo {};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &app;

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

        const char* extensions[] = { VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                                     VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };

        VkPhysicalDeviceVulkan12Features features12 {};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        features12.timelineSemaphore = VK_TRUE;

        VkDeviceCreateInfo deviceInfo {};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.pNext = &features12;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 2;
        deviceInfo.ppEnabledExtensionNames = extensions;

        if (vkCreateDevice(physical, &deviceInfo, nullptr, &device) != VK_SUCCESS)
            return false;

        vkGetDeviceQueue(device, family, 0, &queue);
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);

        VkCommandPoolCreateInfo poolInfo {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        vkCreateCommandPool(device, &poolInfo, nullptr, &pool);

        VkCommandBufferAllocateInfo allocate {};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        return vkAllocateCommandBuffers(device, &allocate, &cmd) == VK_SUCCESS;
    }

    uint32_t HostMemory(uint32_t bits) const
    {
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & want) == want)
                return i;

        return UINT32_MAX;
    }

    uint32_t DeviceMemory(uint32_t bits) const
    {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                return i;

        return UINT32_MAX;
    }

    void Begin()
    {
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);
    }

    // Submits, waiting on and signalling the timeline semaphore at the given values (0: none), then waits idle on the CPU
    // only when `idle` (the reads).
    bool Submit(VkSemaphore timeline, uint64_t waitValue, uint64_t signalValue, bool idle)
    {
        vkEndCommandBuffer(cmd);

        VkTimelineSemaphoreSubmitInfo values {};
        values.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        values.waitSemaphoreValueCount = waitValue != 0 ? 1 : 0;
        values.pWaitSemaphoreValues = &waitValue;
        values.signalSemaphoreValueCount = signalValue != 0 ? 1 : 0;
        values.pSignalSemaphoreValues = &signalValue;

        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.pNext = &values;
        submit.waitSemaphoreCount = waitValue != 0 ? 1 : 0;
        submit.pWaitSemaphores = &timeline;
        submit.pWaitDstStageMask = &stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = signalValue != 0 ? 1 : 0;
        submit.pSignalSemaphores = &timeline;

        if (vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
            return false;

        return !idle || vkQueueWaitIdle(queue) == VK_SUCCESS;
    }
};

struct Dx
{
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
        // The debug layer, so an invalid copy or view of the depth formats fails the test.
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            debug->EnableDebugLayer();

        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
            FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) ||
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

    void Begin()
    {
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
    }

    void Execute()
    {
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
    }

    void WaitIdle()
    {
        queue->Signal(idle.Get(), ++idleValue);
        idle->SetEventOnCompletion(idleValue, event);
        WaitForSingleObject(event, INFINITE);
    }

    ComPtr<ID3D12Resource> Buffer(D3D12_HEAP_TYPE type, UINT64 size)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> buffer;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                        type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                                       : D3D12_RESOURCE_STATE_COPY_DEST,
                                        nullptr, IID_PPV_ARGS(&buffer));
        return buffer;
    }

    void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = from;
        barrier.Transition.StateAfter = to;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
    }

    // Errors the debug layer reported since the last call (0 when the layer is not installed).
    UINT64 DebugErrors()
    {
        ComPtr<ID3D12InfoQueue> info;

        if (FAILED(device.As(&info)))
            return 0;

        UINT64 errors = 0;
        const UINT64 count = info->GetNumStoredMessages();

        for (UINT64 i = 0; i < count; ++i)
        {
            SIZE_T size = 0;
            info->GetMessage(i, nullptr, &size);
            std::vector<char> bytes(size);
            auto* message = (D3D12_MESSAGE*) bytes.data();
            info->GetMessage(i, message, &size);

            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            {
                printf("    D3D12: %s\n", message->pDescription);
                ++errors;
            }
        }

        info->ClearStoredMessages();
        return errors;
    }

    UINT64 VideoMemoryUsed()
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO info {};
        adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
        return info.CurrentUsage;
    }
};

// A Vulkan image of the "game" plus a host buffer to fill and read it through.
struct GameImage
{
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
    void* mapped = nullptr;

    bool Create(const Vk& vk, VkFormat format, uint32_t width, uint32_t height, uint32_t bytes)
    {
        device = vk.device;
        VkImageCreateInfo info {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = { width, height, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(device, &info, nullptr, &image) != VK_SUCCESS)
            return false;

        VkMemoryRequirements requirements {};
        vkGetImageMemoryRequirements(device, image, &requirements);
        VkMemoryAllocateInfo allocate {};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = vk.DeviceMemory(requirements.memoryTypeBits);

        if (vkAllocateMemory(device, &allocate, nullptr, &imageMemory) != VK_SUCCESS ||
            vkBindImageMemory(device, image, imageMemory, 0) != VK_SUCCESS)
            return false;

        VkBufferCreateInfo bufferInfo {};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = (VkDeviceSize) width * height * bytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS)
            return false;

        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = vk.HostMemory(requirements.memoryTypeBits);

        return vkAllocateMemory(device, &allocate, nullptr, &bufferMemory) == VK_SUCCESS &&
               vkBindBufferMemory(device, buffer, bufferMemory, 0) == VK_SUCCESS &&
               vkMapMemory(device, bufferMemory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS;
    }

    void Destroy()
    {
        vkDestroyImage(device, image, nullptr);
        vkFreeMemory(device, imageMemory, nullptr);
        vkDestroyBuffer(device, buffer, nullptr);
        vkFreeMemory(device, bufferMemory, nullptr);
    }

    void Transfer(VkCommandBuffer cmd, bool toImage, VkImageLayout from, VkImageLayout to, uint32_t width,
                  uint32_t height)
    {
        VkImageMemoryBarrier barrier {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = from;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &barrier);

        VkBufferImageCopy region {};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { width, height, 1 };

        if (toImage)
            vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        else
            vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);

        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_HOST_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = to;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &barrier);
    }
};

struct FormatCase
{
    const char* name;
    VkFormat format;
    uint32_t bytes;
};

bool RoundTrip(Vk& vk, Dx& dx, native::VkInterop& interop, native::SharedFenceVk& fence, const FormatCase& f)
{
    constexpr uint32_t kWidth = 333; // odd, so a row pitch that is not the width shows
    constexpr uint32_t kHeight = 187;

    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
    VkFormat sharedFormat = VK_FORMAT_UNDEFINED;
    bool ok = Check("the format is one the producer takes", native::VkSharedPictureFormat(f.format, &dxgi, &sharedFormat));

    native::SharedImageVk shared;
    ok &= Check("shared image made on D3D12 and opened in Vulkan",
                shared.Create(dx.device.Get(), interop, kWidth, kHeight, dxgi, sharedFormat, true));

    if (!ok)
    {
        printf("    %s\n", shared.Error().c_str());
        return false;
    }

    GameImage game;
    ok &= Check("the game's image", game.Create(vk, f.format, kWidth, kHeight, f.bytes));

    if (!ok)
        return false;

    const uint32_t rowBytes = kWidth * f.bytes;
    auto* bytes = (uint8_t*) game.mapped;

    for (uint32_t y = 0; y < kHeight; ++y)
        for (uint32_t x = 0; x < rowBytes; ++x)
            bytes[y * rowBytes + x] = Pattern(x, y, 0, 1);

    // The "present": the game's image, in GENERAL (what the swapchain's PRESENT_SRC stands for here), into the shared
    // image, then the timeline is signalled. No CPU wait follows.
    const uint64_t copied = fence.Next();
    vk.Begin();
    game.Transfer(vk.cmd, true, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, kWidth, kHeight);
    native::RecordCopyToShared(vk.cmd, game.image, VK_IMAGE_LAYOUT_GENERAL, shared, vk.family);
    ok &= Check("Vulkan copy into the shared image submitted", vk.Submit(fence.Semaphore(), 0, copied, false));

    // D3D12 waits on the GPU, reads the shared image into a readback buffer.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 total = 0;
    const auto desc = shared.Res12()->GetDesc();
    dx.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    auto readback = dx.Buffer(D3D12_HEAP_TYPE_READBACK, total);
    auto upload = dx.Buffer(D3D12_HEAP_TYPE_UPLOAD, total);

    dx.queue->Wait(fence.Fence12(), copied);
    dx.Begin();
    dx.Transition(shared.Res12(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src { shared.Res12(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION dst { readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint = footprint;
    dx.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    dx.Transition(shared.Res12(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    dx.Execute();
    dx.WaitIdle();

    uint8_t* read = nullptr;
    readback->Map(0, nullptr, (void**) &read);
    bool same = true;

    for (uint32_t y = 0; y < kHeight && same; ++y)
        same = memcmp(read + y * footprint.Footprint.RowPitch, bytes + y * rowBytes, rowBytes) == 0;

    readback->Unmap(0, nullptr);
    ok &= Check("D3D12 reads exactly the game's bytes", same);

    // D3D12 writes another pattern and signals; Vulkan waits on the GPU, copies it back into the game's image.
    uint8_t* write = nullptr;
    upload->Map(0, nullptr, (void**) &write);

    for (uint32_t y = 0; y < kHeight; ++y)
        for (uint32_t x = 0; x < rowBytes; ++x)
            write[y * footprint.Footprint.RowPitch + x] = Pattern(x, y, 0, 77);

    upload->Unmap(0, nullptr);

    dx.Begin();
    dx.Transition(shared.Res12(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION from { upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    from.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION to { shared.Res12(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    dx.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    dx.Transition(shared.Res12(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    dx.Execute();
    const uint64_t processed = fence.Next();
    dx.queue->Signal(fence.Fence12(), processed);

    memset(bytes, 0, (size_t) rowBytes * kHeight);
    vk.Begin();
    native::RecordCopyFromShared(vk.cmd, shared, game.image, VK_IMAGE_LAYOUT_GENERAL, vk.family);
    game.Transfer(vk.cmd, false, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, kWidth, kHeight);
    ok &= Check("Vulkan copy back submitted, waiting on D3D12's signal", vk.Submit(fence.Semaphore(), processed, 0, true));

    same = true;

    for (uint32_t y = 0; y < kHeight && same; ++y)
        for (uint32_t x = 0; x < rowBytes && same; ++x)
            same = bytes[y * rowBytes + x] == Pattern(x, y, 0, 77);

    ok &= Check("Vulkan reads exactly what D3D12 wrote", same);

    dx.WaitIdle();
    shared.Reset();
    game.Destroy();
    return ok;
}

// The depth path: a Vulkan depth image cleared to a known value is copied by RecordDepthToBuffer into the finder's
// own buffer, from there into the shared buffer (the frame source's step), and on D3D12 into a texture of the format it
// is read through. The value must survive, and the view must be one D3D12 accepts.
bool DepthRoundTrip(Vk& vk, Dx& dx, native::VkInterop& interop, native::SharedFenceVk& fence, VkFormat format)
{
    constexpr uint32_t kWidth = 333;
    constexpr uint32_t kHeight = 187;
    constexpr float kValue = 0.375f;

    VkFormatProperties properties {};
    vkGetPhysicalDeviceFormatProperties(vk.physical, format, &properties);

    if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0)
    {
        printf("  not supported by this device, skipped\n");
        return true;
    }

    native::VkDepthCopyFormat copy;
    bool ok = Check("a depth format that crosses", native::VkDepthCopyFormatOf(format, &copy));

    const uint32_t pitch = native::DepthRowPitch(kWidth, copy.bytes);
    const uint64_t size = (uint64_t) pitch * kHeight;

    // The game's depth image.
    VkImageCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = { kWidth, kHeight, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    vkCreateImage(vk.device, &info, nullptr, &image);
    VkMemoryRequirements requirements {};
    vkGetImageMemoryRequirements(vk.device, image, &requirements);
    VkMemoryAllocateInfo allocate {};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = vk.DeviceMemory(requirements.memoryTypeBits);
    vkAllocateMemory(vk.device, &allocate, nullptr, &imageMemory);
    vkBindImageMemory(vk.device, image, imageMemory, 0);

    // The finder's own buffer.
    VkBufferCreateInfo bufferInfo {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer own = VK_NULL_HANDLE;
    VkDeviceMemory ownMemory = VK_NULL_HANDLE;
    vkCreateBuffer(vk.device, &bufferInfo, nullptr, &own);
    vkGetBufferMemoryRequirements(vk.device, own, &requirements);
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = vk.DeviceMemory(requirements.memoryTypeBits);
    vkAllocateMemory(vk.device, &allocate, nullptr, &ownMemory);
    vkBindBufferMemory(vk.device, own, ownMemory, 0);

    native::SharedBufferVk shared;
    ok &= Check("shared buffer made on D3D12 and opened in Vulkan", shared.Create(dx.device.Get(), interop, size));

    if (!ok)
    {
        printf("    %s\n", shared.Error().c_str());
        return false;
    }

    const bool stencil = format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
                         format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    const VkImageAspectFlags aspects = VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);

    vk.Begin();
    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { aspects, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    const VkClearDepthStencilValue clear { kValue, 0x5A };
    vkCmdClearDepthStencilImage(vk.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                                &barrier.subresourceRange);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);

    // What a hook records after the scene's pass ends, then what the frame source records at the present.
    native::RecordDepthToBuffer(vk.cmd, image, format, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, kWidth,
                                kHeight, own);
    const VkBufferCopy whole { 0, 0, size };
    vkCmdCopyBuffer(vk.cmd, own, shared.Buffer(), 1, &whole);

    const uint64_t copied = fence.Next();
    ok &= Check("Vulkan depth copies submitted", vk.Submit(fence.Semaphore(), 0, copied, false));

    // D3D12: into a texture of the crossing format, then read back.
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = copy.texture;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> texture;
    ok &= Check("D3D12 depth texture made",
                SUCCEEDED(dx.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                              IID_PPV_ARGS(&texture))));

    if (!ok)
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    dx.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptors));
    D3D12_SHADER_RESOURCE_VIEW_DESC view {};
    view.Format = copy.view;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    dx.device->CreateShaderResourceView(texture.Get(), &view, descriptors->GetCPUDescriptorHandleForHeapStart());

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 total = 0;
    dx.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    auto readback = dx.Buffer(D3D12_HEAP_TYPE_READBACK, total);

    dx.queue->Wait(fence.Fence12(), copied);
    dx.Begin();
    native::RecordBufferToDepthTexture(dx.list.Get(), shared.Res12(), texture.Get(), copy, kWidth, kHeight);
    dx.Transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src { texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION dst { readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint = footprint;
    dx.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    dx.Execute();
    dx.WaitIdle();

    uint8_t* read = nullptr;
    readback->Map(0, nullptr, (void**) &read);
    bool same = true;
    double worst = 0.0;

    for (uint32_t y = 0; y < kHeight && same; ++y)
    {
        const uint8_t* row = read + y * footprint.Footprint.RowPitch;

        for (uint32_t x = 0; x < kWidth && same; ++x)
        {
            double got = 0.0;

            if (copy.bytes == 2)
                got = ((const uint16_t*) row)[x] / 65535.0;
            else if (copy.texture == DXGI_FORMAT_R24G8_TYPELESS)
                got = (((const uint32_t*) row)[x] & 0xFFFFFF) / 16777215.0;
            else
                got = ((const float*) row)[x];

            const double error = got > kValue ? got - kValue : kValue - got;
            worst = error > worst ? error : worst;
            same = error <= (copy.bytes == 2 ? 1.0 / 65535.0 : 1.0 / 16777215.0);
        }
    }

    readback->Unmap(0, nullptr);
    printf("    read %s, worst error %.3g\n", same ? "the cleared value" : "ANOTHER value", worst);
    ok &= Check("D3D12 reads the cleared depth", same);
    ok &= Check("no D3D12 debug layer error (the copy and the view are valid)", dx.DebugErrors() == 0);

    vkDestroyImage(vk.device, image, nullptr);
    vkFreeMemory(vk.device, imageMemory, nullptr);
    vkDestroyBuffer(vk.device, own, nullptr);
    vkFreeMemory(vk.device, ownMemory, nullptr);
    return ok;
}

} // namespace

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    Vk vk;

    if (!vk.Init())
    {
        printf("SKIP: no Vulkan 1.2 discrete GPU with external memory and timeline semaphores\n");
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
        { "B8G8R8A8_UNORM", VK_FORMAT_B8G8R8A8_UNORM, 4 },
        { "B8G8R8A8_SRGB", VK_FORMAT_B8G8R8A8_SRGB, 4 },
        { "R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM, 4 },
        { "A2B10G10R10_UNORM", VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4 },
        { "R16G16B16A16_SFLOAT", VK_FORMAT_R16G16B16A16_SFLOAT, 8 },
    };

    for (const auto& f : cases)
    {
        printf("%s:\n", f.name);
        ok &= RoundTrip(vk, dx, interop, fence, f);
    }

    const struct
    {
        const char* name;
        VkFormat format;
    } depths[] = {
        { "D16_UNORM", VK_FORMAT_D16_UNORM },
        { "X8_D24_UNORM_PACK32", VK_FORMAT_X8_D24_UNORM_PACK32 },
        { "D24_UNORM_S8_UINT", VK_FORMAT_D24_UNORM_S8_UINT },
        { "D32_SFLOAT", VK_FORMAT_D32_SFLOAT },
        { "D32_SFLOAT_S8_UINT", VK_FORMAT_D32_SFLOAT_S8_UINT },
    };

    for (const auto& d : depths)
    {
        printf("depth %s:\n", d.name);
        ok &= DepthRoundTrip(vk, dx, interop, fence, d.format);
    }

    printf("Ten resizes:\n");
    {
        const UINT64 before = dx.VideoMemoryUsed();
        bool made = true;

        for (uint32_t i = 0; i < 10; ++i)
        {
            native::SharedImageVk shared;
            made &= shared.Create(dx.device.Get(), interop, 1280 + i * 64, 720 + i * 36, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                  VK_FORMAT_R16G16B16A16_SFLOAT, true);
        }

        dx.WaitIdle();
        const UINT64 after = dx.VideoMemoryUsed();
        const long long grown = (long long) after - (long long) before;
        printf("    video memory before %llu MB, after %llu MB\n", before >> 20, after >> 20);
        ok &= Check("every size made", made);
        ok &= Check("video memory back where it was (within 8 MB)", grown < (8ll << 20));
    }

    printf("Fence:\n");
    {
        const uint64_t a = fence.Value();
        const uint64_t b = fence.Next();
        const uint64_t c = fence.Next();
        ok &= Check("values only rise", b > a && c > b);
        uint64_t reached = 0;
        dx.queue->Signal(fence.Fence12(), c);
        dx.WaitIdle();
        vkGetSemaphoreCounterValue(vk.device, fence.Semaphore(), &reached);
        ok &= Check("a D3D12 signal is seen by the Vulkan semaphore", reached >= c);
    }

    // The output copy failed after the producer ran: the picture is handed back, and the next copy in (which waits for
    // `done` on the shared fence) must not go ahead before the producer is finished with it.
    printf("A failed output copy:\n");
    {
        ComPtr<ID3D12Fence> producer;
        dx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&producer));
        const uint64_t copied = fence.Next();
        dx.queue->Signal(fence.Fence12(), copied); // the copy in is done
        const uint64_t done = fence.Next();
        native::HandBackPicture(dx.queue.Get(), fence.Fence12(), copied, producer.Get(), 1, done);

        // The producer has not finished: nothing may be signalled yet
        Sleep(100);
        uint64_t reached = 0;
        vkGetSemaphoreCounterValue(vk.device, fence.Semaphore(), &reached);
        ok &= Check("the picture is not handed back while the producer still reads it", reached < done);

        // The producer finishes: the hand back goes through, in order
        producer->Signal(1);
        dx.WaitIdle();
        vkGetSemaphoreCounterValue(vk.device, fence.Semaphore(), &reached);
        ok &= Check("the picture is handed back once the producer is done", reached >= done);

        // Without a producer (null fence) it is only the copy in that is waited for
        const uint64_t copied2 = fence.Next();
        dx.queue->Signal(fence.Fence12(), copied2);
        const uint64_t done2 = fence.Next();
        native::HandBackPicture(dx.queue.Get(), fence.Fence12(), copied2, nullptr, 0, done2);
        dx.WaitIdle();
        vkGetSemaphoreCounterValue(vk.device, fence.Semaphore(), &reached);
        ok &= Check("with no producer the hand back follows the copy in", reached >= done2 && done2 > done);

        // A normal frame afterwards
        ok &= RoundTrip(vk, dx, interop, fence, { "B8G8R8A8_UNORM", VK_FORMAT_B8G8R8A8_UNORM, 4 });
        ok &= Check("no D3D12 debug layer error", dx.DebugErrors() == 0);
    }

    fence.Reset();
    vkDestroyCommandPool(vk.device, vk.pool, nullptr);
    vkDestroyDevice(vk.device, nullptr);
    vkDestroyInstance(vk.instance, nullptr);

    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
