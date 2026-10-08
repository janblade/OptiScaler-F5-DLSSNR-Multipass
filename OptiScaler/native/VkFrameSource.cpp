#include "pch.h"

#include "VkFrameSource.h"
#include "DepthFinderCore.h"

#include <Config.h>
#include <proxies/D3D12_Proxy.h>
#include <proxies/DXGI_Proxy.h>

#include <format>
#include <mutex>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace native
{

namespace
{

// What the hooks saw being made. Vulkan cannot be asked afterwards which family a queue is of, or what a swapchain's
// images are, without the game's own calls.
struct SwapchainRecord
{
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent {};
    VkImageUsageFlags usage = 0;
    std::vector<VkImage> images;
    std::vector<VkSemaphore> presentWaits; // one per image: what its present waits on in place of the game's list
};

// A semaphore of a replaced swapchain: a present may still wait on it, so it goes a few presents later.
struct Retired
{
    VkDevice device = VK_NULL_HANDLE;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    uint64_t after = 0; // the Vulkan present count after which it is destroyed
};

constexpr uint64_t kRetireAfterPresents = 8;

std::mutex g_registryMutex;
struct QueueRecord
{
    VkDevice device = VK_NULL_HANDLE;
    uint32_t family = 0;
};

std::unordered_map<VkQueue, QueueRecord> g_queues;
std::unordered_map<VkSwapchainKHR, SwapchainRecord> g_swapchains;
std::vector<Retired> g_retired;

uint64_t PresentCount() { return State::Instance().vulkanPresentCount.load(std::memory_order_relaxed); }

void RetireLocked(SwapchainRecord& record)
{
    for (auto semaphore : record.presentWaits)
    {
        if (semaphore != VK_NULL_HANDLE)
            g_retired.push_back({ record.device, semaphore, PresentCount() + kRetireAfterPresents });
    }

    record.presentWaits.clear();
}

void DestroyRetiredLocked(bool all, VkDevice onlyDevice = VK_NULL_HANDLE)
{
    const uint64_t now = PresentCount();

    std::erase_if(g_retired,
                  [&](const Retired& r)
                  {
                      if (onlyDevice != VK_NULL_HANDLE && r.device != onlyDevice)
                          return false;

                      if (!all && now < r.after)
                          return false;

                      vkDestroySemaphore(r.device, r.semaphore, nullptr);
                      return true;
                  });
}

ColorSpace ToColorSpace(VkColorSpaceKHR space, VkFormat format)
{
    if (space == VK_COLOR_SPACE_HDR10_ST2084_EXT)
        return ColorSpace::Pq;

    // A float swapchain in SRGB_NONLINEAR holds sRGB-encoded values (Vulkan, unlike DXGI, does not make it scRGB).
    (void) format;

    if (space == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT)
        return ColorSpace::ScRgb;

    return ColorSpace::Srgb;
}

} // namespace

void VkFrameSource::NoteDevice(VkDevice device, const VkDeviceCreateInfo& info)
{
    std::lock_guard lock(g_registryMutex);

    for (uint32_t i = 0; i < info.queueCreateInfoCount; ++i)
    {
        const auto& queues = info.pQueueCreateInfos[i];

        // Queues made with flags (protected ones) are only reachable through vkGetDeviceQueue2; the game does not
        // present on those.
        if (queues.flags != 0)
            continue;

        for (uint32_t index = 0; index < queues.queueCount; ++index)
        {
            VkQueue queue = VK_NULL_HANDLE;
            vkGetDeviceQueue(device, queues.queueFamilyIndex, index, &queue);

            if (queue != VK_NULL_HANDLE)
                g_queues[queue] = { device, queues.queueFamilyIndex };
        }
    }
}

VkImageUsageFlags VkFrameSource::SwapchainUsage(VkPhysicalDevice physical, const VkSwapchainCreateInfoKHR& info)
{
    constexpr VkImageUsageFlags kCopies = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if (!Config::Instance()->DlssNrEnabled.value_or_default() || physical == VK_NULL_HANDLE ||
        (info.imageUsage & kCopies) == kCopies)
        return info.imageUsage;

    VkSurfaceCapabilitiesKHR caps {};

    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, info.surface, &caps) != VK_SUCCESS ||
        (caps.supportedUsageFlags & kCopies) != kCopies)
    {
        LOG_INFO("Native motion (Vulkan): the surface does not allow copies of the swapchain's images (supported usage "
                 "0x{:X}); Optical F5Low cannot run on it",
                 (unsigned) caps.supportedUsageFlags);
        return info.imageUsage;
    }

    return info.imageUsage | kCopies;
}

