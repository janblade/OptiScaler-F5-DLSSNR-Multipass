// Not built with the precompiled header: Vulkan and Direct3D only, so tests/nr_shared_frame_vk_gpu.cpp compiles it alone.
#include "SharedFrameVk.h"

#include <format>

namespace native
{

bool VkInterop::Load(VkPhysicalDevice physicalDevice, VkDevice vkDevice, std::string& error)
{
    device = vkDevice;
    physical = physicalDevice;
    getMemoryWin32HandleProperties = (PFN_vkGetMemoryWin32HandlePropertiesKHR) vkGetDeviceProcAddr(
        vkDevice, "vkGetMemoryWin32HandlePropertiesKHR");
    importSemaphoreWin32Handle =
        (PFN_vkImportSemaphoreWin32HandleKHR) vkGetDeviceProcAddr(vkDevice, "vkImportSemaphoreWin32HandleKHR");
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memory);

    if (getMemoryWin32HandleProperties == nullptr || importSemaphoreWin32Handle == nullptr)
    {
        error = "the Vulkan device was made without VK_KHR_external_memory_win32 and VK_KHR_external_semaphore_win32";
        device = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

bool VkPhysicalDeviceLuid(VkPhysicalDevice physicalDevice, LUID& luid)
{
    VkPhysicalDeviceIDProperties id {};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties {};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &id;
    vkGetPhysicalDeviceProperties2(physicalDevice, &properties);

    if (!id.deviceLUIDValid)
        return false;

    static_assert(sizeof(LUID) == VK_LUID_SIZE);
    memcpy(&luid, id.deviceLUID, sizeof(LUID));
    return true;
}

bool VkSharedPictureFormat(VkFormat swapchainFormat, DXGI_FORMAT* dxgi, VkFormat* shared)
{
    switch (swapchainFormat)
    {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        *dxgi = DXGI_FORMAT_B8G8R8A8_UNORM;
        *shared = VK_FORMAT_B8G8R8A8_UNORM;
        return true;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        *dxgi = DXGI_FORMAT_R8G8B8A8_UNORM;
        *shared = VK_FORMAT_R8G8B8A8_UNORM;
        return true;
    // Vulkan names the packed 10-bit format from the most significant bit down: A2B10G10R10 is DXGI's R10G10B10A2.
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        *dxgi = DXGI_FORMAT_R10G10B10A2_UNORM;
        *shared = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        return true;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        *dxgi = DXGI_FORMAT_R16G16B16A16_FLOAT;
        *shared = VK_FORMAT_R16G16B16A16_SFLOAT;
        return true;
    default:
        return false;
    }
}

namespace
{

uint32_t FindMemoryType(const VkPhysicalDeviceMemoryProperties& memory, uint32_t allowed)
{
    // Device-local first; an imported D3D12 resource is in video memory, but a driver may list it otherwise.
    for (int pass = 0; pass < 2; ++pass)
    {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        {
            if ((allowed & (1u << i)) == 0)
                continue;

            if (pass == 0 && (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0)
                continue;

            return i;
        }
    }

    return UINT32_MAX;
}

} // namespace

bool SharedImageVk::Create(ID3D12Device* device12, const VkInterop& vk, uint32_t width, uint32_t height,
                           DXGI_FORMAT dxgiFormat, VkFormat vkFormat, bool uav)
{
    Reset();

    if (device12 == nullptr || !vk.Ready())
    {
        _error = "no device to share on";
        return false;
    }

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = dxgiFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                 (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);

    HRESULT hr = device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                   nullptr, IID_PPV_ARGS(&_res12));

    if (FAILED(hr))
    {
        _error = std::format("CreateCommittedResource (shared, {}x{}, format {}) failed: 0x{:X}", width, height,
                             (int) dxgiFormat, (unsigned) hr);
        return false;
    }

    HANDLE handle = nullptr;
    hr = device12->CreateSharedHandle(_res12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);

    if (FAILED(hr))
    {
        _error = std::format("CreateSharedHandle failed: 0x{:X}", (unsigned) hr);
        Reset();
        return false;
    }

    VkExternalMemoryImageCreateInfo external {};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;

    VkImageCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.pNext = &external;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = vkFormat;
    info.extent = { width, height, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    _device = vk.device;
    VkResult result = vkCreateImage(vk.device, &info, nullptr, &_image);

    if (result != VK_SUCCESS)
    {
        _error = std::format("vkCreateImage for the shared picture failed: {}", (int) result);
        CloseHandle(handle);
        Reset();
        return false;
    }

    VkMemoryRequirements requirements {};
    vkGetImageMemoryRequirements(vk.device, _image, &requirements);

    VkMemoryWin32HandlePropertiesKHR handleProperties {};
    handleProperties.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;
    result = vk.getMemoryWin32HandleProperties(vk.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, handle,
                                               &handleProperties);

    const uint32_t type = result == VK_SUCCESS
                              ? FindMemoryType(vk.memory, requirements.memoryTypeBits & handleProperties.memoryTypeBits)
                              : UINT32_MAX;

    if (type == UINT32_MAX)
    {
        _error = std::format("no Vulkan memory type can hold the shared picture (handle properties {}, bits 0x{:X} and "
                             "0x{:X})",
                             (int) result, requirements.memoryTypeBits, handleProperties.memoryTypeBits);
        CloseHandle(handle);
        Reset();
        return false;
    }

    VkMemoryDedicatedAllocateInfo dedicated {};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = _image;

    VkImportMemoryWin32HandleInfoKHR import {};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    import.pNext = &dedicated;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    import.handle = handle;

    VkMemoryAllocateInfo allocate {};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.pNext = &import;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;

    result = vkAllocateMemory(vk.device, &allocate, nullptr, &_memory);
    // A D3D12 resource handle is not owned by the import: it is closed here either way.
    CloseHandle(handle);

    if (result != VK_SUCCESS)
    {
        _error = std::format("importing the shared picture's memory failed: {}", (int) result);
        Reset();
        return false;
    }

    result = vkBindImageMemory(vk.device, _image, _memory, 0);

    if (result != VK_SUCCESS)
    {
        _error = std::format("vkBindImageMemory for the shared picture failed: {}", (int) result);
        Reset();
        return false;
    }

    _width = width;
    _height = height;
    _vkFormat = vkFormat;
    _error.clear();
    return true;
}

void SharedImageVk::Reset()
{
    if (_image != VK_NULL_HANDLE)
        vkDestroyImage(_device, _image, nullptr);

    if (_memory != VK_NULL_HANDLE)
        vkFreeMemory(_device, _memory, nullptr);

    _image = VK_NULL_HANDLE;
    _memory = VK_NULL_HANDLE;
    _res12.Reset();
    _width = _height = 0;
    _vkFormat = VK_FORMAT_UNDEFINED;
}

bool SharedFenceVk::Create(ID3D12Device* device12, const VkInterop& vk)
{
    Reset();

    if (device12 == nullptr || !vk.Ready())
    {
        _error = "no device to share on";
        return false;
    }

    HRESULT hr = device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&_fence12));

    if (FAILED(hr))
    {
        _error = std::format("CreateFence (shared) failed: 0x{:X}", (unsigned) hr);
        return false;
    }

    HANDLE handle = nullptr;
    hr = device12->CreateSharedHandle(_fence12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);

    if (FAILED(hr))
    {
        _error = std::format("CreateSharedHandle for the fence failed: 0x{:X}", (unsigned) hr);
        Reset();
        return false;
    }

    VkSemaphoreTypeCreateInfo type {};
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;

    VkSemaphoreCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.pNext = &type;

    _device = vk.device;
    VkResult result = vkCreateSemaphore(vk.device, &info, nullptr, &_semaphore);

    if (result == VK_SUCCESS)
    {
        VkImportSemaphoreWin32HandleInfoKHR import {};
        import.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
        import.semaphore = _semaphore;
        import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        import.handle = handle;
        result = vk.importSemaphoreWin32Handle(vk.device, &import);
    }

    CloseHandle(handle);

    if (result != VK_SUCCESS)
    {
        _error = std::format("opening the D3D12 fence as a Vulkan timeline semaphore failed: {} (the device needs the "
                             "timeline semaphore feature)",
                             (int) result);
        Reset();
        return false;
    }

    _error.clear();
    return true;
}

void SharedFenceVk::Reset()
{
    if (_semaphore != VK_NULL_HANDLE)
        vkDestroySemaphore(_device, _semaphore, nullptr);

    _semaphore = VK_NULL_HANDLE;
    _fence12.Reset();
    _value = 0;
}

namespace
{

VkImageMemoryBarrier Barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                             VkAccessFlags dstAccess, uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED,
                             uint32_t dstFamily = VK_QUEUE_FAMILY_IGNORED)
{
    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = srcFamily;
    barrier.dstQueueFamilyIndex = dstFamily;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    return barrier;
}

VkImageCopy WholeImage(uint32_t width, uint32_t height)
{
    VkImageCopy region {};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent = { width, height, 1 };
    return region;
}

} // namespace

