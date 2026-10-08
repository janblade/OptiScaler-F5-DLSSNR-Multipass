#include "pch.h"

#include "DlssNrDetailReuse_Vk.h"

#include "precompile/dlssnr_detail_reuse_Shader_Vk.h"

#include <algorithm>
#include <cstring>

DlssNrDetailReuse_Vk::DlssNrDetailReuse_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice)
    : Shader_Vk(InName, InDevice, InPhysicalDevice)
{
    if (InDevice == VK_NULL_HANDLE || InPhysicalDevice == VK_NULL_HANDLE)
    {
        _init = false;
        return;
    }

    // Linear clamp, as the D3D12 pass's s0: the moved detail and the depth guide are read between texels.
    CreateSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    VkPhysicalDeviceProperties props {};
    vkGetPhysicalDeviceProperties(_physicalDevice, &props);

    const VkDeviceSize alignment = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 1);
    _slotStride = ((sizeof(DlssNrDetailReuseConstants) + alignment - 1) / alignment) * alignment;

    if (!CreateBufferResource(_device, _physicalDevice, &_constantBuffer, &_constantBufferMemory,
                              _slotStride * kSlots, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
        vkMapMemory(_device, _constantBufferMemory, 0, _slotStride * kSlots, 0, &_mappedConstantBuffer) != VK_SUCCESS)
    {
        LOG_ERROR("DLSS-NR Vulkan detail reuse: could not allocate its constant ring");
        _init = false;
        return;
    }

    // The [[vk::binding]] numbers of dlssnr_detail_reuse.hlsl under VK_MODE, entry for entry. Combined image samplers
    // for the reads, as DlssNr_Vk: the shader's own sampler is declared separately and the combined one's is unused.
    std::vector<VkDescriptorSetLayoutBinding> bindings = {
        CreateBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),         // b0
        CreateBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER), // t0
        CreateBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER), // t1
        CreateBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER), // t2
        CreateBinding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER), // t3
        CreateBinding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER), // t4
        CreateBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),          // u0
        CreateBinding(7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),          // u1
        CreateBinding(8, VK_DESCRIPTOR_TYPE_SAMPLER),                // s0
        CreateBinding(9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER), // t5
    };

    CreateLayouts(bindings);

    std::vector<VkDescriptorPoolSize> poolSizes = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSlots },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kReads * kSlots },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * kSlots },
        { VK_DESCRIPTOR_TYPE_SAMPLER, kSlots },
    };

    CreateDescriptorPool(poolSizes, kSlots);

    // One set per slot (CreateDescriptorSets makes _maxFramesInFlight of them).
    _maxFramesInFlight = kSlots;
    CreateDescriptorSets(_descriptorSetLayout, _descriptorPool, _descriptorSets);
    _maxFramesInFlight = kFramesInFlight;

    // The helpers log their own failures but do not report them: a failed pool still leaves kSlots null sets.
    bool setsOk = _descriptorSets.size() >= kSlots;
    for (const VkDescriptorSet set : _descriptorSets)
        setsOk = setsOk && set != VK_NULL_HANDLE;

    if (!setsOk || _descriptorSetLayout == VK_NULL_HANDLE || _pipelineLayout == VK_NULL_HANDLE ||
        _descriptorPool == VK_NULL_HANDLE || _textureSampler == VK_NULL_HANDLE)
    {
        LOG_ERROR("DLSS-NR Vulkan detail reuse: its descriptor sets, layouts or sampler could not be created");
        _init = false;
        return;
    }

    std::vector<char> shaderCode(dlssnr_detail_reuse_spv, dlssnr_detail_reuse_spv + sizeof(dlssnr_detail_reuse_spv));

    if (!CreateComputePipeline(_device, _pipelineLayout, &_pipeline, shaderCode))
    {
        LOG_ERROR("DLSS-NR Vulkan detail reuse: could not create its compute pipeline");
        _init = false;
        return;
    }

    _init = true;
}

DlssNrDetailReuse_Vk::~DlssNrDetailReuse_Vk()
{
    if (_device == VK_NULL_HANDLE)
        return;

    if (_dummyView != VK_NULL_HANDLE)
        vkDestroyImageView(_device, _dummyView, nullptr);

    if (_dummyImage != VK_NULL_HANDLE)
        vkDestroyImage(_device, _dummyImage, nullptr);

    if (_dummyMemory != VK_NULL_HANDLE)
        vkFreeMemory(_device, _dummyMemory, nullptr);
}

// One RGBA16F pixel in GENERAL, legal for both a sampled read and a storage write; its content is never read.
bool DlssNrDetailReuse_Vk::CreateDummy(VkCommandBuffer cmdList)
{
    if (_dummyReady)
        return true;

    if (_dummyFailed)
        return false;

    _dummyFailed = true;

    {
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
    }

    VkImageSubresourceRange range { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    SetImageLayout(cmdList, _dummyImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, range);

    _dummyFailed = false;
    _dummyReady = true;
    return true;
}

bool DlssNrDetailReuse_Vk::Dispatch(VkCommandBuffer InCmdList, const DlssNrDetailReuseConstants& InConstants,
                                    uint32_t InWidth, uint32_t InHeight, const Read (&InReads)[kReads],
                                    VkImageView InTarget, VkImageView InSecond)
{
    if (!CanRender() || InCmdList == VK_NULL_HANDLE || InTarget == VK_NULL_HANDLE || InWidth == 0 || InHeight == 0)
        return false;

    if (!CreateDummy(InCmdList))
    {
        LOG_ERROR("DLSS-NR Vulkan detail reuse: could not create its placeholder image");
        return false;
    }

    const uint32_t slot = _slot;
    _slot = (_slot + 1) % kSlots;

    const VkDeviceSize offset = _slotStride * slot;
    std::memcpy((char*) _mappedConstantBuffer + offset, &InConstants, sizeof(DlssNrDetailReuseConstants));

    const VkDescriptorSet set = _descriptorSets[slot];
    VkDescriptorBufferInfo bufferInfo { _constantBuffer, offset, sizeof(DlssNrDetailReuseConstants) };

    VkDescriptorImageInfo reads[kReads] {};
    for (uint32_t i = 0; i < kReads; ++i)
    {
        const bool given = InReads[i].view != VK_NULL_HANDLE;
        reads[i] = { _textureSampler, given ? InReads[i].view : _dummyView,
                     given ? InReads[i].layout : VK_IMAGE_LAYOUT_GENERAL };
    }

    VkDescriptorImageInfo target { VK_NULL_HANDLE, InTarget, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo second { VK_NULL_HANDLE, InSecond != VK_NULL_HANDLE ? InSecond : _dummyView,
                                   VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo sampler { _textureSampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };

    VkWriteDescriptorSet writes[10] {};
    for (uint32_t i = 0; i < 10; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &bufferInfo;
    for (uint32_t i = 0; i < 5; ++i)
    {
        writes[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1 + i].pImageInfo = &reads[i];
    }
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[6].pImageInfo = &target;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[7].pImageInfo = &second;
    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[8].pImageInfo = &sampler;
    writes[9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[9].pImageInfo = &reads[5];

    vkUpdateDescriptorSets(_device, 10, writes, 0, nullptr);

    vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
    vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(InCmdList, (InWidth + 7) / 8, (InHeight + 7) / 8, 1);

    // What this dispatch wrote, the next one reads.
    VkMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(InCmdList, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &barrier, 0, nullptr, 0, nullptr);

    return true;
}
