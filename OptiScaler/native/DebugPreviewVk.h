#pragma once

// Diagnostic-only D3D12 -> Vulkan transport. A shared BUFFER avoids cross-API image
// layout aliases. The menu's normal per-image fence protects slot reuse; a timeline
// semaphore orders the D3D12 copy before the Vulkan upload. No CPU readback/queue idle
// occurs per frame. Old resources are released only after both APIs have finished.
#include "SharedFrameVk.h"
#include <array>
#include <algorithm>

namespace native
{
class DebugPreviewVk
{
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
  public:
    struct Image
    {
        SharedBufferVk shared;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12Resource> source;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet descriptor = VK_NULL_HANDLE; // registered/released by the renderer
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        uint32_t width = 0, height = 0, bytes = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint64_t copied = 0;
        bool initialized = false;
    };
    using RemoveDescriptorFn = void(*)(VkDescriptorSet);
  private:
    std::array<Image, 16> _images;
    std::array<Image*, 2> _used {};
    uint32_t _slot = UINT32_MAX, _count = 0, _family = 0;
    uint64_t _wait = 0;
    VkInterop _vk;
    VkSampler _sampler = VK_NULL_HANDLE;
    ComPtr<ID3D12Device> _device12;
    ComPtr<ID3D12CommandQueue> _queue;
    SharedFenceVk _fence;
    RemoveDescriptorFn _remove = nullptr;
    std::string _error;

    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags wanted)
    {
        for (uint32_t i = 0; i < _vk.memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (_vk.memory.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
        return UINT32_MAX;
    }
    void Release(Image& image)
    {
        if (image.descriptor && _remove) _remove(image.descriptor);
        image.descriptor = VK_NULL_HANDLE;
        if (image.view) vkDestroyImageView(_vk.device, image.view, nullptr);
        if (image.image) vkDestroyImage(_vk.device, image.image, nullptr);
        if (image.memory) vkFreeMemory(_vk.device, image.memory, nullptr);
        image.view = VK_NULL_HANDLE; image.image = VK_NULL_HANDLE; image.memory = VK_NULL_HANDLE;
        image.shared.Reset(); image.source.Reset(); image.initialized = false;
    }
    bool Allocate(Image& image, ID3D12Resource* source, DXGI_FORMAT format, bool gray)
    {
        auto desc = source->GetDesc();
        const auto width = (uint32_t)desc.Width, height = desc.Height;
        Release(image);
        _device12->GetCopyableFootprints(&desc, 0, 1, 0, &image.footprint, nullptr, nullptr, nullptr);
        const uint64_t bytes = uint64_t(image.footprint.Footprint.RowPitch) * height;
        if (!image.shared.Create(_device12.Get(), _vk, bytes))
        { _error = image.shared.Error(); return false; }
        VkImageCreateInfo info {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format == DXGI_FORMAT_R8_UNORM ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {width, height, 1}; info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(_vk.device, &info, nullptr, &image.image) != VK_SUCCESS) return false;
        VkMemoryRequirements req {}; vkGetImageMemoryRequirements(_vk.device, image.image, &req);
        const auto type = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == UINT32_MAX) return false;
        VkMemoryAllocateInfo mem {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mem.allocationSize = req.size; mem.memoryTypeIndex = type;
        if (vkAllocateMemory(_vk.device, &mem, nullptr, &image.memory) != VK_SUCCESS ||
            vkBindImageMemory(_vk.device, image.image, image.memory, 0) != VK_SUCCESS) return false;
        VkImageViewCreateInfo view {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = info.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (gray) view.components = {VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_ONE};
        if (vkCreateImageView(_vk.device, &view, nullptr, &image.view) != VK_SUCCESS) return false;
        image.width = width; image.height = height; image.format = format;
        image.bytes = format == DXGI_FORMAT_R8_UNORM ? 1 : 4;
        return true;
    }
  public:
    void SkipFrame(){_count=0;_wait=0;_slot=UINT32_MAX;}
    bool BeginFrame(VkDevice device, VkPhysicalDevice physical, uint32_t family, uint32_t slot,
                    RemoveDescriptorFn remove = nullptr)
    {
        _count = 0; _wait = 0; _slot = slot; _family = family; _remove = remove;
        if (slot >= 8) return false;
        if (!_vk.Ready())
        {
            if (!_vk.Load(physical, device, _error)) return false;
        }
        if(!_sampler)
        {
            VkSamplerCreateInfo info {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            info.magFilter = info.minFilter = VK_FILTER_LINEAR;
            info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            if (vkCreateSampler(device, &info, nullptr, &_sampler) != VK_SUCCESS) return false;
        }
        if(_vk.device != device){SkipFrame();return false;}
        return true;
    }
    Image* Export(ID3D12CommandQueue* queue, ID3D12Resource* source, DXGI_FORMAT format,
                  D3D12_RESOURCE_STATES resting, bool gray)
    {
        if (!_vk.Ready() || !queue || !source || _slot >= 8 ||
            (format != DXGI_FORMAT_R8_UNORM && format != DXGI_FORMAT_R8G8B8A8_UNORM)) return nullptr;
        for (uint32_t i = 0; i < _count; ++i) if (_used[i]->source.Get() == source) return _used[i];
        if (_count == _used.size()) return nullptr;
        if (!_device12)
        {
            _queue = queue;
            if (FAILED(queue->GetDevice(IID_PPV_ARGS(&_device12))) || !_fence.Create(_device12.Get(), _vk))
            { _error = "Could not share the diagnostic fence: " + _fence.Error(); return nullptr; }
        }
        if (_queue.Get() != queue || !_fence.Ready()) return nullptr;
        auto& image = _images[_slot * 2 + _count];
        const auto complete = _fence.Fence12()->GetCompletedValue();
        if (complete == UINT64_MAX || complete < image.copied) return nullptr;
        auto desc = source->GetDesc();
        if (!image.view || image.width != desc.Width || image.height != desc.Height || image.format != format)
            if (!Allocate(image, source, format, gray)) return nullptr;
        if (!image.commands)
        {
            if (FAILED(_device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&image.allocator))) ||
                FAILED(_device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, image.allocator.Get(), nullptr,
                                                   IID_PPV_ARGS(&image.commands))) || FAILED(image.commands->Close())) return nullptr;
        }
        if (FAILED(image.allocator->Reset()) || FAILED(image.commands->Reset(image.allocator.Get(), nullptr))) return nullptr;
        image.source = source;
        D3D12_RESOURCE_BARRIER barriers[2] {};
        for (auto& b : barriers) { b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; }
        barriers[0].Transition = {source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, resting, D3D12_RESOURCE_STATE_COPY_SOURCE};
        barriers[1].Transition = {image.shared.Res12(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
        image.commands->ResourceBarrier(2, barriers);
        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.pResource = image.shared.Res12(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = image.footprint;
        src.pResource = source; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        image.commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        for (auto& b : barriers) std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        image.commands->ResourceBarrier(2, barriers);
        if (FAILED(image.commands->Close())) return nullptr;
        ID3D12CommandList* commands[] = {image.commands.Get()}; queue->ExecuteCommandLists(1, commands);
        image.copied = _fence.Next();
        if (FAILED(queue->Signal(_fence.Fence12(), image.copied))) return nullptr;
        _wait = image.copied; _used[_count++] = &image; _error.clear();
        return &image;
    }
    // Record BEFORE the render pass, on the SAME queue family as BeginFrame. The
    // submit must wait on WaitSemaphore/WaitValue at TRANSFER before reading buffers.
    void RecordTransfers(VkCommandBuffer cmd)
    {
        for (uint32_t i = 0; i < _count; ++i)
        {
            auto& image = *_used[i];
            VkBufferMemoryBarrier buffer {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            buffer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            buffer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL; buffer.dstQueueFamilyIndex = _family;
            buffer.buffer = image.shared.Buffer(); buffer.size = VK_WHOLE_SIZE;
            VkImageMemoryBarrier barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask = image.initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = image.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image.image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&buffer,1,&barrier);
            VkBufferImageCopy copy {};
            copy.bufferRowLength = image.footprint.Footprint.RowPitch / image.bytes;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent = {image.width,image.height,1};
            vkCmdCopyBufferToImage(cmd,image.shared.Buffer(),image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            buffer.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; buffer.dstAccessMask = 0;
            std::swap(buffer.srcQueueFamilyIndex,buffer.dstQueueFamilyIndex);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,1,&buffer,1,&barrier);
            image.initialized = true;
        }
    }
    VkSemaphore WaitSemaphore() const { return _wait ? _fence.Semaphore() : VK_NULL_HANDLE; }
    uint64_t WaitValue() const { return _wait; }
    VkSampler Sampler() const { return _sampler; }
    ID3D12Fence* CopyFence() const { return _fence.Fence12(); }
    const std::string& Error() const { return _error; }
    // Only on swapchain/backend retirement. Normal frame reuse is protected by the
    // menu's per-swapchain-image fence, which is waited BEFORE BeginFrame.
    void Reset()
    {
        if (!_vk.device) return;
        vkDeviceWaitIdle(_vk.device);
        if (_fence.Ready() && _fence.Fence12()->GetCompletedValue() < _fence.Value())
        {
            HANDLE event = CreateEvent(nullptr,FALSE,FALSE,nullptr);
            if (!event) return;
            const HRESULT hr = _fence.Fence12()->SetEventOnCompletion(_fence.Value(),event);
            const bool done = SUCCEEDED(hr) && WaitForSingleObject(event,2000)==WAIT_OBJECT_0;
            CloseHandle(event);
            if (!done) { _error="Waiting to retire diagnostic copies."; return; }
        }
        for (auto& image : _images) { Release(image); image.commands.Reset(); image.allocator.Reset(); image.copied=0; }
        if (_sampler) vkDestroySampler(_vk.device,_sampler,nullptr);
        _sampler=VK_NULL_HANDLE; _fence.Reset(); _queue.Reset(); _device12.Reset(); _vk={}; _count=0; _wait=0;
    }
};
}