void RecordCopyToShared(VkCommandBuffer cmd, VkImage source, VkImageLayout layout, const SharedImageVk& shared,
                        uint32_t family)
{
    // The game's image was last written by the game's work before the present's semaphores, which the submit waits on:
    // every earlier write is visible to the transfer that follows.
    VkImageMemoryBarrier before[2] = {
        Barrier(source, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT),
        Barrier(shared.Image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT),
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, before);

    const VkImageCopy region = WholeImage(shared.Width(), shared.Height());
    vkCmdCopyImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shared.Image(),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // The shared image goes to the D3D12 queue (an external queue family); the game's image back to its layout.
    VkImageMemoryBarrier after[2] = {
        Barrier(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout, 0, 0),
        Barrier(shared.Image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, family, VK_QUEUE_FAMILY_EXTERNAL),
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, after);
}

void RecordCopyFromShared(VkCommandBuffer cmd, const SharedImageVk& shared, VkImage target, VkImageLayout layout,
                          uint32_t family)
{
    // Taken back from the D3D12 queue (the submit waits on its signal), and the game's image made ready to be written.
    VkImageMemoryBarrier before[2] = {
        Barrier(shared.Image(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_EXTERNAL, family),
        Barrier(target, layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_MEMORY_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT),
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, before);

    const VkImageCopy region = WholeImage(shared.Width(), shared.Height());
    vkCmdCopyImage(cmd, shared.Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier after =
        Barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_MEMORY_READ_BIT);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &after);
}

} // namespace native
