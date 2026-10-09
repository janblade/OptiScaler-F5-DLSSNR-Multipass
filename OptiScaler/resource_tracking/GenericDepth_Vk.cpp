#include "pch.h"

#include "GenericDepth_Vk.h"
#include "GenericDepth_VkEvents.h"

#include <Config.h>
#include <dlssnr/DlssNr_NativeMode.h>

#include <native/DepthFinderCore.h>
#include <native/SharedFrameVk.h>

#include <detours/detours.h>

#include <imgui/imgui.h>

#include <atomic>
#include <format>
#include <mutex>
#include <vector>

// See GenericDepth_Vk.h. The D3D11 sibling is GenericDepth_Dx11.cpp; both feed native::DepthFinderCore.

namespace
{

using GenericDepthVkEvents::Copy;
using GenericDepthVkEvents::ImageInfo;
using GenericDepthVkEvents::IsDepthFormat;

native::DepthFinderCore g_core;
GenericDepthVkEvents::Tracker g_tracker(g_core);

std::atomic<bool> g_installed { false };
bool g_installFailed = false;
bool g_addCopyUsage = false; // [DlssNr] NativeDepthVkCopyUsage, read at install

// The device the copies are made on: the one created last, then the one the game presents with.
std::atomic<VkDevice> g_device { VK_NULL_HANDLE };
std::atomic<VkPhysicalDevice> g_physical { VK_NULL_HANDLE };

// Whether a depth format can be copied out at all (optimal tiling, TRANSFER_SRC), asked once per format.
std::mutex g_formatMutex;
std::vector<std::pair<VkFormat, bool>> g_formatCopyable;

bool FormatCopyable(VkFormat format)
{
    std::lock_guard lock(g_formatMutex);

    for (const auto& [known, copyable] : g_formatCopyable)
    {
        if (known == format)
            return copyable;
    }

    VkFormatProperties properties {};
    const VkPhysicalDevice physical = g_physical.load();

    if (physical != VK_NULL_HANDLE)
        vkGetPhysicalDeviceFormatProperties(physical, format, &properties);

    const bool copyable = (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0;
    g_formatCopyable.emplace_back(format, copyable);
    return copyable;
}

// ---- the finder's own buffers: one per frame in flight, rotated at each present -------------------------------------

struct CopyBuffer
{
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint64_t size = 0;
    bool taken = false; // written in its frame
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
};

// A buffer replaced while a command buffer recorded earlier may still write it: destroyed a few presents later.
struct Retired
{
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint64_t after = 0;
};

constexpr int kSlots = 3;
constexpr uint64_t kRetireAfterPresents = 8;

std::mutex g_copyMutex;
CopyBuffer g_slots[kSlots];
int g_write = 0;  // the slot this frame's copies go into
int g_read = -1;  // the slot of the frame being presented, -1 none
uint64_t g_presents = 0;
std::vector<Retired> g_retired;
std::atomic<const char*> g_notReadable { nullptr };
uint64_t g_copies = 0;

void RetireLocked(CopyBuffer& slot)
{
    if (slot.buffer != VK_NULL_HANDLE)
        g_retired.push_back({ slot.device, slot.buffer, slot.memory, g_presents + kRetireAfterPresents });

    slot = CopyBuffer {};
}

void DestroyRetiredLocked(bool all, VkDevice onlyDevice = VK_NULL_HANDLE)
{
    std::erase_if(g_retired,
                  [&](const Retired& r)
                  {
                      if (onlyDevice != VK_NULL_HANDLE && r.device != onlyDevice)
                          return false;

                      if (!all && g_presents < r.after)
                          return false;

                      vkDestroyBuffer(r.device, r.buffer, nullptr);
                      vkFreeMemory(r.device, r.memory, nullptr);
                      return true;
                  });
}

// Makes `slot` hold at least `size` bytes on the current device.
bool EnsureBufferLocked(CopyBuffer& slot, uint64_t size)
{
    const VkDevice device = g_device.load();

    if (slot.buffer != VK_NULL_HANDLE && slot.size >= size && slot.device == device)
        return true;

    RetireLocked(slot);

    if (device == VK_NULL_HANDLE)
        return false;

    VkBufferCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &info, nullptr, &slot.buffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements requirements {};
    vkGetBufferMemoryRequirements(device, slot.buffer, &requirements);

    VkPhysicalDeviceMemoryProperties memory {};
    vkGetPhysicalDeviceMemoryProperties(g_physical.load(), &memory);

    uint32_t type = UINT32_MAX;

    for (uint32_t i = 0; i < memory.memoryTypeCount && type == UINT32_MAX; ++i)
    {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = i;
    }

    VkMemoryAllocateInfo allocate {};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;

    if (type == UINT32_MAX || vkAllocateMemory(device, &allocate, nullptr, &slot.memory) != VK_SUCCESS ||
        vkBindBufferMemory(device, slot.buffer, slot.memory, 0) != VK_SUCCESS)
    {
        if (slot.memory != VK_NULL_HANDLE)
            vkFreeMemory(device, slot.memory, nullptr);

        vkDestroyBuffer(device, slot.buffer, nullptr);
        slot = CopyBuffer {};
        return false;
    }

    slot.device = device;
    slot.size = size;
    return true;
}

// Records the copy the tracker asked for into the game's command buffer.
void Record(VkCommandBuffer cmd, const Copy& copy)
{
    if (!copy.take)
        return;

    ImageInfo info;

    if (!g_tracker.FindImage(copy.image, &info))
        return;

    if (info.samples != VK_SAMPLE_COUNT_1_BIT)
    {
        g_notReadable = "the scene's depth is multisampled, which cannot be copied out";
        return;
    }

    if ((info.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0)
    {
        g_notReadable = g_addCopyUsage ? "the scene's depth was made before OptiScaler could make it copyable"
                                       : "NativeDepthVkCopyUsage is off, so the game's depth cannot be copied";
        return;
    }

    native::VkDepthCopyFormat format;

    if (!native::VkDepthCopyFormatOf(info.format, &format))
        return;

    const uint64_t size = (uint64_t) native::DepthRowPitch(info.width, format.bytes) * info.height;

    std::lock_guard lock(g_copyMutex);
    CopyBuffer& slot = g_slots[g_write];

    if (!EnsureBufferLocked(slot, size))
    {
        g_notReadable = "the finder's copy buffer could not be made";
        return;
    }

    native::RecordDepthToBuffer(cmd, copy.image, info.format, copy.layout, info.width, info.height, slot.buffer);
    slot.taken = true;
    slot.format = info.format;
    slot.width = info.width;
    slot.height = info.height;
    g_notReadable = nullptr;

    if (++g_copies == 1)
        LOG_INFO("Depth finder (Vulkan): first depth copy taken at \"{}\", {}x{}, format {}, layout {}", copy.where,
                 info.width, info.height, (int) info.format, (int) copy.layout);
}

bool Watching() { return g_installed.load(std::memory_order_relaxed) && g_core.Active(); }

// ---- the resource hooks -----------------------------------------------------------------------------------------------

PFN_vkCreateImage o_vkCreateImage = nullptr;
PFN_vkDestroyImage o_vkDestroyImage = nullptr;
PFN_vkCreateImageView o_vkCreateImageView = nullptr;
PFN_vkDestroyImageView o_vkDestroyImageView = nullptr;
PFN_vkCreateFramebuffer o_vkCreateFramebuffer = nullptr;
PFN_vkDestroyFramebuffer o_vkDestroyFramebuffer = nullptr;
PFN_vkCreateRenderPass o_vkCreateRenderPass = nullptr;
PFN_vkCreateRenderPass2 o_vkCreateRenderPass2 = nullptr;
PFN_vkCreateRenderPass2KHR o_vkCreateRenderPass2KHR = nullptr;
PFN_vkDestroyRenderPass o_vkDestroyRenderPass = nullptr;

VkResult VKAPI_CALL hkCreateImage(VkDevice device, const VkImageCreateInfo* info, const VkAllocationCallbacks* allocator,
                                  VkImage* image)
{
    const bool watch = g_installed.load(std::memory_order_relaxed) && info != nullptr && IsDepthFormat(info->format);
    VkImageCreateInfo local {};

    // Depth for rendering, single-sampled, not yet copyable: made copyable so the finder can copy the scene's.
    if (watch && g_addCopyUsage && (info->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
        info->samples == VK_SAMPLE_COUNT_1_BIT && (info->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0 &&
        FormatCopyable(info->format))
    {
        local = *info;
        local.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info = &local;
    }

    const VkResult result = o_vkCreateImage(device, info, allocator, image);

    if (watch && result == VK_SUCCESS && image != nullptr)
    {
        g_tracker.OnImage(*image, { info->format, info->extent.width, info->extent.height, info->samples,
                                    info->usage });
    }

    return result;
}

void VKAPI_CALL hkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* allocator)
{
    if (g_installed.load(std::memory_order_relaxed) && image != VK_NULL_HANDLE)
        g_tracker.OnImageDestroyed(image);

    o_vkDestroyImage(device, image, allocator);
}

VkResult VKAPI_CALL hkCreateImageView(VkDevice device, const VkImageViewCreateInfo* info,
                                      const VkAllocationCallbacks* allocator, VkImageView* view)
{
    const VkResult result = o_vkCreateImageView(device, info, allocator, view);

    if (g_installed.load(std::memory_order_relaxed) && result == VK_SUCCESS && info != nullptr && view != nullptr &&
        IsDepthFormat(info->format))
    {
        g_core.Counters().depthViewsCreated.fetch_add(1, std::memory_order_relaxed);
        g_tracker.OnView(*view, info->image);
    }

    return result;
}

void VKAPI_CALL hkDestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks* allocator)
{
    if (g_installed.load(std::memory_order_relaxed) && view != VK_NULL_HANDLE)
        g_tracker.OnViewDestroyed(view);

    o_vkDestroyImageView(device, view, allocator);
}

VkResult VKAPI_CALL hkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* info,
                                        const VkAllocationCallbacks* allocator, VkFramebuffer* framebuffer)
{
    const VkResult result = o_vkCreateFramebuffer(device, info, allocator, framebuffer);

    if (g_installed.load(std::memory_order_relaxed) && result == VK_SUCCESS && info != nullptr &&
        framebuffer != nullptr)
    {
        std::vector<VkImageView> views;

        // An imageless framebuffer names no views: they come with each begin.
        if ((info->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) == 0 && info->pAttachments != nullptr)
            views.assign(info->pAttachments, info->pAttachments + info->attachmentCount);

        g_tracker.OnFramebuffer(*framebuffer, std::move(views));
    }

    return result;
}

void VKAPI_CALL hkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer,
                                     const VkAllocationCallbacks* allocator)
{
    if (g_installed.load(std::memory_order_relaxed) && framebuffer != VK_NULL_HANDLE)
        g_tracker.OnFramebufferDestroyed(framebuffer);

    o_vkDestroyFramebuffer(device, framebuffer, allocator);
}

VkResult VKAPI_CALL hkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* info,
                                       const VkAllocationCallbacks* allocator, VkRenderPass* renderPass)
{
    const VkResult result = o_vkCreateRenderPass(device, info, allocator, renderPass);

    if (g_installed.load(std::memory_order_relaxed) && result == VK_SUCCESS && info != nullptr &&
        renderPass != nullptr)
    {
        GenericDepthVkEvents::RenderPassInfo pass;

        for (uint32_t i = 0; i < info->attachmentCount; ++i)
        {
            const auto& a = info->pAttachments[i];
            pass.attachments.push_back({ a.format, a.loadOp, a.initialLayout, a.finalLayout });
        }

        for (uint32_t i = 0; i < info->subpassCount; ++i)
        {
            const auto* depth = info->pSubpasses[i].pDepthStencilAttachment;

            if (depth != nullptr && depth->attachment != VK_ATTACHMENT_UNUSED)
                pass.subpasses.push_back({ (int32_t) depth->attachment, depth->layout });
            else
                pass.subpasses.push_back({});
        }

        g_tracker.OnRenderPass(*renderPass, std::move(pass));
    }

    return result;
}

