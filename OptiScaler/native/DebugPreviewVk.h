#pragma once

// The Vulkan menu's copy of the Optical F5Low debug view (native/FlowDiagnosticUi.h). The producer draws it on its own
// D3D12 device; the Vulkan menu (ImGui_ImplVulkan) samples a VkImage. Per menu frame that shows it: the picture is
// copied on the producer's queue into a shared buffer (no cross-API image layouts), which signals a shared timeline
// semaphore; the menu's command buffer copies the buffer into its own image before its render pass, and its submit
// waits for the semaphore at the transfer stage. One slot per swap chain image: the menu waits for that image's last
// submit (its fence) before it hands the slot out again. No CPU readback. From PR #232 (TechvibeCoder07).

#include "SharedFrameVk.h"

#include <d3d12.h>
#include <vulkan/vulkan.h>
#include <wrl/client.h>

#include <array>
#include <string>

namespace native
{

class DebugPreviewVk
{
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

  public:
    using AddTextureFn = VkDescriptorSet (*)(VkSampler, VkImageView, VkImageLayout);
    using RemoveTextureFn = void (*)(VkDescriptorSet);

    // Once per menu frame, before the menu is drawn, for swap chain image `slot` (whose last submit is done). False
    // when the copy cannot be used this frame.
    bool BeginFrame(VkDevice device, VkPhysicalDevice physical, uint32_t family, uint32_t slot, AddTextureFn add,
                    RemoveTextureFn remove)
    {
        _current = nullptr;
        _wait = 0;

        if (_vk.device != VK_NULL_HANDLE && _vk.device != device)
            Reset();

        if (!_vk.Ready() && !_vk.Load(physical, device, _error))
            return false;

        if (_sampler == VK_NULL_HANDLE)
        {
            VkSamplerCreateInfo info { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            info.magFilter = info.minFilter = VK_FILTER_LINEAR;
            info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

            if (vkCreateSampler(device, &info, nullptr, &_sampler) != VK_SUCCESS)
                return false;
        }

        if (slot >= _slots.size())
            return false;

        _slot = slot;
        _family = family;
        _add = add;
        _remove = remove;
        _began = true;
        return true;
    }

    // An ImGui texture of `source` (the producer's preview, R8G8B8A8_UNORM, in `resting` between frames) for this menu
    // frame, or 0. The copy itself is recorded by RecordTransfers.
    VkDescriptorSet Export(ID3D12CommandQueue* queue, ID3D12Resource* source, D3D12_RESOURCE_STATES resting)
    {
        if (!_began || queue == nullptr || source == nullptr)
            return VK_NULL_HANDLE;

        Slot& slot = _slots[_slot];

        if (_current == &slot && slot.source.Get() == source)
            return slot.descriptor;

        if (_device12 == nullptr)
        {
            if (FAILED(queue->GetDevice(IID_PPV_ARGS(&_device12))) || !_fence.Create(_device12.Get(), _vk))
            {
                _error = "The debug view cannot be shared with this Vulkan device: " + _fence.Error();
                _device12.Reset();
                return VK_NULL_HANDLE;
            }

            _queue = queue;
        }

        if (_queue.Get() != queue)
        {
            _error = "The motion estimate's device changed; the debug view shows again after a restart.";
            return VK_NULL_HANDLE;
        }

        const D3D12_RESOURCE_DESC desc = source->GetDesc();

        if (slot.view == VK_NULL_HANDLE || slot.width != desc.Width || slot.height != desc.Height)
        {
            if (!Allocate(slot, desc))
            {
                Release(slot);
                return VK_NULL_HANDLE;
            }
        }

        if (slot.list == nullptr &&
            (FAILED(_device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) ||
             FAILED(_device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                                 IID_PPV_ARGS(&slot.list))) ||
             FAILED(slot.list->Close())))
        {
            _error = "Could not make the debug view's copy commands.";
            slot.list.Reset();
            slot.allocator.Reset();
            return VK_NULL_HANDLE;
        }

        // The slot's last D3D12 copy finished before its Vulkan read, which the menu waited for (BeginFrame).
        const uint64_t completed = _fence.Fence12()->GetCompletedValue();

        if (completed == UINT64_MAX || completed < slot.copied || FAILED(slot.allocator->Reset()) ||
            FAILED(slot.list->Reset(slot.allocator.Get(), nullptr)))
            return VK_NULL_HANDLE;

        D3D12_RESOURCE_BARRIER barriers[2] {};

        for (auto& barrier : barriers)
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;

        barriers[0].Transition = { source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, resting,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE };
        barriers[1].Transition = { slot.shared.Res12(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                   D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST };
        slot.list->ResourceBarrier(2, barriers);

        D3D12_TEXTURE_COPY_LOCATION to {};
        to.pResource = slot.shared.Res12();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = slot.footprint;
        D3D12_TEXTURE_COPY_LOCATION from {};
        from.pResource = source;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        slot.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);

        for (auto& barrier : barriers)
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);