void VkFrameSource::NoteSwapchain(VkDevice device, VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& info)
{
    SwapchainRecord record;
    record.device = device;
    record.format = info.imageFormat;
    record.space = info.imageColorSpace;
    record.extent = info.imageExtent;
    record.usage = info.imageUsage;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    record.images.resize(count);
    vkGetSwapchainImagesKHR(device, swapchain, &count, record.images.data());
    record.images.resize(count);
    record.presentWaits.resize(count, VK_NULL_HANDLE);

    std::lock_guard lock(g_registryMutex);

    if (info.oldSwapchain != VK_NULL_HANDLE)
    {
        if (auto it = g_swapchains.find(info.oldSwapchain); it != g_swapchains.end())
        {
            RetireLocked(it->second);
            g_swapchains.erase(it);
        }
    }

    // A handle the driver hands out again replaces whatever was recorded under it.
    if (auto it = g_swapchains.find(swapchain); it != g_swapchains.end())
        RetireLocked(it->second);

    g_swapchains[swapchain] = std::move(record);
}

void VkFrameSource::SetPresent(VkDevice device, VkPhysicalDevice physical, VkQueue queue, VkSwapchainKHR swapchain,
                               uint32_t imageIndex, const VkSemaphore* waits, uint32_t waitCount)
{
    _device = device;
    _physical = physical;
    _queue = queue;
    _swapchain = swapchain;
    _imageIndex = imageIndex;
    _waits = waits;
    _waitCount = waitCount;
}

bool VkFrameSource::EnsureDevices(std::string& why)
{
    if (_interop.device != _device)
    {
        Release();

        if (!_interop.Load(_physical, _device, why))
            return false;
    }

    if (_device12 == nullptr)
    {
        LUID luid {};

        if (!VkPhysicalDeviceLuid(_physical, luid))
        {
            why = "the Vulkan driver does not say which adapter the device is on";
            return false;
        }

        ScopedSkipSpoofingGlobal skipSpoofing {};
        ScopedSkipVulkanHooks skipVulkanHooks {};

        ComPtr<IDXGIFactory2> factory2;
        HRESULT hr = DxgiProxy::Module() == nullptr
                         ? CreateDXGIFactory2(0, IID_PPV_ARGS(&factory2))
                         : DxgiProxy::CreateDxgiFactory2_()(0, __uuidof(IDXGIFactory2), &factory2);

        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;

        if (SUCCEEDED(hr))
            hr = factory2.As(&factory);

        if (SUCCEEDED(hr))
            hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));

        if (SUCCEEDED(hr))
        {
            hr = D3d12Proxy::Module() == nullptr
                     ? D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&_device12))
                     : D3d12Proxy::D3D12CreateDevice_()(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&_device12));
        }

        if (FAILED(hr) || _device12 == nullptr)
        {
            why = std::format("could not make a D3D12 device on the Vulkan device's adapter (0x{:X})", (unsigned) hr);
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC desc {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = _device12->CreateCommandQueue(&desc, IID_PPV_ARGS(&_queue12));

        if (FAILED(hr))
        {
            why = std::format("could not make the D3D12 queue (0x{:X})", (unsigned) hr);
            _device12->Release();
            _device12 = nullptr;
            return false;
        }

        LOG_INFO("Native motion (Vulkan): D3D12 device made on the Vulkan device's adapter");
    }

    if (!_fence.Ready() && !_fence.Create(_device12, _interop))
    {
        why = _fence.Error();
        return false;
    }

    return true;
}