GenericDepthVkEvents::RenderPassInfo FromCreateInfo2(const VkRenderPassCreateInfo2* info)
{
    GenericDepthVkEvents::RenderPassInfo pass;

    for (uint32_t i = 0; i < info->attachmentCount; ++i)
    {
        const auto& a = info->pAttachments[i];
        pass.attachments.push_back({ a.format, a.loadOp, a.initialLayout, a.finalLayout });
    }

    for (uint32_t i = 0; i < info->subpassCount; ++i)
    {
        const auto* depth = info->pSubpasses[i].pDepthStencilAttachment;

        if (depth != nullptr && depth->attachment != VK_ATTACHMENT_UNUSED)
            pass.subpasses.push_back({ (int32_t) depth->attachment, depth->layout });
        else
            pass.subpasses.push_back({});
    }

    return pass;
}

VkResult VKAPI_CALL hkCreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* info,
                                        const VkAllocationCallbacks* allocator, VkRenderPass* renderPass)
{
    const VkResult result = o_vkCreateRenderPass2(device, info, allocator, renderPass);

    if (g_installed.load(std::memory_order_relaxed) && result == VK_SUCCESS && info != nullptr &&
        renderPass != nullptr)
        g_tracker.OnRenderPass(*renderPass, FromCreateInfo2(info));

    return result;
}

