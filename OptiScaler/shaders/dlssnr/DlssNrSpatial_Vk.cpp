#include "pch.h"

#include "DlssNrSpatial_Vk.h"

#include "precompile/dlssnr_spatial_Shader_Vk.h"
#include "precompile/dlssnr_spatial_guides_Shader_Vk.h"

#include <algorithm>
#include <cstring>

DlssNrSpatial_Vk::DlssNrSpatial_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice)
    : Shader_Vk(InName, InDevice, InPhysicalDevice)
{
    if (InDevice == VK_NULL_HANDLE || InPhysicalDevice == VK_NULL_HANDLE)
    {
        _init = false;
        return;
    }

    // Linear clamp, as the D3D12 pass's s0: the colour is read between texels.
    CreateSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    VkPhysicalDeviceProperties props {};
    vkGetPhysicalDeviceProperties(_physicalDevice, &props);

    const VkDeviceSize alignment = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 1);
    _slotStride = ((sizeof(DlssNr::Spatial::Constants) + alignment - 1) / alignment) * alignment;

    if (!CreateBufferResource(_device, _physicalDevice, &_constantBuffer, &_constantBufferMemory,
                              _slotStride * kSlots, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
        vkMapMemory(_device, _constantBufferMemory, 0, _slotStride * kSlots, 0, &_mappedConstantBuffer) != VK_SUCCESS)
    {
        LOG_ERROR("DLSS-NR Vulkan compress screen edges: could not allocate its constant ring");
        _init = false;
        return;
    }

    // The [[vk::binding]] numbers of dlssnr_spatial.hlsl under VK_MODE, entry for entry (binding 4 is not used).
    // Combined image samplers for the reads, as DlssNr_Vk: the shader's own sampler is declared separately and the
    // combined one's is unused.
    std::vector<VkDescriptorSetLayoutBinding> bindings = {
        CreateBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),         // Params
        CreateBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER), // gSource0
        CreateBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER), // gSource1
        CreateBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER), // gSource2
        CreateBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),          // gTarget0
        CreateBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),          // gTarget1
        CreateBinding(7, VK_DESCRIPTOR_TYPE_SAMPLER),                // gLinearClamp
    };

    CreateLayouts(bindings);

    std::vector<VkDescriptorPoolSize> poolSizes = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSlots },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * kSlots },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * kSlots },
        { VK_DESCRIPTOR_TYPE_SAMPLER, kSlots },
    };

    CreateDescriptorPool(poolSizes, kSlots);

    // One set per slot (CreateDescriptorSets makes _maxFramesInFlight of them).
    _maxFramesInFlight = kSlots;
    CreateDescriptorSets(_descriptorSetLayout, _descriptorPool, _descriptorSets);
    _maxFramesInFlight = kFramesInFlight;

    bool setsOk = _descriptorSets.size() >= kSlots;
    for (const VkDescriptorSet set : _descriptorSets)
        setsOk = setsOk && set != VK_NULL_HANDLE;

    if (!setsOk || _descriptorSetLayout == VK_NULL_HANDLE || _pipelineLayout == VK_NULL_HANDLE ||
        _descriptorPool == VK_NULL_HANDLE || _textureSampler == VK_NULL_HANDLE)
    {
        LOG_ERROR("DLSS-NR Vulkan compress screen edges: its descriptor sets, layouts or sampler could not be created");
        _init = false;
        return;
    }

    std::vector<char> colourCode(dlssnr_spatial_spv, dlssnr_spatial_spv + sizeof(dlssnr_spatial_spv));
    std::vector<char> guidesCode(dlssnr_spatial_guides_spv,
                                 dlssnr_spatial_guides_spv + sizeof(dlssnr_spatial_guides_spv));

    if (!CreateComputePipeline(_device, _pipelineLayout, &_pipeline, colourCode) ||
        !CreateComputePipeline(_device, _pipelineLayout, &_guidesPipeline, guidesCode))
    {
        LOG_ERROR("DLSS-NR Vulkan compress screen edges: could not create its compute pipelines");
        _init = false;
        return;
    }

    _init = true;
}

DlssNrSpatial_Vk::~DlssNrSpatial_Vk()
{
    if (_device == VK_NULL_HANDLE)
        return;

    if (_guidesPipeline != VK_NULL_HANDLE)
        vkDestroyPipeline(_device, _guidesPipeline, nullptr);

    if (_dummyView != VK_NULL_HANDLE)
        vkDestroyImageView(_device, _dummyView, nullptr);

    if (_dummyImage != VK_NULL_HANDLE)
        vkDestroyImage(_device, _dummyImage, nullptr);

    if (_dummyMemory != VK_NULL_HANDLE)
        vkFreeMemory(_device, _dummyMemory, nullptr);
}

