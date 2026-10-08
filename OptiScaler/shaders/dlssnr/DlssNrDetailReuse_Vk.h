#pragma once

// Reuse detail between frames on Vulkan: the dispatches of precompile/dlssnr_detail_reuse.hlsl (SPIR-V).
//
// The same shader as D3D12's DlssNr_Dx12::DispatchDetailReuse, compiled from the same source. It reads six textures and
// writes two, which DlssNr_Vk's layout (four sampled, two storage) cannot hold, so it has a pass and a descriptor set
// layout of its own, one binding per D3D12 register: 0 constants (b0), 1-5 sampled (t0-t4), 6-7 storage (u0-u1),
// 8 the linear sampler (s0), 9 sampled (t5, the history distrust; no Vulkan route supplies one yet). What the frame
// does with it is dlssnr/DlssNr_DetailReuse_Vk.inl.
//
// As in DlssNr_Vk: every binding is written every dispatch (a slot a mode does not use gets a 1x1 placeholder),
// constants and descriptor sets come from a ring of slots so dispatches of one frame and of frames still in flight
// never share one, and the layouts of the images are the caller's to state. Each dispatch is followed by a
// compute-to-compute barrier.
//
// The storage images are declared without a format (they write RGBA16F, RG32F, RGBA32F or the answers' format by
// mode), which needs the device's shaderStorageImageWriteWithoutFormat (DlssNr_VkExtensions.h switches it on at device
// creation where the device offers it); Ready() says whether this device has it.

#include "SysUtils.h"
#include <shaders/Shader_Vk.h>
#include "DlssNr_DetailReuseConstants.h"

class DlssNrDetailReuse_Vk : public Shader_Vk
{
    // A frame records at most six dispatches (reused: Estimate, Coverage, Fill, SaveMotion, Capture; a paused full
    // frame with steadiness: Compose, Estimate, Coverage, Steady, Capture), so the 36 slots cover six frames: twice the
    // three that can be in flight, with no fence, on the assumption of one NR evaluate per rendered frame.
    static constexpr uint32_t kSlotsPerFrame = 12;
    static constexpr uint32_t kFramesInFlight = 3;
    static constexpr uint32_t kSlots = kSlotsPerFrame * kFramesInFlight;

    VkDeviceSize _slotStride = 0;
    uint32_t _slot = 0;

    VkImage _dummyImage = VK_NULL_HANDLE;
    VkDeviceMemory _dummyMemory = VK_NULL_HANDLE;
    VkImageView _dummyView = VK_NULL_HANDLE;
    bool _dummyReady = false;
    bool _dummyFailed = false; // not tried again: a half-made placeholder is never used

    bool CreateDummy(VkCommandBuffer cmdList);

  public:
    // A sampled read: the view, and the layout its image is in when the dispatch runs. A null view binds the
    // placeholder.
    struct Read
    {
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    };

    DlssNrDetailReuse_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice);
    ~DlssNrDetailReuse_Vk();

    bool Ready() const { return CanRender(); }

    // t0-t4, then t5 (the history distrust, DlssNrDetailReuseConstants::HistoryDistrust). Arrays of this size given
    // fewer entries leave the rest null, which binds the placeholder.
    static constexpr uint32_t kReads = 6;

    // One dispatch over InWidth x InHeight threads (8x8 groups). InReads are t0-t5; InTarget (u0) must be given,
    // InSecond (u1) may be null. Both written images are in GENERAL.
    bool Dispatch(VkCommandBuffer InCmdList, const DlssNrDetailReuseConstants& InConstants, uint32_t InWidth,
                  uint32_t InHeight, const Read (&InReads)[kReads], VkImageView InTarget, VkImageView InSecond);
};