VkResult VKAPI_CALL hkCreateRenderPass2KHR(VkDevice device, const VkRenderPassCreateInfo2* info,
                                           const VkAllocationCallbacks* allocator, VkRenderPass* renderPass)
{
    const VkResult result = o_vkCreateRenderPass2KHR(device, info, allocator, renderPass);

    if (g_installed.load(std::memory_order_relaxed) && result == VK_SUCCESS && info != nullptr &&
        renderPass != nullptr)
        g_tracker.OnRenderPass(*renderPass, FromCreateInfo2(info));

    return result;
}

void VKAPI_CALL hkDestroyRenderPass(VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks* allocator)
{
    if (g_installed.load(std::memory_order_relaxed) && renderPass != VK_NULL_HANDLE)
        g_tracker.OnRenderPassDestroyed(renderPass);

    o_vkDestroyRenderPass(device, renderPass, allocator);
}

// One entry of the resource hooks: its name, our function and the original's slot.
struct ResourceHook
{
    const char* name;
    PFN_vkVoidFunction hook;
    PFN_vkVoidFunction* original;
};

const ResourceHook kResourceHooks[] = {
    { "vkCreateImage", (PFN_vkVoidFunction) hkCreateImage, (PFN_vkVoidFunction*) &o_vkCreateImage },
    { "vkDestroyImage", (PFN_vkVoidFunction) hkDestroyImage, (PFN_vkVoidFunction*) &o_vkDestroyImage },
    { "vkCreateImageView", (PFN_vkVoidFunction) hkCreateImageView, (PFN_vkVoidFunction*) &o_vkCreateImageView },
    { "vkDestroyImageView", (PFN_vkVoidFunction) hkDestroyImageView, (PFN_vkVoidFunction*) &o_vkDestroyImageView },
    { "vkCreateFramebuffer", (PFN_vkVoidFunction) hkCreateFramebuffer, (PFN_vkVoidFunction*) &o_vkCreateFramebuffer },
    { "vkDestroyFramebuffer", (PFN_vkVoidFunction) hkDestroyFramebuffer,
      (PFN_vkVoidFunction*) &o_vkDestroyFramebuffer },
    { "vkCreateRenderPass", (PFN_vkVoidFunction) hkCreateRenderPass, (PFN_vkVoidFunction*) &o_vkCreateRenderPass },
    { "vkCreateRenderPass2", (PFN_vkVoidFunction) hkCreateRenderPass2, (PFN_vkVoidFunction*) &o_vkCreateRenderPass2 },
    { "vkCreateRenderPass2KHR", (PFN_vkVoidFunction) hkCreateRenderPass2KHR,
      (PFN_vkVoidFunction*) &o_vkCreateRenderPass2KHR },
    { "vkDestroyRenderPass", (PFN_vkVoidFunction) hkDestroyRenderPass, (PFN_vkVoidFunction*) &o_vkDestroyRenderPass },
};

} // namespace

