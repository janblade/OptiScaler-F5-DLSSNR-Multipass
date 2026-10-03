// Runs the production SPIR-V's automatic-exposure passes and its live white point on a real Vulkan
// device. No game or NR DLL needed.
//   1. The parallel 64x64 meter and the tile-mean reduction (modes 3 and 13) against exposures worked
//      out by hand, with and without highlight protection.
//   2. The white point the shader recomputes from an exposure image bound in the motion slot, with the
//      Trim slider, with Trim anchors, and with the live path off. The resolve paints that white point
//      into the compare divider's column, which is what is read back.
// cl /std:c++20 /EHsc /Iexternal/vulkan/include tests/nr_auto_exposure_smoke.cpp
//    /link OptiScaler/library/vulkan/vulkan-1.lib
#include <vulkan/vulkan.h>
#include "../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#include <array>
#include <vector>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstring>
#include <cmath>

static void check(VkResult r) { if (r != VK_SUCCESS) throw std::runtime_error("Vulkan result " + std::to_string(r)); }

constexpr uint32_t kFrame = 640;    // 10x10 pixels per meter tile
constexpr uint32_t kGrid = 64;
constexpr uint32_t kSide = kFrame;  // every image is kSide x kSide, so one size serves all bindings

static int failures = 0;
static void expect(const char* what, float got, float want, float tolerance)
{
    const bool ok = std::isfinite(got) && std::abs(got - want) <= tolerance * std::max(1.0f, std::abs(want));
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << ": got " << got << ", want " << want << '\n';
    if (!ok) ++failures;
}