bool VkFrameSource::EnsureRing(uint32_t family)
{
    if (_ringFamily == family)
        return true;

    ReleaseRing();

    for (auto& slot : _ring)
    {
        VkCommandPoolCreateInfo poolInfo {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = family;

        VkCommandBuffer buffers[2] {};
        VkCommandBufferAllocateInfo allocate {};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 2;

        VkFenceCreateInfo fenceInfo {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

        if (vkCreateCommandPool(_device, &poolInfo, nullptr, &slot.pool) != VK_SUCCESS ||
            (allocate.commandPool = slot.pool,
             vkAllocateCommandBuffers(_device, &allocate, buffers) != VK_SUCCESS) ||
            vkCreateFence(_device, &fenceInfo, nullptr, &slot.done) != VK_SUCCESS)
        {
            _error = "could not make the Vulkan command buffers";
            ReleaseRing();
            return false;
        }

        slot.copyIn = buffers[0];
        slot.copyOut = buffers[1];
    }

    _ringFamily = family;
    return true;
}

void VkFrameSource::WaitRingIdle()
{
    for (auto& slot : _ring)
    {
        if (slot.submitted)
        {
            vkWaitForFences(_device, 1, &slot.done, VK_TRUE, UINT64_MAX);
            slot.submitted = false;
        }
    }

    // And the D3D12 side, which may still read the shared picture.
    if (_queue12 != nullptr && _fence.Ready())
    {
        const uint64_t value = _fence.Next();
        _queue12->Signal(_fence.Fence12(), value);

        if (_fence.Fence12()->GetCompletedValue() < value)
        {
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            _fence.Fence12()->SetEventOnCompletion(value, event);
            WaitForSingleObject(event, 5000);
            CloseHandle(event);
        }
    }
}

void VkFrameSource::ReleaseRing()
{
    WaitRingIdle();

    for (auto& slot : _ring)
    {
        if (slot.done != VK_NULL_HANDLE)
            vkDestroyFence(_device, slot.done, nullptr);

        if (slot.pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(_device, slot.pool, nullptr);

        slot = Slot {};
    }

    _ringFamily = UINT32_MAX;
}

void VkFrameSource::Release()
{
    if (_interop.device != VK_NULL_HANDLE)
    {
        // The ring and the shared objects belong to the device they were made on, which may not be _device any more.
        const VkDevice current = _device;
        _device = _interop.device;
        ReleaseRing();
        _picture.Reset();
        _fence.Reset();
        _device = current;
    }

    if (_queue12 != nullptr)
        _queue12->Release();

    if (_device12 != nullptr)
        _device12->Release();

    _queue12 = nullptr;
    _device12 = nullptr;
    _interop = VkInterop {};
    _acquired = false;
}

void VkFrameSource::OnDeviceDestroyed(VkDevice device)
{
    if (_interop.device == device)
    {
        Release();
        _device = VK_NULL_HANDLE;
    }

    std::lock_guard lock(g_registryMutex);

    for (auto it = g_swapchains.begin(); it != g_swapchains.end();)
    {
        if (it->second.device == device)
        {
            RetireLocked(it->second);
            it = g_swapchains.erase(it);
        }
        else
            ++it;
    }

    DestroyRetiredLocked(true, device);
    std::erase_if(g_queues, [&](const auto& entry) { return entry.second.device == device; });
}

AcquireStatus VkFrameSource::Acquire(FrameInput& input)
{
    _acquired = false;
    _tookPresentWaits = false;
    _presentWait = VK_NULL_HANDLE;

    if (_device == VK_NULL_HANDLE || _queue == VK_NULL_HANDLE || _swapchain == VK_NULL_HANDLE)
        return AcquireStatus::Unavailable;

    if (GameUpscalerCalledRecently())
        return AcquireStatus::WaitingForUpscaler;

    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent {};
    VkImageUsageFlags usage = 0;
    uint32_t family = UINT32_MAX;

    {
        std::lock_guard lock(g_registryMutex);
        DestroyRetiredLocked(false);

        if (auto it = g_swapchains.find(_swapchain); it != g_swapchains.end() && _imageIndex < it->second.images.size())
        {
            image = it->second.images[_imageIndex];
            format = it->second.format;
            space = it->second.space;
            extent = it->second.extent;
            usage = it->second.usage;
        }

        if (auto it = g_queues.find(_queue); it != g_queues.end())
            family = it->second.family;
    }

    if (image == VK_NULL_HANDLE)
    {
        _error = "the swapchain was made before OptiScaler could see it";
        return AcquireStatus::Unavailable;
    }

    constexpr VkImageUsageFlags kCopies = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if ((usage & kCopies) != kCopies)
    {
        _error = "the swapchain's pictures cannot be copied (the surface does not allow it)";
        return AcquireStatus::Unavailable;
    }

    DXGI_FORMAT dxgiFormat = DXGI_FORMAT_UNKNOWN;
    VkFormat sharedFormat = VK_FORMAT_UNDEFINED;

    if (!VkSharedPictureFormat(format, &dxgiFormat, &sharedFormat))
    {
        _error = std::format("the swapchain's format ({}) is not one Optical F5Low takes", (int) format);
        return AcquireStatus::Unavailable;
    }

    if (family == UINT32_MAX)
    {
        _error = "the present's queue was not seen at device creation";
        return AcquireStatus::Unavailable;
    }

    if (!EnsureDevices(_error))
        return AcquireStatus::Unavailable;

    if (!EnsureRing(family))
        return AcquireStatus::Unavailable;

    if (!_picture.Matches(extent.width, extent.height, sharedFormat))
    {
        WaitRingIdle();

        if (!_picture.Create(_device12, _interop, extent.width, extent.height, dxgiFormat, sharedFormat, true))
        {
            _error = _picture.Error();
            return AcquireStatus::Unavailable;
        }

        LOG_INFO("Native motion (Vulkan): sharing the picture at {}x{}, format {}", extent.width, extent.height,
                 (int) format);
    }

    _slot = (uint32_t) (_frame % kRing);
    Slot& slot = _ring[_slot];

    if (slot.submitted)
    {
        vkWaitForFences(_device, 1, &slot.done, VK_TRUE, UINT64_MAX);
        slot.submitted = false;
    }

    vkResetFences(_device, 1, &slot.done);
    vkResetCommandPool(_device, slot.pool, 0);

    VkCommandBufferBeginInfo begin {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(slot.copyIn, &begin);
    RecordCopyToShared(slot.copyIn, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, _picture, family);
    vkEndCommandBuffer(slot.copyIn);

    // The present's semaphores are binary: their values are ignored, but the arrays must match the counts.
    const uint64_t copied = _fence.Next();
    std::vector<uint64_t> waitValues(_waitCount, 0);
    std::vector<VkPipelineStageFlags> waitStages(_waitCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    const VkSemaphore signal = _fence.Semaphore();

    VkTimelineSemaphoreSubmitInfo values {};
    values.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    values.waitSemaphoreValueCount = _waitCount;
    values.pWaitSemaphoreValues = waitValues.data();
    values.signalSemaphoreValueCount = 1;
    values.pSignalSemaphoreValues = &copied;

    VkSubmitInfo submit {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.pNext = &values;
    submit.waitSemaphoreCount = _waitCount;
    submit.pWaitSemaphores = _waits;
    submit.pWaitDstStageMask = waitStages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &slot.copyIn;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &signal;

    if (const VkResult result = vkQueueSubmit(_queue, 1, &submit, VK_NULL_HANDLE); result != VK_SUCCESS)
    {
        _error = std::format("the copy of the picture could not be submitted ({})", (int) result);
        return AcquireStatus::Unavailable;
    }

    _acquired = true;
    _tookPresentWaits = true;
    _error.clear();
    _image = image;
    _family = family;

    input = FrameInput {};
    input.api = Api::Vulkan;
    input.frame = ++_frame;
    input.picture = _picture.Res12();
    input.pictureFormat = dxgiFormat;
    input.pictureState = D3D12_RESOURCE_STATE_COMMON;
    input.colorSpace = ToColorSpace(space, format);
    input.width = extent.width;
    input.height = extent.height;
    input.depthReadability = DepthReadability::Unknown;
    input.ready = SyncPoint { _fence.Fence12(), copied };
    return AcquireStatus::Ready;
}

void VkFrameSource::Return(const FrameInput& input, const FrameOutput& output)
{
    (void) input;

    if (!_acquired)
        return;

    _acquired = false;
    Slot& slot = _ring[_slot];

    VkSemaphore presentWait = VK_NULL_HANDLE;

    {
        std::lock_guard lock(g_registryMutex);

        if (auto it = g_swapchains.find(_swapchain); it != g_swapchains.end() &&
                                                     _imageIndex < it->second.presentWaits.size())
        {
            VkSemaphore& semaphore = it->second.presentWaits[_imageIndex];

            if (semaphore == VK_NULL_HANDLE)
            {
                VkSemaphoreCreateInfo info {};
                info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                vkCreateSemaphore(_device, &info, nullptr, &semaphore);
            }

            presentWait = semaphore;
        }
    }

    // The producer's done point is on its own fence, which Vulkan cannot see: a signal of ours on the same D3D12 queue,
    // right after its work, stands for it.
    const bool processed = output.done.fence != nullptr && _queue12 != nullptr;
    const uint64_t done = processed ? _fence.Next() : 0;

    if (processed)
    {
        _queue12->Signal(_fence.Fence12(), done);

        VkCommandBufferBeginInfo begin {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(slot.copyOut, &begin);
        RecordCopyFromShared(slot.copyOut, _picture, _image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, _family);
        vkEndCommandBuffer(slot.copyOut);
    }

    // Without a result the submit carries no work: its signal still comes after the copy in, earlier on this queue.
    const VkSemaphore timeline = _fence.Semaphore();
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const uint64_t binaryValue = 0;

    VkTimelineSemaphoreSubmitInfo values {};
    values.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    values.waitSemaphoreValueCount = processed ? 1 : 0;
    values.pWaitSemaphoreValues = &done;
    values.signalSemaphoreValueCount = presentWait != VK_NULL_HANDLE ? 1 : 0;
    values.pSignalSemaphoreValues = &binaryValue;

    VkSubmitInfo submit {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.pNext = &values;
    submit.waitSemaphoreCount = processed ? 1 : 0;
    submit.pWaitSemaphores = &timeline;
    submit.pWaitDstStageMask = &stage;
    submit.commandBufferCount = processed ? 1 : 0;
    submit.pCommandBuffers = &slot.copyOut;
    submit.signalSemaphoreCount = presentWait != VK_NULL_HANDLE ? 1 : 0;
    submit.pSignalSemaphores = &presentWait;

    const VkResult result = vkQueueSubmit(_queue, 1, &submit, slot.done);
    slot.submitted = result == VK_SUCCESS;

    if (result != VK_SUCCESS)
    {
        static bool logged = false;

        if (!logged)
        {
            logged = true;
            LOG_ERROR("Native motion (Vulkan): the copy back could not be submitted ({})", (int) result);
        }

        presentWait = VK_NULL_HANDLE;
    }

    _presentWait = presentWait;
}

} // namespace native