namespace GenericDepthVk
{

bool Installed() { return g_installed.load(); }
bool InstallFailed() { return g_installFailed; }

void OnDevice(VkDevice device, VkPhysicalDevice physical)
{
    if (device == VK_NULL_HANDLE)
        return;

    g_device = device;
    g_physical = physical;

    if (g_installed.load() || !Config::Instance()->DlssNrNativeDepthFinder.value_or_default())
        return;

    if (o_vkCreateImage == nullptr || o_vkCreateRenderPass == nullptr || o_vkCreateFramebuffer == nullptr)
    {
        LOG_WARN("Depth finder (Vulkan): the image and render pass functions are not hooked, not installed");
        g_installFailed = true;
        return;
    }

    g_addCopyUsage = Config::Instance()->DlssNrNativeDepthVkCopyUsage.value_or_default();
    g_core.Start([](const std::string& line) { LOG_INFO("{}", line); });
    g_installFailed = false;
    g_installed = true;

    LOG_INFO("Depth finder (Vulkan): observing the game's depth images, after {} frames of warm-up; it stands down if "
             "the game makes an upscaler call. Depth images {} copyable.",
             Config::Instance()->DlssNrNativeDepthWarmupFrames.value_or_default(),
             g_addCopyUsage ? "are made" : "are NOT made (NativeDepthVkCopyUsage=false)");
}

void OnDeviceDestroyed(VkDevice device)
{
    std::lock_guard lock(g_copyMutex);

    for (auto& slot : g_slots)
    {
        if (slot.device == device)
            RetireLocked(slot);
    }

    DestroyRetiredLocked(true, device);
    g_read = -1;

    if (g_device.load() == device)
        g_device = VK_NULL_HANDLE;
}

PFN_vkVoidFunction GetProcAddr(PFN_vkVoidFunction original, const char* name)
{
    if (original == nullptr || name == nullptr)
        return nullptr;

    for (const auto& hook : kResourceHooks)
    {
        if (strcmp(name, hook.name) == 0)
        {
            if (*hook.original == nullptr)
                *hook.original = original;

            return hook.hook;
        }
    }

    return nullptr;
}

void Hook(HMODULE vulkanModule)
{
    if (vulkanModule == nullptr || o_vkCreateImage != nullptr)
        return;

    for (const auto& hook : kResourceHooks)
        *hook.original = (PFN_vkVoidFunction) GetProcAddress(vulkanModule, hook.name);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    for (const auto& hook : kResourceHooks)
    {
        if (*hook.original != nullptr)
            DetourAttach(&(PVOID&) *hook.original, (PVOID) hook.hook);
    }

    if (const auto result = DetourTransactionCommit(); result != NO_ERROR)
    {
        LOG_ERROR("Depth finder (Vulkan): hooking the image and render pass functions failed ({:X})", (UINT) result);

        for (const auto& hook : kResourceHooks)
            *hook.original = nullptr;
    }
}

void BeginCommandBuffer(VkCommandBuffer cmd, const VkCommandBufferBeginInfo* info)
{
    if (!g_installed.load(std::memory_order_relaxed))
        return;

    const bool continues = info != nullptr && (info->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) &&
                           info->pInheritanceInfo != nullptr;
    g_tracker.Begin(cmd, continues);
}

void EndCommandBuffer(VkCommandBuffer cmd)
{
    if (Watching())
        g_tracker.End(cmd);
}

void ForgetCommandBuffers(uint32_t count, const VkCommandBuffer* cmds)
{
    if (!g_installed.load(std::memory_order_relaxed) || cmds == nullptr)
        return;

    for (uint32_t i = 0; i < count; ++i)
        g_tracker.Forget(cmds[i]);
}

void BeginRenderPass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info)
{
    if (!Watching() || info == nullptr)
        return;

    GenericDepthVkEvents::PassBegin begin;
    begin.renderPass = info->renderPass;
    begin.framebuffer = info->framebuffer;
    begin.clears = info->pClearValues;
    begin.clearCount = info->clearValueCount;
    begin.areaWidth = info->renderArea.extent.width;

    for (auto* node = (const VkBaseInStructure*) info->pNext; node != nullptr; node = node->pNext)
    {
        if (node->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO)
        {
            const auto* views = (const VkRenderPassAttachmentBeginInfo*) node;
            begin.views = views->pAttachments;
            begin.viewCount = views->attachmentCount;
        }
    }

    Record(cmd, g_tracker.BeginRenderPass(cmd, begin));
}