int main(int argc, char** argv)
try
{
    if (argc != 2) throw std::runtime_error("Pass the production DlssNr_Shader_Vk.spv path");
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open SPIR-V");
    const auto bytes = (size_t) file.tellg();
    if (!bytes || bytes % 4) throw std::runtime_error("Invalid SPIR-V length");
    std::vector<uint32_t> code(bytes / 4);
    file.seekg(0); file.read((char*) code.data(), bytes);

    VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "NR auto exposure smoke"; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ici.pApplicationInfo = &app;
    VkInstance instance {}; check(vkCreateInstance(&ici, nullptr, &instance));
    uint32_t count = 0; check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count); check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    if (!count) throw std::runtime_error("No Vulkan device");
    VkPhysicalDevice pd = devices.front();
    for (auto d : devices) { VkPhysicalDeviceProperties p {}; vkGetPhysicalDeviceProperties(d, &p); if (p.vendorID == 0x10de) pd = d; }
    VkPhysicalDeviceProperties props {}; vkGetPhysicalDeviceProperties(pd, &props);
    std::cout << props.deviceName << '\n';
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count); vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, families.data());
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    if (family == count) throw std::runtime_error("No compute queue");
    float priority = 1;
    VkDeviceQueueCreateInfo qci { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures available {}; vkGetPhysicalDeviceFeatures(pd, &available);
    VkPhysicalDeviceFeatures enabled {};
    enabled.shaderStorageImageReadWithoutFormat = available.shaderStorageImageReadWithoutFormat;
    enabled.shaderStorageImageWriteWithoutFormat = available.shaderStorageImageWriteWithoutFormat;
    VkDeviceCreateInfo dci { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci; dci.pEnabledFeatures = &enabled;
    VkDevice device {}; check(vkCreateDevice(pd, &dci, nullptr, &device));
    VkQueue queue {}; vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties memory {}; vkGetPhysicalDeviceMemoryProperties(pd, &memory);
    auto allocate = [&](VkMemoryRequirements req, VkMemoryPropertyFlags flags) {
        VkMemoryAllocateInfo ai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; ai.allocationSize = req.size;
        for (ai.memoryTypeIndex = 0; ai.memoryTypeIndex < memory.memoryTypeCount; ++ai.memoryTypeIndex)
            if ((req.memoryTypeBits & (1u << ai.memoryTypeIndex)) &&
                (memory.memoryTypes[ai.memoryTypeIndex].propertyFlags & flags) == flags) break;
        if (ai.memoryTypeIndex == memory.memoryTypeCount) throw std::runtime_error("No suitable memory type");
        VkDeviceMemory m {}; check(vkAllocateMemory(device, &ai, nullptr, &m)); return m;
    };

    struct Image { VkImage image {}; VkImageView view {}; VkDeviceMemory memory {}; };
    auto makeImage = [&]() {
        Image image;
        VkImageCreateInfo ci { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        ci.extent = { kSide, kSide, 1 }; ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        check(vkCreateImage(device, &ci, nullptr, &image.image));
        VkMemoryRequirements req {}; vkGetImageMemoryRequirements(device, image.image, &req);
        image.memory = allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkBindImageMemory(device, image.image, image.memory, 0));
        VkImageViewCreateInfo vi { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = image.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ci.format;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        check(vkCreateImageView(device, &vi, nullptr, &image.view));
        return image;
    };
    // frame, meter, exposure, proxy, model, original, result, keep
    enum { Frame, Meter, Exposure, Proxy, Model, Original, Result, Keep, ImageCount };
    std::array<Image, ImageCount> images {};
    for (auto& image : images) image = makeImage();

    struct Buffer { VkBuffer buffer {}; VkDeviceMemory memory {}; void* mapped {}; };
    auto buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage) {
        Buffer b; VkBufferCreateInfo ci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO }; ci.size = size; ci.usage = usage;
        check(vkCreateBuffer(device, &ci, nullptr, &b.buffer));
        VkMemoryRequirements req {}; vkGetBufferMemoryRequirements(device, b.buffer, &req);
        b.memory = allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkBindBufferMemory(device, b.buffer, b.memory, 0));
        check(vkMapMemory(device, b.memory, 0, size, 0, &b.mapped)); return b;
    };
    constexpr VkDeviceSize imageBytes = (VkDeviceSize) kSide * kSide * 16;
    auto readback = buffer(imageBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    constexpr uint32_t kMaxDispatches = 16;
    std::vector<Buffer> uniforms;
    for (uint32_t i = 0; i < kMaxDispatches; ++i) uniforms.push_back(buffer(sizeof(DlssNrConstants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT));

    std::array<VkDescriptorSetLayoutBinding, 8> bindings {};
    for (uint32_t i = 0; i < 8; ++i)
        bindings[i] = { i, i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER :
                          i < 5 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
                          i < 7 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER,
                        1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo lci { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.bindingCount = 8; lci.pBindings = bindings.data();
    VkDescriptorSetLayout layout {}; check(vkCreateDescriptorSetLayout(device, &lci, nullptr, &layout));
    VkPipelineLayoutCreateInfo plci { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1; plci.pSetLayouts = &layout;
    VkPipelineLayout pipelineLayout {}; check(vkCreatePipelineLayout(device, &plci, nullptr, &pipelineLayout));
    VkShaderModuleCreateInfo sci { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO }; sci.codeSize = bytes; sci.pCode = code.data();
    VkShaderModule shader {}; check(vkCreateShaderModule(device, &sci, nullptr, &shader));
    VkComputePipelineCreateInfo pci { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO }; pci.layout = pipelineLayout;
    pci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shader, "CSMain", nullptr };
    VkPipeline pipeline {}; check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline));
    VkSamplerCreateInfo si { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO }; si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler {}; check(vkCreateSampler(device, &si, nullptr, &sampler));
    VkDescriptorPoolSize sizes[] = { {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxDispatches}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * kMaxDispatches},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * kMaxDispatches}, {VK_DESCRIPTOR_TYPE_SAMPLER, kMaxDispatches} };
    VkDescriptorPoolCreateInfo dpci { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO }; dpci.maxSets = kMaxDispatches; dpci.poolSizeCount = 4; dpci.pPoolSizes = sizes;
    VkDescriptorPool pool {}; check(vkCreateDescriptorPool(device, &dpci, nullptr, &pool));
    VkCommandPoolCreateInfo cpci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; cpci.queueFamilyIndex = family;
    VkCommandPool commandPool {}; check(vkCreateCommandPool(device, &cpci, nullptr, &commandPool));
    const VkImageSubresourceRange range { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // One submission per scenario, so each reads back exactly what it wrote.
    struct Bind { uint32_t source, model, original, motion, target, keep; };
    struct Dispatch { DlssNrConstants constants; Bind bind; uint32_t groupsX, groupsY; };
    struct Clear { uint32_t image; VkClearColorValue value; };
    struct Fill { uint32_t image; uint32_t leftPixels; VkClearColorValue left, right; }; // split the frame at a column
    auto run = [&](const std::vector<Clear>& clears, const std::vector<Dispatch>& dispatches, uint32_t readImage) {
        if (dispatches.size() > kMaxDispatches) throw std::runtime_error("too many dispatches");
        check(vkResetDescriptorPool(device, pool, 0)); check(vkResetCommandPool(device, commandPool, 0));
        VkCommandBufferAllocateInfo cai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cai.commandPool = commandPool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        VkCommandBuffer cmd {}; check(vkAllocateCommandBuffers(device, &cai, &cmd));
        VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; check(vkBeginCommandBuffer(cmd, &begin));
        auto memoryBarrier = [&](VkAccessFlags src, VkAccessFlags dst) {
            VkMemoryBarrier b { VK_STRUCTURE_TYPE_MEMORY_BARRIER }; b.srcAccessMask = src; b.dstAccessMask = dst;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
        };
        static bool laidOut = false; // first submission moves every image to GENERAL, where it stays
        if (!laidOut)
        {
            for (auto& image : images)
            {
                VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image.image; barrier.subresourceRange = range;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            }
            laidOut = true;
        }
        for (const auto& c : clears) vkCmdClearColorImage(cmd, images[c.image].image, VK_IMAGE_LAYOUT_GENERAL, &c.value, 1, &range);
        memoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        for (uint32_t d = 0; d < dispatches.size(); ++d)
        {
            std::memcpy(uniforms[d].mapped, &dispatches[d].constants, sizeof(DlssNrConstants));
            VkDescriptorSetAllocateInfo dai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dai.descriptorPool = pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &layout;
            VkDescriptorSet set {}; check(vkAllocateDescriptorSets(device, &dai, &set));
            const Bind& b = dispatches[d].bind;
            // Production binding order: Source, Model, Original, Motion, Target, Keep, Sampler.
            const uint32_t order[6] = { b.source, b.model, b.original, b.motion, b.target, b.keep };
            VkDescriptorBufferInfo bi { uniforms[d].buffer, 0, sizeof(DlssNrConstants) };
            std::array<VkDescriptorImageInfo, 7> ii {}; std::array<VkWriteDescriptorSet, 8> writes {};
            for (uint32_t i = 0; i < 8; ++i)
            {
                writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; writes[i].dstSet = set;
                writes[i].dstBinding = i; writes[i].descriptorCount = 1; writes[i].descriptorType = bindings[i].descriptorType;
                if (!i) writes[i].pBufferInfo = &bi;
                else { ii[i - 1] = { sampler, i < 7 ? images[order[i - 1]].view : VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL }; writes[i].pImageInfo = &ii[i - 1]; }
            }
            vkUpdateDescriptorSets(device, 8, writes.data(), 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
            vkCmdDispatch(cmd, dispatches[d].groupsX, dispatches[d].groupsY, 1);
            memoryBarrier(VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
        }
        VkBufferImageCopy copy {}; copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { kSide, kSide, 1 };
        vkCmdCopyImageToBuffer(cmd, images[readImage].image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &copy);
        memoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        check(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO }; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
        check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); check(vkQueueWaitIdle(queue)); // standalone test only
        return (const float*) readback.mapped;
    };
    auto rgb = [](float v) { return VkClearColorValue { { v, v, v, 1.0f } }; };

    // ---- 1. the meter and the reduction ---------------------------------------------------------
    // Source luminance is dot(rgb, kLuma) and the weights sum to 1, so a grey of v has luminance v.
    auto exposureOf = [&](VkClearColorValue grey, float preExposure, float protection, bool percentile = false,
                          float low = 10.0f, float high = 90.0f) {
        std::vector<Clear> clears { { Frame, grey } };
        DlssNrConstants meter {}; meter.Mode = DlssNrMode_Meter; meter.Width = kGrid; meter.Height = kGrid; meter.MeterCopiesExposure = 0;
        DlssNrConstants reduce {}; reduce.Mode = DlssNrMode_AutoExposure; reduce.Width = 1; reduce.Height = 1;
        reduce.PreExposure = preExposure; reduce.ExposureSourceWidth = kFrame; reduce.ExposureSourceHeight = kFrame;
        reduce.AutoExposureShadowProtection = protection;
        SetAutoExposureMeter(reduce, percentile, low, high);
        std::vector<Dispatch> dispatches {
            { meter, { Frame, Frame, Frame, Frame, Meter, Keep }, kGrid, kGrid },
            { reduce, { Meter, Meter, Meter, Meter, Exposure, Keep }, 1, 1 } };
        return run(clears, dispatches, Exposure)[0];
    };

    std::cout << "automatic exposure (mode 3 + mode 13)\n";
    // grey 0.4, PreExposure 2 -> scene luminance 0.2 -> 0.18 / (0.2 * 0.82) = 1.0976
    expect("uniform 0.4, pre 2, protection 0", exposureOf(rgb(0.4f), 2.0f, 0.0f), 0.18f / (0.2f * 0.82f), 0.01f);
    expect("uniform 0.4, pre 2, protection 100", exposureOf(rgb(0.4f), 2.0f, 100.0f), 0.18f / (0.2f * 0.82f), 0.01f);
    // a missing PreExposure falls back to 1: 0.18 / (0.4 * 0.82)
    expect("uniform 0.4, pre 0 (missing -> 1)", exposureOf(rgb(0.4f), 0.0f, 0.0f), 0.18f / (0.4f * 0.82f), 0.01f);
    expect("percentile uniform 0.4, pre 2", exposureOf(rgb(0.4f), 2.0f, 0.0f, true), 0.18f / (0.2f * 0.82f), 0.02f);
    expect("percentile black frame (no reading)", exposureOf(rgb(0.0f), 1.0f, 0.0f, true), 0.0f, 0.01f);
    // a black frame is not a division by zero
    expect("black frame (0 = no reading; it was 1 before the shader kept the last value instead)", exposureOf(rgb(0.0f), 1.0f, 0.0f), 0.0f, 0.01f);

    // Half the frame 0.1 (top) and half 4.0 (bottom): clear the frame to 0.1, then overwrite the lower
    // half with a buffer-to-image copy of 4.0.
    {
        std::vector<float> bright((size_t) kSide * (kSide / 2) * 4);
        for (size_t i = 0; i < bright.size(); i += 4) { bright[i] = bright[i + 1] = bright[i + 2] = 4.0f; bright[i + 3] = 1.0f; }
        auto staging = buffer(bright.size() * sizeof(float), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        std::memcpy(staging.mapped, bright.data(), bright.size() * sizeof(float));

        auto splitExposure = [&](float protection, bool percentile = false, float low = 10.0f, float high = 90.0f) {
            check(vkResetDescriptorPool(device, pool, 0)); check(vkResetCommandPool(device, commandPool, 0));
            VkCommandBufferAllocateInfo cai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            cai.commandPool = commandPool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
            VkCommandBuffer cmd {}; check(vkAllocateCommandBuffers(device, &cai, &cmd));
            VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; check(vkBeginCommandBuffer(cmd, &begin));
            const VkClearColorValue dark = rgb(0.1f);
            vkCmdClearColorImage(cmd, images[Frame].image, VK_IMAGE_LAYOUT_GENERAL, &dark, 1, &range);
            VkMemoryBarrier b { VK_STRUCTURE_TYPE_MEMORY_BARRIER }; b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
            VkBufferImageCopy region {}; region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageOffset = { 0, (int32_t) (kSide / 2), 0 }; region.imageExtent = { kSide, kSide / 2, 1 };
            vkCmdCopyBufferToImage(cmd, staging.buffer, images[Frame].image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            DlssNrConstants meter {}; meter.Mode = DlssNrMode_Meter; meter.Width = kGrid; meter.Height = kGrid; meter.MeterCopiesExposure = 0;
            DlssNrConstants reduce {}; reduce.Mode = DlssNrMode_AutoExposure; reduce.Width = 1; reduce.Height = 1;
            reduce.PreExposure = 1.0f; reduce.ExposureSourceWidth = kFrame; reduce.ExposureSourceHeight = kFrame;
            reduce.AutoExposureShadowProtection = protection;
            SetAutoExposureMeter(reduce, percentile, low, high);
            const DlssNrConstants list[2] = { meter, reduce };
            const Bind binds[2] = { { Frame, Frame, Frame, Frame, Meter, Keep }, { Meter, Meter, Meter, Meter, Exposure, Keep } };
            const uint32_t groups[2] = { kGrid, 1 };
            for (uint32_t d = 0; d < 2; ++d)
            {
                std::memcpy(uniforms[d].mapped, &list[d], sizeof(DlssNrConstants));
                VkDescriptorSetAllocateInfo dai { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                dai.descriptorPool = pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &layout;
                VkDescriptorSet set {}; check(vkAllocateDescriptorSets(device, &dai, &set));
                const Bind& bd = binds[d]; const uint32_t order[6] = { bd.source, bd.model, bd.original, bd.motion, bd.target, bd.keep };
                VkDescriptorBufferInfo bi { uniforms[d].buffer, 0, sizeof(DlssNrConstants) };
                std::array<VkDescriptorImageInfo, 7> ii {}; std::array<VkWriteDescriptorSet, 8> writes {};
                for (uint32_t i = 0; i < 8; ++i)
                {
                    writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; writes[i].dstSet = set;
                    writes[i].dstBinding = i; writes[i].descriptorCount = 1; writes[i].descriptorType = bindings[i].descriptorType;
                    if (!i) writes[i].pBufferInfo = &bi;
                    else { ii[i - 1] = { sampler, i < 7 ? images[order[i - 1]].view : VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL }; writes[i].pImageInfo = &ii[i - 1]; }
                }
                vkUpdateDescriptorSets(device, 8, writes.data(), 0, nullptr);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
                vkCmdDispatch(cmd, groups[d], groups[d], 1);
                VkMemoryBarrier m { VK_STRUCTURE_TYPE_MEMORY_BARRIER }; m.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                m.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &m, 0, nullptr, 0, nullptr);
            }
            VkBufferImageCopy copy {}; copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { kSide, kSide, 1 };
            vkCmdCopyImageToBuffer(cmd, images[Exposure].image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &copy);
            VkMemoryBarrier h { VK_STRUCTURE_TYPE_MEMORY_BARRIER }; h.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; h.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &h, 0, nullptr, 0, nullptr);
            check(vkEndCommandBuffer(cmd));
            VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO }; submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
            check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); check(vkQueueWaitIdle(queue));
            return ((const float*) readback.mapped)[0];
        };
        // half 0.1, half 4.0, pre 1, no protection: mean 2.05 -> 0.18 / (2.05 * 0.82)
        expect("half 0.1 / half 4.0, protection 0", splitExposure(0.0f), 0.18f / (2.05f * 0.82f), 0.01f);
        // protection 100: reference log2 = (log2 0.1 + log2 4) / 2; the bright half is past the 1 EV knee
        // and compressed at 0.35; the dark half is not.
        const float reference = (std::log2(0.1f) + std::log2(4.0f)) * 0.5f;
        const float compressed = reference + 1.0f + ((std::log2(4.0f) - reference) - 1.0f) * 0.35f;
        const float protectedMean = (0.1f + std::exp2(compressed)) * 0.5f;
        expect("half 0.1 / half 4.0, protection 100", splitExposure(100.0f), 0.18f / (protectedMean * 0.82f), 0.01f);

        // The percentile meter (DlssNr_ExposureMeter.h): the log-average between two percentiles of the 4096 tiles.
        // Half the tiles at 0.1 and half at 4.0: the 10..90 window takes 1638 of each, so log2 luma is their mean.
        const float logMean = std::exp2(reference);
        expect("percentile 10..90, half 0.1 / half 4.0", splitExposure(0.0f, true, 10.0f, 90.0f), 0.18f / (logMean * 0.82f), 0.02f);
        expect("percentile 0..100, half 0.1 / half 4.0", splitExposure(0.0f, true, 0.0f, 100.0f), 0.18f / (logMean * 0.82f), 0.02f);
        // 0..30 sees only the dark half; 70..100 only the bright half.
        expect("percentile 0..30, half 0.1 / half 4.0", splitExposure(0.0f, true, 0.0f, 30.0f), 0.18f / (0.1f * 0.82f), 0.02f);
        expect("percentile 70..100, half 0.1 / half 4.0", splitExposure(0.0f, true, 70.0f, 100.0f), 0.18f / (4.0f * 0.82f), 0.02f);
        // The plain meter is untouched by the new fields: protection 0 still reads the arithmetic mean.
        expect("average meter unchanged", splitExposure(0.0f, false), 0.18f / (2.05f * 0.82f), 0.01f);
    }

    // ---- 2. the live white point --------------------------------------------------------------------
    // The resolve paints WhitePoint() into the compare wipe's divider column (result = the white point,
    // x3), so the white point the shader computes can be read straight off the frame.
    std::cout << "live white point (resolve, compare divider)\n";
    auto whitePoint = [&](float exposureValue, bool live, float sliderTrim, const std::vector<std::pair<float, float>>& anchors, bool preview) {
        DlssNrConstants c {};
        c.Mode = DlssNrMode_Resolve; c.Width = kFrame; c.Height = kFrame;
        c.Passthrough = 0; c.ApplyModel = 1; c.WhitePoint = 123.0f; // the CPU fallback
        c.CompareMode = 2; c.CompareSplit = 0.5f;
        c.UseExposureWhitePoint = live ? 1u : 0u; c.PreExposure = 2.0f;
        c.ExposureTrim = sliderTrim; c.ExposureTrimPreview = preview ? 1u : 0u;
        c.ExposureTrimAnchorCount = (uint32_t) anchors.size();
        float* pairs = &c.ExposureTrimAnchorExposure0;
        for (size_t i = 0; i < anchors.size(); ++i) { pairs[i * 2] = anchors[i].first; pairs[i * 2 + 1] = anchors[i].second; }
        const VkClearColorValue exposure { { exposureValue, 0, 0, 1 } };
        std::vector<Clear> clears { { Proxy, rgb(0.5f) }, { Model, rgb(0.5f) }, { Original, rgb(0.5f) }, { Exposure, exposure } };
        // proxy, model, original, exposure-in-the-motion-slot, target = Result, keep
        std::vector<Dispatch> dispatches { { c, { Proxy, Model, Original, Exposure, Result, Keep }, (kFrame + 7) / 8, (kFrame + 7) / 8 } };
        const float* pixels = run(clears, dispatches, Result);
        return pixels[((size_t) 100 * kSide + kFrame / 2) * 4]; // a row in the middle of the wipe's divider
    };
    // exposure 0.04, PreExposure 2 -> base white point 50
    expect("live, no anchors, Trim 2  (50 x 2)", whitePoint(0.04f, true, 2.0f, {}, false), 100.0f, 0.01f);
    expect("live is off: the CPU value", whitePoint(0.04f, false, 2.0f, {}, false), 123.0f, 0.01f);
    expect("live, exposure absent (0): the CPU value", whitePoint(0.0f, true, 2.0f, {}, false), 123.0f, 0.01f);
    expect("live, Trim clamps at 50x  (50 x 50)", whitePoint(0.04f, true, 500.0f, {}, false), 2500.0f, 0.01f);
    // anchors 50->2 and 200->8: at base 50 the Trim is 2, at 200 it is 8, at 100 (log midpoint) it is 4
    expect("live, anchors at 50 -> Trim 2  (50 x 2)", whitePoint(0.04f, true, 9.0f, { { 50.0f, 2.0f }, { 200.0f, 8.0f } }, false), 100.0f, 0.01f);
    expect("live, anchors at 100 -> Trim 4  (100 x 4)", whitePoint(0.02f, true, 9.0f, { { 50.0f, 2.0f }, { 200.0f, 8.0f } }, false), 400.0f, 0.01f);
    expect("live, anchors at 200 -> Trim 8  (200 x 8)", whitePoint(0.01f, true, 9.0f, { { 50.0f, 2.0f }, { 200.0f, 8.0f } }, false), 1600.0f, 0.01f);
    expect("live, anchors below the first: flat  (25 x 2)", whitePoint(0.08f, true, 9.0f, { { 50.0f, 2.0f }, { 200.0f, 8.0f } }, false), 50.0f, 0.01f);
    expect("live, anchors above the last: flat  (400 x 8)", whitePoint(0.005f, true, 9.0f, { { 50.0f, 2.0f }, { 200.0f, 8.0f } }, false), 3200.0f, 0.01f);
    expect("live, one anchor applies everywhere  (100 x 5)", whitePoint(0.02f, true, 9.0f, { { 400.0f, 5.0f } }, false), 500.0f, 0.01f);
    expect("live, preview ignores the anchors  (100 x slider 3)", whitePoint(0.02f, true, 3.0f, { { 50.0f, 2.0f }, { 200.0f, 8.0f } }, true), 300.0f, 0.01f);

    vkDestroyCommandPool(device, commandPool, nullptr); vkDestroyDescriptorPool(device, pool, nullptr);
    vkDestroySampler(device, sampler, nullptr); vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyShaderModule(device, shader, nullptr); vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, layout, nullptr);
    for (auto& image : images) { vkDestroyImageView(device, image.view, nullptr); vkDestroyImage(device, image.image, nullptr); vkFreeMemory(device, image.memory, nullptr); }
    for (auto& b : uniforms) { vkUnmapMemory(device, b.memory); vkDestroyBuffer(device, b.buffer, nullptr); vkFreeMemory(device, b.memory, nullptr); }
    vkUnmapMemory(device, readback.memory); vkDestroyBuffer(device, readback.buffer, nullptr); vkFreeMemory(device, readback.memory, nullptr);
    vkDestroyDevice(device, nullptr); vkDestroyInstance(instance, nullptr);
    std::cout << (failures ? "FAILED\n" : "PASS: automatic exposure and the live white point match the hand-worked values\n");
    return failures ? 1 : 0;
}
catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