        slot.list->ResourceBarrier(2, barriers);

        if (FAILED(slot.list->Close()))
            return VK_NULL_HANDLE;

        ID3D12CommandList* lists[] = { slot.list.Get() };
        queue->ExecuteCommandLists(1, lists);
        const uint64_t copied = _fence.Next();

        if (FAILED(queue->Signal(_fence.Fence12(), copied)))
        {
            slot.copied = UINT64_MAX; // the copy's end is unknown: never reused
            return VK_NULL_HANDLE;
        }

        if (slot.descriptor == VK_NULL_HANDLE && _add != nullptr)
            slot.descriptor = _add(_sampler, slot.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        slot.copied = copied;
        slot.source = source;
        _current = &slot;
        _wait = copied;
        _error.clear();
        return slot.descriptor;
    }

    // Into the menu's command buffer, after it began and before its render pass: this frame's buffer into its image.
    void RecordTransfers(VkCommandBuffer commands)
    {
        if (_current == nullptr)
            return;

        Slot& slot = *_current;
        VkBufferMemoryBarrier buffer { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        buffer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        buffer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        buffer.dstQueueFamilyIndex = _family;
        buffer.buffer = slot.shared.Buffer();
        buffer.size = VK_WHOLE_SIZE;

        VkImageMemoryBarrier image { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        image.srcAccessMask = slot.initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
        image.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        image.oldLayout = slot.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        image.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        image.srcQueueFamilyIndex = image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image.image = slot.image;
        image.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 1, &buffer, 1, &image);

        VkBufferImageCopy copy {};
        copy.bufferRowLength = slot.footprint.Footprint.RowPitch / 4;
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { slot.width, slot.height, 1 };
        vkCmdCopyBufferToImage(commands, slot.shared.Buffer(), slot.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &copy);

        buffer.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        buffer.dstAccessMask = 0;
        std::swap(buffer.srcQueueFamilyIndex, buffer.dstQueueFamilyIndex);
        image.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        image.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        image.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        image.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 1, &buffer, 1, &image);
        _recorded = true; // the image is in SHADER_READ_ONLY once this command buffer was submitted (EndFrame)
    }

    // What the menu's submit waits on at VK_PIPELINE_STAGE_TRANSFER_BIT this frame (VK_NULL_HANDLE: nothing).
    VkSemaphore WaitSemaphore() const { return _wait != 0 ? _fence.Semaphore() : VK_NULL_HANDLE; }
    uint64_t WaitValue() const { return _wait; }

    // After the menu's submit (`submitted`: it went to the queue), or when it drew nothing or gave up.
    void EndFrame(bool submitted = false)
    {
        if (submitted && _recorded && _current != nullptr)
            _current->initialized = true;

        _recorded = false;
        _began = false;
        _current = nullptr;
        _wait = 0;
    }

    const std::string& Error() const { return _error; }

    // When the menu's swap chain or device goes away. Waits for the device and the last copy (up to 2 s), then lets
    // every object go, the descriptor sets through the backend while it is up (`backendUp`; else they go with its
    // pool).
    void Reset(bool backendUp = true)
    {
        if (!backendUp)
            _remove = nullptr;

        if (_vk.device == VK_NULL_HANDLE)
            return;

        vkDeviceWaitIdle(_vk.device);

        if (_fence.Ready() && _fence.Fence12()->GetCompletedValue() < _fence.Value())
        {
            if (HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr))
            {
                if (SUCCEEDED(_fence.Fence12()->SetEventOnCompletion(_fence.Value(), event)))
                    WaitForSingleObject(event, 2000);

                CloseHandle(event);
            }
        }