void NextSubpass(VkCommandBuffer cmd)
{
    if (Watching())
        g_tracker.NextSubpass(cmd);
}

void EndRenderPass(VkCommandBuffer cmd)
{
    if (Watching())
        Record(cmd, g_tracker.EndRenderPass(cmd));
}

void BeginRendering(VkCommandBuffer cmd, const VkRenderingInfo* info)
{
    if (!Watching() || info == nullptr)
        return;

    GenericDepthVkEvents::RenderingBegin begin;
    begin.areaWidth = info->renderArea.extent.width;
    begin.resuming = (info->flags & VK_RENDERING_RESUMING_BIT) != 0;
    begin.suspending = (info->flags & VK_RENDERING_SUSPENDING_BIT) != 0;

    if (info->pDepthAttachment != nullptr)
    {
        begin.depthView = info->pDepthAttachment->imageView;
        begin.layout = info->pDepthAttachment->imageLayout;
        begin.loadOp = info->pDepthAttachment->loadOp;
        begin.clearDepth = info->pDepthAttachment->clearValue.depthStencil.depth;
    }

    Record(cmd, g_tracker.BeginRendering(cmd, begin));
}

void EndRendering(VkCommandBuffer cmd)
{
    if (Watching())
        Record(cmd, g_tracker.EndRendering(cmd));
}

