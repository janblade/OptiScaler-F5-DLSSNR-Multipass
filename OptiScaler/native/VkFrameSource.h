#pragma once

// The Vulkan adapter's frame source: the game's swapchain image is copied into a texture shared with a private D3D12
// device (native/SharedFrameVk.h), the producer works on it there, and the result is copied back before the present.
// Everything is ordered on the GPU through one D3D12 fence opened as a Vulkan timeline semaphore:
//
//   Acquire, on the present's queue:  wait for the present's semaphores -> copy in -> signal N
//   the producer, on the D3D12 queue: wait N -> optical flow, trust mask, DLSS-NR -> (Return) signal N+1
//   Return, on the present's queue:   wait N+1 -> copy back -> signal the semaphore the present now waits on
//
// So the present waits for NR on the GPU, and the CPU waits for nothing (the command ring's slots are a few frames old
// when reused). When the producer gives no result, Return still hands the present a semaphore, signalled after the copy
// in; the picture is then the game's own.
//
// The hooks (hooks/Vulkan_Hooks.cpp) tell it about the devices' queues and the swapchains, since Vulkan cannot be asked
// afterwards which family a queue is of or what a swapchain's images are.

#include "FrameContract.h"
#include "SharedFrameVk.h"

#include <cstdint>
#include <string>
#include <vector>

namespace native
{

class VkFrameSource : public IFrameSource
{
  public:
    ~VkFrameSource() override { Release(); }

    // The present being processed. `waits` are the present's own wait semaphores: when Acquire returns Ready it has
    // waited on them, and the present must wait on PresentWait() instead once Return has run. Call before Acquire().
    void SetPresent(VkDevice device, VkPhysicalDevice physical, VkQueue queue, VkSwapchainKHR swapchain,
                    uint32_t imageIndex, const VkSemaphore* waits, uint32_t waitCount);

    // After Return: the semaphore the present waits on in place of its own list, or VK_NULL_HANDLE when this frame was
    // not acquired (the present keeps its own list then).
    VkSemaphore PresentWait() const { return _presentWait; }

    // Acquire waited on the present's own semaphores: the present must not wait on them again (they are unsignalled
    // now), only on PresentWait(), or on nothing when that is null.
    bool TookPresentWaits() const { return _tookPresentWaits; }

    Api GetApi() const override { return Api::Vulkan; }
    AcquireStatus Acquire(FrameInput& input) override;
    void Return(const FrameInput& input, const FrameOutput& output) override;
    void OnResize() override {}

    ID3D12Device* Device() const override { return _device12; }
    ID3D12CommandQueue* Queue() const override { return _queue12; }
    const std::string& Error() const { return _error; }

    // Everything made on `device` goes (the game is destroying it).
    void OnDeviceDestroyed(VkDevice device);

    // From the hooks. A device's queues by family, as created.
    static void NoteDevice(VkDevice device, const VkDeviceCreateInfo& info);
    // The usage a swapchain is made with: TRANSFER_SRC and TRANSFER_DST added when the surface allows them and NR is on.
    static VkImageUsageFlags SwapchainUsage(VkPhysicalDevice physical, const VkSwapchainCreateInfoKHR& info);
    // A swapchain was made: its images, format and colour space. The one it replaces is forgotten.
    static void NoteSwapchain(VkDevice device, VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& info);
    // The picture size of a swapchain seen at creation.
    static bool SwapchainExtent(VkSwapchainKHR swapchain, uint32_t* width, uint32_t* height);

  private:
    static constexpr uint32_t kRing = 3;

    struct Slot
    {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer copyIn = VK_NULL_HANDLE;
        VkCommandBuffer copyOut = VK_NULL_HANDLE;
        VkFence done = VK_NULL_HANDLE; // signalled by the slot's last submit
        bool submitted = false;
    };

    bool EnsureDevices(std::string& why);
    bool EnsureRing(uint32_t family);
    void ReleaseRing();
    void WaitRingIdle();
    void ReleaseDepth();
    void Release();

    // The scene's depth for this frame (resource_tracking/GenericDepth_Vk.h): the finder's buffer into _depthShared in
    // the copy-in command buffer, then on D3D12 into _depthTexture. False when the frame has none.
    bool RecordDepth(VkCommandBuffer copyIn, uint32_t family, FrameInput& input);
    // After the copy-in submit: the D3D12 side of RecordDepth, ordered after `copied` on the D3D12 queue.
    void SubmitDepthCopy(uint64_t copied);

    // The present being processed.
    VkDevice _device = VK_NULL_HANDLE;
    VkPhysicalDevice _physical = VK_NULL_HANDLE;
    VkQueue _queue = VK_NULL_HANDLE;
    VkSwapchainKHR _swapchain = VK_NULL_HANDLE;
    uint32_t _imageIndex = 0;
    const VkSemaphore* _waits = nullptr;
    uint32_t _waitCount = 0;

    // Between Acquire and Return.
    bool _acquired = false;
    bool _tookPresentWaits = false;
    VkImage _image = VK_NULL_HANDLE;
    uint32_t _family = 0;
    uint32_t _slot = 0;
    VkSemaphore _presentWait = VK_NULL_HANDLE;

    // Made on first use, for _interop.device.
    VkInterop _interop;
    ID3D12Device* _device12 = nullptr;
    ID3D12CommandQueue* _queue12 = nullptr;
    SharedFenceVk _fence;
    SharedImageVk _picture;
    Slot _ring[kRing];
    uint32_t _ringFamily = UINT32_MAX;

    // The depth path. Its D3D12 copy has a fence of its own (not the shared one, whose values Vulkan also signals).
    SharedBufferVk _depthShared;
    Microsoft::WRL::ComPtr<ID3D12Resource> _depthTexture;
    VkDepthCopyFormat _depthFormat;
    uint32_t _depthWidth = 0;
    uint32_t _depthHeight = 0;
    bool _depthPending = false; // RecordDepth recorded this frame's copy; SubmitDepthCopy sends the D3D12 half
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> _depthAllocators[kRing];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> _depthList;
    Microsoft::WRL::ComPtr<ID3D12Fence> _depthFence;
    uint64_t _depthFenceValue = 0;
    uint64_t _depthSlotValue[kRing] = {};
    uint64_t _frame = 0;
    std::string _error;
};

} // namespace native