        for (Slot& slot : _slots)
        {
            Release(slot);
            slot.list.Reset();
            slot.allocator.Reset();
            slot.copied = 0;
        }

        if (_sampler != VK_NULL_HANDLE)
            vkDestroySampler(_vk.device, _sampler, nullptr);

        _sampler = VK_NULL_HANDLE;
        _fence.Reset();
        _queue.Reset();
        _device12.Reset();
        _vk = {};
        _current = nullptr;
        _wait = 0;
        _began = false;
    }

  private:
    struct Slot
    {
        SharedBufferVk shared;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Resource> source;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet descriptor = VK_NULL_HANDLE; // the backend's (AddTextureFn / RemoveTextureFn)
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        uint32_t width = 0, height = 0;
        uint64_t copied = 0; // the fence value its last copy signalled
        bool initialized = false;
    };

    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags wanted) const
    {
        for (uint32_t i = 0; i < _vk.memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) != 0 && (_vk.memory.memoryTypes[i].propertyFlags & wanted) == wanted)
                return i;

        return UINT32_MAX;
    }

    void Release(Slot& slot)
    {
        if (slot.descriptor != VK_NULL_HANDLE && _remove != nullptr)
            _remove(slot.descriptor);

        if (slot.view != VK_NULL_HANDLE)
            vkDestroyImageView(_vk.device, slot.view, nullptr);

        if (slot.image != VK_NULL_HANDLE)
            vkDestroyImage(_vk.device, slot.image, nullptr);

        if (slot.memory != VK_NULL_HANDLE)
            vkFreeMemory(_vk.device, slot.memory, nullptr);

        slot.descriptor = VK_NULL_HANDLE;
        slot.view = VK_NULL_HANDLE;
        slot.image = VK_NULL_HANDLE;
        slot.memory = VK_NULL_HANDLE;
        slot.shared.Reset();
        slot.source.Reset();
        slot.width = slot.height = 0;
        slot.initialized = false;
    }

    bool Allocate(Slot& slot, const D3D12_RESOURCE_DESC& desc)
    {
        Release(slot);
        const uint32_t width = (uint32_t) desc.Width;
        const uint32_t height = desc.Height;
        _device12->GetCopyableFootprints(&desc, 0, 1, 0, &slot.footprint, nullptr, nullptr, nullptr);

        if (!slot.shared.Create(_device12.Get(), _vk, uint64_t(slot.footprint.Footprint.RowPitch) * height))
        {
            _error = "Could not make the debug view's shared buffer: " + slot.shared.Error();
            return false;
        }

        VkImageCreateInfo info { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = { width, height, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(_vk.device, &info, nullptr, &slot.image) != VK_SUCCESS)
            return false;

        VkMemoryRequirements needs {};
        vkGetImageMemoryRequirements(_vk.device, slot.image, &needs);
        const uint32_t type = MemoryType(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo memory { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        memory.allocationSize = needs.size;
        memory.memoryTypeIndex = type;

        if (type == UINT32_MAX || vkAllocateMemory(_vk.device, &memory, nullptr, &slot.memory) != VK_SUCCESS ||
            vkBindImageMemory(_vk.device, slot.image, slot.memory, 0) != VK_SUCCESS)
            return false;

        VkImageViewCreateInfo view { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = slot.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = info.format;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        if (vkCreateImageView(_vk.device, &view, nullptr, &slot.view) != VK_SUCCESS)
            return false;

        slot.width = width;
        slot.height = height;
        return true;
    }

    std::array<Slot, 16> _slots; // one per swap chain image (the menu's own limit)
    Slot* _current = nullptr;    // this menu frame's copy
    uint32_t _slot = 0, _family = 0;
    uint64_t _wait = 0;
    bool _began = false;
    bool _recorded = false; // RecordTransfers ran into this frame's command buffer
    VkInterop _vk;
    VkSampler _sampler = VK_NULL_HANDLE;
    ComPtr<ID3D12Device> _device12;
    ComPtr<ID3D12CommandQueue> _queue;
    SharedFenceVk _fence;
    AddTextureFn _add = nullptr;
    RemoveTextureFn _remove = nullptr;
    std::string _error;
};

} // namespace native