void ClearDepthStencilImage(VkCommandBuffer cmd, VkImage image, VkImageLayout layout,
                            const VkClearDepthStencilValue* value, uint32_t rangeCount,
                            const VkImageSubresourceRange* ranges)
{
    if (!Watching() || value == nullptr || ranges == nullptr)
        return;

    for (uint32_t i = 0; i < rangeCount; ++i)
    {
        if (ranges[i].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)
        {
            Record(cmd, g_tracker.ClearImage(cmd, image, layout, value->depth));
            return;
        }
    }
}

void ClearAttachments(VkCommandBuffer cmd, uint32_t count, const VkClearAttachment* attachments)
{
    if (!Watching() || attachments == nullptr)
        return;

    for (uint32_t i = 0; i < count; ++i)
    {
        if (attachments[i].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)
        {
            g_tracker.ClearBound(cmd, attachments[i].clearValue.depthStencil.depth);
            return;
        }
    }
}

void Draw(VkCommandBuffer cmd, uint32_t vertices, uint32_t instances)
{
    if (Watching())
        g_tracker.Draw(cmd, vertices, instances);
}

void Indirect(VkCommandBuffer cmd, uint32_t maxCount)
{
    if (Watching())
        g_tracker.Indirect(cmd, maxCount);
}

void Viewport(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkViewport* viewports)
{
    if (Watching() && first == 0 && count > 0 && viewports != nullptr)
        g_tracker.Viewport(cmd, viewports[0].width);
}

void ExecuteCommands(VkCommandBuffer primary, uint32_t count, const VkCommandBuffer* secondaries)
{
    if (Watching() && secondaries != nullptr)
        g_tracker.Execute(primary, secondaries, count);
}

void OnPresent(VkDevice device, uint32_t width, uint32_t height)
{
    if (!g_installed.load())
        return;

    if (device != VK_NULL_HANDLE)
        g_device = device;

    g_core.SetSnapshotsWanted(Config::Instance()->DlssNrNativeMotion.value_or_default());
    g_core.BeginPresent(width, height);
    const bool stoodDown = g_core.EndPresent(width, height,
                                             Config::Instance()->DlssNrNativeDepthWarmupFrames.value_or_default());

    std::lock_guard lock(g_copyMutex);
    ++g_presents;
    g_read = g_slots[g_write].taken ? g_write : -1;
    g_write = (g_write + 1) % kSlots;
    g_slots[g_write].taken = false;
    DestroyRetiredLocked(false);

    // Stood down: nothing is copied while the game calls an upscaler, so the buffers go until the finder wakes.
    if (stoodDown)
    {
        for (auto& slot : g_slots)
            RetireLocked(slot);

        g_read = -1;
    }

    static uint64_t noCopyStreak = 0;
    const auto pick = g_core.CurrentPick();

    if (g_core.Armed() && pick.valid && g_read < 0 && Config::Instance()->DlssNrNativeMotion.value_or_default())
    {
        if (++noCopyStreak % 300 == 0)
        {
            const char* reason = g_notReadable.load();
            LOG_WARN("Depth finder (Vulkan): {} frames wanting a copy with no copy taken. Picked {:X}; {} ({} requests "
                     "fell inside a pass)",
                     noCopyStreak, pick.id, reason != nullptr ? reason : "no request came at a point outside a pass",
                     g_tracker.DroppedRequests());
        }
    }
    else
        noCopyStreak = 0;
}

