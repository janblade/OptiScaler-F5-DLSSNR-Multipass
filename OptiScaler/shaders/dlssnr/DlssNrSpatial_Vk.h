#pragma once

// Compress screen edges on Vulkan: the dispatches of precompile/dlssnr_spatial.hlsl (SPIR-V), the same shader as
// D3D12's DlssNr_Dx12::DispatchSpatial. What the frame does with them is dlssnr/DlssNr_Spatial_Vk.inl.
//
// Its bindings are the ones the shader declares under VK_MODE: 0 constants, 1-3 sampled (t0-t2), 5-6 storage (u0-u1),
// 7 the linear sampler. The storage images carry their formats in the shader (RGBA16F for the colour passes, R32F and
// RGBA32F for depth and motion), so the pass needs no optional device feature.
//
// As DlssNr_Vk and DlssNrDetailReuse_Vk: every binding is written every dispatch (a slot a mode does not use gets a
// 1x1 placeholder), constants and descriptor sets come from a ring of slots so the dispatches of one frame and of the
// frames still in flight never share one, the layouts of the images are the caller's to state, and each dispatch is
// followed by a compute-to-compute barrier.

#include "SysUtils.h"
#include <shaders/Shader_Vk.h>
#include "DlssNr_Spatial.h"

class DlssNrSpatial_Vk : public Shader_Vk
{
    // A frame records three dispatches (colour pack, guides pack, unpack); a slot is four, to leave room, for six
    // frames: twice the three that can be in flight, with no fence, on the assumption of one NR evaluate per rendered
    // frame.
    static constexpr uint32_t kSlotsPerFrame = 4;
    static constexpr uint32_t kFramesInFlight = 6;
    static constexpr uint32_t kSlots = kSlotsPerFrame * kFramesInFlight;

    VkDeviceSize _slotStride = 0;
    uint32_t _slot = 0;

    VkPipeline _guidesPipeline = VK_NULL_HANDLE; // mode 101; _pipeline is the colour pack / unpack

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

    DlssNrSpatial_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice);
    ~DlssNrSpatial_Vk();

    bool Ready() const { return CanRender() && _guidesPipeline != VK_NULL_HANDLE; }

    // t0-t2. By mode (InConstants.mode):
    //   100  InReads[0] the encoded colour                                -> InOut0 the packed colour (RGBA16F)
    //   101  InReads[1] depth, InReads[2] motion                          -> InOut0 R32F depth, InOut1 RGBA32F motion
    //   102  InReads[0] the packed model input, InReads[1] the answer     -> InOut0 the unpacked input, InOut1 the answer
    // Width x Height threads come from the constants (8x8 groups). The written images are in GENERAL.
    bool Dispatch(VkCommandBuffer InCmdList, const DlssNr::Spatial::Constants& InConstants, const Read (&InReads)[3],
                  VkImageView InOut0, VkImageView InOut1);
};
