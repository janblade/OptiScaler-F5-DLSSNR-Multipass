#pragma once

// The transport between a Vulkan game device and the D3D12 device the native input producer runs on: the Vulkan
// counterpart of native/SharedFrame.h. A resource is made on the D3D12 side (a shared committed resource) and opened in
// Vulkan as an image bound to imported memory; one D3D12 fence is opened in Vulkan as a timeline semaphore. The game's
// queue copies into the shared image and signals the fence, the D3D12 queue waits for it, works and signals again, and
// the game's queue waits for that before copying back: all on the GPU, no CPU wait.
//
// Needs, on the Vulkan device: VK_KHR_external_memory_win32, VK_KHR_external_semaphore_win32 and the timeline
// semaphore feature (hooks/Vulkan_Hooks.cpp switches it on at device creation when the device offers it).
//
// Not built with the precompiled header: Vulkan and Direct3D only, so tests/nr_shared_frame_vk_gpu.cpp compiles it alone.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <cstdint>
#include <string>

namespace native
{

// The Vulkan device and the few functions of it the transport calls, loaded once per device.
struct VkInterop
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    PFN_vkGetMemoryWin32HandlePropertiesKHR getMemoryWin32HandleProperties = nullptr;
    PFN_vkImportSemaphoreWin32HandleKHR importSemaphoreWin32Handle = nullptr;
    VkPhysicalDeviceMemoryProperties memory {};

    // False with a reason when the device lacks the external memory or semaphore functions.
    bool Load(VkPhysicalDevice physicalDevice, VkDevice vkDevice, std::string& error);
    bool Ready() const { return device != VK_NULL_HANDLE && getMemoryWin32HandleProperties != nullptr; }
};

// The adapter LUID of a Vulkan physical device, to make the D3D12 device on the same GPU. False when the driver does not
// report one.
bool VkPhysicalDeviceLuid(VkPhysicalDevice physicalDevice, LUID& luid);

// The format a Vulkan picture is shared in, as a DXGI format and as the Vulkan format of the shared image. sRGB forms
// become the plain UNORM one (a copy between the two keeps the bits, and the producer reads encoded values). False for
// a format the producer does not take.
bool VkSharedPictureFormat(VkFormat swapchainFormat, DXGI_FORMAT* dxgi, VkFormat* shared);

// A 2D texture made on the D3D12 device and opened in Vulkan. The D3D12 side rests in COMMON (the producer transitions
// it as it needs); the Vulkan side is handed to the D3D12 queue in GENERAL, released to VK_QUEUE_FAMILY_EXTERNAL.
class SharedImageVk
{
  public:
    ~SharedImageVk() { Reset(); }

    // Replaces what was there. `uav`: the D3D12 side may be written through an unordered access view (DLSS-NR writes
    // its result over the picture).
    bool Create(ID3D12Device* device12, const VkInterop& vk, uint32_t width, uint32_t height, DXGI_FORMAT dxgiFormat,
                VkFormat vkFormat, bool uav);

    // The caller has made sure neither API still uses it.
    void Reset();

    ID3D12Resource* Res12() const { return _res12.Get(); }
    VkImage Image() const { return _image; }
    uint32_t Width() const { return _width; }
    uint32_t Height() const { return _height; }
    VkFormat Format() const { return _vkFormat; }
    bool Matches(uint32_t width, uint32_t height, VkFormat format) const
    {
        return _image != VK_NULL_HANDLE && _width == width && _height == height && _vkFormat == format;
    }

    const std::string& Error() const { return _error; }

  private:
    Microsoft::WRL::ComPtr<ID3D12Resource> _res12;
    VkDevice _device = VK_NULL_HANDLE;
    VkImage _image = VK_NULL_HANDLE;
    VkDeviceMemory _memory = VK_NULL_HANDLE;
    uint32_t _width = 0;
    uint32_t _height = 0;
    VkFormat _vkFormat = VK_FORMAT_UNDEFINED;
    std::string _error;
};

// One D3D12 fence, opened in Vulkan as a timeline semaphore. Both sides signal it, always with a higher value than any
// before.
class SharedFenceVk
{
  public:
    ~SharedFenceVk() { Reset(); }

    bool Create(ID3D12Device* device12, const VkInterop& vk);
    void Reset();

    bool Ready() const { return _fence12 != nullptr && _semaphore != VK_NULL_HANDLE; }
    ID3D12Fence* Fence12() const { return _fence12.Get(); }
    VkSemaphore Semaphore() const { return _semaphore; }

    uint64_t Next() { return ++_value; }
    uint64_t Value() const { return _value; }

    const std::string& Error() const { return _error; }

  private:
    Microsoft::WRL::ComPtr<ID3D12Fence> _fence12;
    VkDevice _device = VK_NULL_HANDLE;
    VkSemaphore _semaphore = VK_NULL_HANDLE;
    uint64_t _value = 0;
    std::string _error;
};

// The copies on the game's queue, recorded into a command buffer of the caller's. `family` is the queue family the
// command buffer is submitted on.
//
// In: the game's image (in `layout`, left in it) is copied into the shared image, which is then handed to the D3D12
// queue. Its old contents are discarded.
void RecordCopyToShared(VkCommandBuffer cmd, VkImage source, VkImageLayout layout, const SharedImageVk& shared,
                        uint32_t family);

// Out: the shared image, as the D3D12 queue left it, is taken back and copied into the game's image (in `layout`, left
// in it).
void RecordCopyFromShared(VkCommandBuffer cmd, const SharedImageVk& shared, VkImage target, VkImageLayout layout,
                          uint32_t family);

} // namespace native