Snapshot BestSnapshot()
{
    std::lock_guard lock(g_copyMutex);
    const auto pick = g_core.CurrentPick();
    Snapshot snap;

    if (g_read >= 0 && pick.valid)
    {
        const CopyBuffer& slot = g_slots[g_read];
        native::VkDepthCopyFormat format;

        if (slot.buffer != VK_NULL_HANDLE && native::VkDepthCopyFormatOf(slot.format, &format))
        {
            snap.valid = true;
            snap.buffer = slot.buffer;
            snap.size = (uint64_t) native::DepthRowPitch(slot.width, format.bytes) * slot.height;
            snap.format = slot.format;
            snap.width = slot.width;
            snap.height = slot.height;
            snap.reversed = pick.reversed;
        }
    }

    return snap;
}

const char* NotReadableReason() { return g_notReadable.load(); }

void NoteUpscalerCall()
{
    // Noted with no finder installed too: the menu and the frame source must know the game has an upscaler either way.
    native::NoteGameUpscalerCall();

    if (g_installed.load(std::memory_order_relaxed))
        g_core.NoteUpscalerCall();
}

bool GameCallsUpscaler()
{
    return native::GameUpscalerCalledRecently() || (g_installed.load() && g_core.GameCallsUpscaler());
}

bool Armed() { return g_installed.load() && g_core.Armed(); }

void DrawStatus()
{
    const bool wanted = Config::Instance()->DlssNrNativeDepthFinder.value_or_default();

    switch (DlssNrNativeMode::FinderFor(wanted, g_installed.load(), g_installFailed))
    {
    case DlssNrNativeMode::Finder::Off:
        ImGui::TextDisabled("Depth: off; NR runs on motion only and does not stand aside for a game upscaler.");
        return;
    case DlssNrNativeMode::Finder::NeedsRestart:
        ImGui::TextDisabled("Depth: the finder needs a restart.");
        return;
    case DlssNrNativeMode::Finder::CouldNotStart:
        ImGui::TextDisabled("Depth: the finder could not start (see the log); NR runs on motion only.");
        return;
    case DlssNrNativeMode::Finder::Installed:
        break;
    }

    const auto pick = g_core.CurrentPick();
    const uint32_t warmup = Config::Instance()->DlssNrNativeDepthWarmupFrames.value_or_default();
    const char* untilRestart = wanted ? "" : " (finder off at next start)";
    const char* notReadable = g_notReadable.load();

    if (g_core.GameCallsUpscaler())
        ImGui::TextDisabled("Depth: stood down, the game is calling an upscaler. Turn it off in the game.");
    else if (!g_core.Armed())
        ImGui::TextDisabled("Depth: watching (%llu of %u frames)...%s",
                            (unsigned long long) (g_core.Presents() - g_core.WarmupStart()), warmup, untilRestart);
    else if (!pick.valid)
        ImGui::TextDisabled("Depth: none found yet; NR runs on motion only%s.", untilRestart);
    else if (notReadable != nullptr)
        ImGui::TextDisabled("Depth: picked %ux%u, but %s; NR runs on motion only.", pick.width, pick.height,
                            notReadable);
    else
        ImGui::Text("Depth: picked %ux%u%s%s", pick.width, pick.height, pick.reversed ? ", reversed-Z" : "",
                    untilRestart);

    if (ImGui::IsItemHovered() && pick.valid)
        ImGui::SetTooltip("Vulkan format %u. The log has the candidates (Depth finder lines).", pick.format);
    else if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The log has the candidates (Depth finder lines).");
}

void DrawAdvancedUi()
{
    auto* config = Config::Instance();
    bool finder = config->DlssNrNativeDepthFinder.value_or_default();

    if (ImGui::Checkbox("Use the game's depth (better quality; needs a restart)##depthfindervk", &finder))
        config->DlssNrNativeDepthFinder = finder;

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "%s", "Watches the game's render passes (Vulkan) and picks the scene's depth image, so NR gets depth\n"
                  "as well as motion. Optional: without it NR runs on motion only. It is also what notices the\n"
                  "game calling its own upscaler: without it, nothing here stands aside for that. To copy the\n"
                  "depth, the game's depth images are made copyable (NativeDepthVkCopyUsage in the ini).\n"
                  "Choosing a mode above turns it on, Off turns it off. Applies at the next start: save the\n"
                  "settings and restart the game.");
}

} // namespace GenericDepthVk