// One RGBA16F pixel in GENERAL, legal for both a sampled read and a storage write; its content is never read. It
// stands in for the sampled slots a mode does not use, and for the second storage image of the colour passes (which
// the shader declares as RGBA16F too).
bool DlssNrSpatial_Vk::CreateDummy(VkCommandBuffer cmdList)
{
    if (_dummyReady)
        return true;

    if (_dummyFailed)
        return false;

    _dummyFailed = true;

    VkImageCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    info.extent = { 1, 1, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(_device, &info, nullptr, &_dummyImage) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req {};
    vkGetImageMemoryRequirements(_device, _dummyImage, &req);

    VkMemoryAllocateInfo alloc {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemoryType(_physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(_device, &alloc, nullptr, &_dummyMemory) != VK_SUCCESS ||
        vkBindImageMemory(_device, _dummyImage, _dummyMemory, 0) != VK_SUCCESS)
        return false;

    VkImageViewCreateInfo view {};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = _dummyImage;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    if (vkCreateImageView(_device, &view, nullptr, &_dummyView) != VK_SUCCESS)
        return false;

    VkImageSubresourceRange range { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    SetImageLayout(cmdList, _dummyImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, range);

    _dummyFailed = false;
    _dummyReady = true;
    return true;
}

bool DlssNrSpatial_Vk::Dispatch(VkCommandBuffer InCmdList, const DlssNr::Spatial::Constants& InConstants,
                                const Read (&InReads)[3], VkImageView InOut0, VkImageView InOut1)
{
    if (!Ready() || InCmdList == VK_NULL_HANDLE || InOut0 == VK_NULL_HANDLE || InConstants.width == 0 ||
        InConstants.height == 0)
        return false;

    const bool guides = InConstants.mode == 101;

    // The guides pass writes two images of its own formats (R32F, RGBA32F); the colour passes write RGBA16F, the second
    // of which the packing does not use and the placeholder stands in for.
    if (guides && InOut1 == VK_NULL_HANDLE)
        return false;

    if (!CreateDummy(InCmdList))
    {
        LOG_ERROR("DLSS-NR Vulkan compress screen edges: could not create its placeholder image");
        return false;
    }

    const uint32_t slot = _slot;
    _slot = (_slot + 1) % kSlots;

    const VkDeviceSize offset = _slotStride * slot;
    std::memcpy((char*) _mappedConstantBuffer + offset, &InConstants, sizeof(DlssNr::Spatial::Constants));

    const VkDescriptorSet set = _descriptorSets[slot];
    VkDescriptorBufferInfo bufferInfo { _constantBuffer, offset, sizeof(DlssNr::Spatial::Constants) };

    VkDescriptorImageInfo reads[3] {};
    for (uint32_t i = 0; i < 3; ++i)
    {
        const bool given = InReads[i].view != VK_NULL_HANDLE;
        reads[i] = { _textureSampler, given ? InReads[i].view : _dummyView,
                     given ? InReads[i].layout : VK_IMAGE_LAYOUT_GENERAL };
    }

    VkDescriptorImageInfo out0 { VK_NULL_HANDLE, InOut0, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo out1 { VK_NULL_HANDLE, InOut1 != VK_NULL_HANDLE ? InOut1 : _dummyView,
                                 VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo sampler { _textureSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };

    VkWriteDescriptorSet writes[7] {};
    const uint32_t bindingOf[7] = { 0, 1, 2, 3, 5, 6, 7 };
    for (uint32_t i = 0; i < 7; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = bindingOf[i];
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &bufferInfo;
    for (uint32_t i = 0; i < 3; ++i)
    {
        writes[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1 + i].pImageInfo = &reads[i];
    }
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[4].pImageInfo = &out0;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[5].pImageInfo = &out1;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[6].pImageInfo = &sampler;

    vkUpdateDescriptorSets(_device, 7, writes, 0, nullptr);

    vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, guides ? _guidesPipeline : _pipeline);
    vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(InCmdList, (InConstants.width + 7) / 8, (InConstants.height + 7) / 8, 1);

    // What this dispatch wrote, the next one reads.
    VkMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(InCmdList, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &barrier, 0, nullptr, 0, nullptr);

    return true;
}
