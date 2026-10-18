#include "pch.h"

#include "DlssNr_ExposureScan_Vk.h"
#include "DlssNr_ExposureScanCore.h"
#include "DlssNr_ExposureScanShape.h"

#include <Config.h>

#include <detours/detours.h>

#include <atomic>
#include <cstring>
#include <string>
#include <unordered_map>

// See DlssNr_ExposureScan_Vk.h. The D3D12 file (DlssNr_ExposureScan.cpp) has the reasons for the shape filter and the
// readback ring; this is the same on Vulkan's terms.

static_assert(VK_FORMAT_R16_SFLOAT == DlssNrExposureScanShape::kVkR16Sfloat);
static_assert(VK_FORMAT_R16G16_SFLOAT == DlssNrExposureScanShape::kVkR16G16Sfloat);
static_assert(VK_FORMAT_R32_SFLOAT == DlssNrExposureScanShape::kVkR32Sfloat);
static_assert(VK_FORMAT_R32G32_SFLOAT == DlssNrExposureScanShape::kVkR32G32Sfloat);

namespace
{
using DlssNr::ExposureScan::Api;
using DlssNr::ExposureScan::Core::Tracked;
namespace Core = DlssNr::ExposureScan::Core;

constexpr VkDeviceSize kStride = 16; // a ring entry: wider than any value read, a multiple of every texel size

PFN_vkCreateBuffer o_vkCreateBuffer = nullptr;
PFN_vkDestroyBuffer o_vkDestroyBuffer = nullptr;
PFN_vkBindBufferMemory o_vkBindBufferMemory = nullptr;
PFN_vkBindBufferMemory2 o_vkBindBufferMemory2 = nullptr;
PFN_vkBindBufferMemory2 o_vkBindBufferMemory2KHR = nullptr;
PFN_vkBindImageMemory o_vkBindImageMemory = nullptr;
PFN_vkBindImageMemory2 o_vkBindImageMemory2 = nullptr;
PFN_vkBindImageMemory2 o_vkBindImageMemory2KHR = nullptr;
PFN_vkFreeMemory o_vkFreeMemory = nullptr;

// A candidate-shaped buffer or image made by the game, waiting for its memory: it is adopted at the binding, when it
// can first be copied. Under Core::Mutex().
struct Pending
{
    VkDevice device = VK_NULL_HANDLE;
    std::string shape;
    unsigned int bytes = 4;
    bool isBuffer = true;
    uint32_t format = 0;
};

std::unordered_map<uint64_t, Pending> g_pending;

// Pending plus adopted Vulkan candidates: the destroy, free and bind hooks take the lock only when there is any.
std::atomic<int> g_watched = 0;

// The adopted images' handles and the layout the game's last recorded barrier left each in (0: none seen), by slot, for
// the barrier hooks, which run for every barrier the game records and must not take the lock.
std::atomic<uint64_t> g_imageHandle[Core::kMaxCandidates] = {};
std::atomic<uint32_t> g_imageLayout[Core::kMaxCandidates] = {};
std::atomic<int> g_images = 0;

// The readback ring: one host-visible buffer of kSlots x kMaxCandidates entries, on the device NR records on. Touched
// only on the thread that records NR (its creation outside the lock: its own create/bind calls reach the hooks).
struct Ring
{
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const unsigned char* mapped = nullptr;
    uint32_t generation[Core::kSlots][Core::kMaxCandidates] = {};
    bool copied[Core::kSlots][Core::kMaxCandidates] = {};
    unsigned long long frames = 0;
    bool failed = false;
};

Ring g_ring;

// The ring's own lock: NR on Vulkan (Tick) and the native frame source (Record) record on different threads, and a game
// can move between them. Taken before Core::Mutex(), never inside it.
std::mutex g_ringMutex;

uint64_t Key(const void* handle) { return (uint64_t) (uintptr_t) handle; }

void CountWatchedLocked()
{
    int alive = 0;

    for (const Tracked& t : Core::ListLocked())
        alive += t.alive && t.api == Api::Vulkan ? 1 : 0;

    g_watched = alive + (int) g_pending.size();
}

void DropVkLocked(size_t slot)
{
    std::vector<Tracked>& list = Core::ListLocked();

    if (slot >= list.size() || !list[slot].alive || list[slot].api != Api::Vulkan)
        return;

    if (!list[slot].isBuffer && slot < Core::kMaxCandidates && g_imageHandle[slot].exchange(0) != 0)
        g_images--;

    Core::DropLocked(slot);
}

void DropHandleLocked(const void* handle)
{
    g_pending.erase(Key(handle));
    std::vector<Tracked>& list = Core::ListLocked();

    for (size_t i = 0; i < list.size(); ++i)
    {
        if (list[i].alive && list[i].api == Api::Vulkan && list[i].handle == handle)
            DropVkLocked(i);
    }

    CountWatchedLocked();
}

// A pending candidate got its memory: from here it can be copied
void AdoptBoundLocked(const void* handle, VkDeviceMemory memory)
{
    const auto it = g_pending.find(Key(handle));

    if (it == g_pending.end())
        return;

    const Pending p = it->second;
    g_pending.erase(it);

    const int slot = Core::AdoptLocked(Api::Vulkan, (void*) handle, p.device, p.shape, p.bytes, p.isBuffer, p.format);

    if (slot >= 0)
    {
        Core::ListLocked()[slot].memory = Key(memory);

        if (!p.isBuffer && slot < (int) Core::kMaxCandidates)
        {
            g_imageLayout[slot] = 0;
            g_imageHandle[slot] = Key(handle);
            g_images++;
        }
    }

    CountWatchedLocked();
}

VkResult VKAPI_CALL hkCreateBuffer(VkDevice device, const VkBufferCreateInfo* info,
                                   const VkAllocationCallbacks* allocator, VkBuffer* buffer)
{
    // What a shader writes and holds a number: made copyable (TRANSFER_SRC) for the scan
    const bool candidate =
        info != nullptr &&
        (info->usage & (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)) != 0 &&
        DlssNrExposureScanShape::SmallBuffer(info->size) && Core::Active();
    VkBufferCreateInfo local {};

    if (candidate && (info->usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) == 0)
    {
        local = *info;
        local.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        info = &local;
    }

    const VkResult result = o_vkCreateBuffer(device, info, allocator, buffer);

    if (candidate && result == VK_SUCCESS && buffer != nullptr && *buffer != VK_NULL_HANDLE)
    {
        std::lock_guard<std::mutex> lock(Core::Mutex());
        Core::ExaminedLocked();

        if (Core::FromGameLocked(_ReturnAddress()))
        {
            g_pending[Key(*buffer)] = { device, "buffer, " + std::to_string((unsigned long long) info->size) + " bytes",
                                        4, true, 0 };
            CountWatchedLocked();
        }
    }

    return result;
}

void VKAPI_CALL hkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* allocator)
{
    if (buffer != VK_NULL_HANDLE && g_watched.load(std::memory_order_relaxed) > 0)
    {
        std::lock_guard<std::mutex> lock(Core::Mutex());
        DropHandleLocked(buffer);
    }

    o_vkDestroyBuffer(device, buffer, allocator);
}

VkResult VKAPI_CALL hkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
                                       VkDeviceSize memoryOffset)
{
    const VkResult result = o_vkBindBufferMemory(device, buffer, memory, memoryOffset);

    if (result == VK_SUCCESS && g_watched.load(std::memory_order_relaxed) > 0)
    {
        std::lock_guard<std::mutex> lock(Core::Mutex());
        AdoptBoundLocked(buffer, memory);
    }

    return result;
}

VkResult BindBuffers2(PFN_vkBindBufferMemory2 original, VkDevice device, uint32_t count,
                      const VkBindBufferMemoryInfo* infos)
{
    const VkResult result = original(device, count, infos);

    if (result == VK_SUCCESS && infos != nullptr && g_watched.load(std::memory_order_relaxed) > 0)
    {
        std::lock_guard<std::mutex> lock(Core::Mutex());

        for (uint32_t i = 0; i < count; ++i)
            AdoptBoundLocked(infos[i].buffer, infos[i].memory);
    }

    return result;
}

VkResult VKAPI_CALL hkBindBufferMemory2(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo* infos)
{
    return BindBuffers2(o_vkBindBufferMemory2, device, count, infos);
}

VkResult VKAPI_CALL hkBindBufferMemory2KHR(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo* infos)
{
    return BindBuffers2(o_vkBindBufferMemory2KHR, device, count, infos);
}

VkResult VKAPI_CALL hkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset)
{
    const VkResult result = o_vkBindImageMemory(device, image, memory, memoryOffset);

    if (result == VK_SUCCESS && g_watched.load(std::memory_order_relaxed) > 0)
    {
        std::lock_guard<std::mutex> lock(Core::Mutex());
        AdoptBoundLocked(image, memory);
    }

    return result;
}

VkResult BindImages2(PFN_vkBindImageMemory2 original, VkDevice device, uint32_t count,
                     const VkBindImageMemoryInfo* infos)
{
    const VkResult result = original(device, count, infos);

    if (result == VK_SUCCESS && infos != nullptr && g_watched.load(std::memory_order_relaxed) > 0)
    {
        std::lock_guard<std::mutex> lock(Core::Mutex());

        for (uint32_t i = 0; i < count; ++i)
            AdoptBoundLocked(infos[i].image, infos[i].memory);
    }

    return result;
}

VkResult VKAPI_CALL hkBindImageMemory2(VkDevice device, uint32_t count, const VkBindImageMemoryInfo* infos)
{
    return BindImages2(o_vkBindImageMemory2, device, count, infos);
}

VkResult VKAPI_CALL hkBindImageMemory2KHR(VkDevice device, uint32_t count, const VkBindImageMemoryInfo* infos)
{
    return BindImages2(o_vkBindImageMemory2KHR, device, count, infos);
}

void VKAPI_CALL hkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* allocator)
{
    // A candidate whose memory goes cannot be copied any more, even if the game keeps the handle
    if (memory != VK_NULL_HANDLE && g_watched.load(std::memory_order_relaxed) > 0)
    {
        std::lock_guard<std::mutex> lock(Core::Mutex());
        std::vector<Tracked>& list = Core::ListLocked();

        for (size_t i = 0; i < list.size(); ++i)
        {
            if (list[i].alive && list[i].api == Api::Vulkan && list[i].memory == Key(memory))
                DropVkLocked(i);
        }

        CountWatchedLocked();
    }

    o_vkFreeMemory(device, memory, allocator);
}

struct ScanHook
{
    const char* name;
    PFN_vkVoidFunction hook;
    PFN_vkVoidFunction* original;
};

const ScanHook kHooks[] = {
    { "vkCreateBuffer", (PFN_vkVoidFunction) hkCreateBuffer, (PFN_vkVoidFunction*) &o_vkCreateBuffer },
    { "vkDestroyBuffer", (PFN_vkVoidFunction) hkDestroyBuffer, (PFN_vkVoidFunction*) &o_vkDestroyBuffer },
    { "vkBindBufferMemory", (PFN_vkVoidFunction) hkBindBufferMemory, (PFN_vkVoidFunction*) &o_vkBindBufferMemory },
    { "vkBindBufferMemory2", (PFN_vkVoidFunction) hkBindBufferMemory2, (PFN_vkVoidFunction*) &o_vkBindBufferMemory2 },
    { "vkBindBufferMemory2KHR", (PFN_vkVoidFunction) hkBindBufferMemory2KHR,
      (PFN_vkVoidFunction*) &o_vkBindBufferMemory2KHR },
    { "vkBindImageMemory", (PFN_vkVoidFunction) hkBindImageMemory, (PFN_vkVoidFunction*) &o_vkBindImageMemory },
    { "vkBindImageMemory2", (PFN_vkVoidFunction) hkBindImageMemory2, (PFN_vkVoidFunction*) &o_vkBindImageMemory2 },
    { "vkBindImageMemory2KHR", (PFN_vkVoidFunction) hkBindImageMemory2KHR,
      (PFN_vkVoidFunction*) &o_vkBindImageMemory2KHR },
    { "vkFreeMemory", (PFN_vkVoidFunction) hkFreeMemory, (PFN_vkVoidFunction*) &o_vkFreeMemory },
};

void DestroyRing()
{
    if (g_ring.device != VK_NULL_HANDLE)
    {
        if (g_ring.mapped != nullptr)
            vkUnmapMemory(g_ring.device, g_ring.memory);

        if (g_ring.buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(g_ring.device, g_ring.buffer, nullptr);

        if (g_ring.memory != VK_NULL_HANDLE)
            vkFreeMemory(g_ring.device, g_ring.memory, nullptr);
    }

    g_ring = Ring {};
}

bool EnsureRing(VkDevice device, VkPhysicalDevice physical)
{
    if (g_ring.device == device && g_ring.mapped != nullptr)
        return true;

    if (g_ring.failed && g_ring.device == device)
        return false;

    DestroyRing();
    g_ring.device = device;

    VkBufferCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = kStride * Core::kSlots * Core::kMaxCandidates;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkMemoryRequirements needs {};
    VkPhysicalDeviceMemoryProperties props {};
    uint32_t type = UINT32_MAX;

    if (vkCreateBuffer(device, &info, nullptr, &g_ring.buffer) == VK_SUCCESS)
    {
        vkGetBufferMemoryRequirements(device, g_ring.buffer, &needs);
        vkGetPhysicalDeviceMemoryProperties(physical, &props);
        const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        for (uint32_t i = 0; i < props.memoryTypeCount && type == UINT32_MAX; ++i)
        {
            if ((needs.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted)
                type = i;
        }
    }

    void* mapped = nullptr;

    if (type != UINT32_MAX)
    {
        VkMemoryAllocateInfo alloc {};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = needs.size;
        alloc.memoryTypeIndex = type;

        if (vkAllocateMemory(device, &alloc, nullptr, &g_ring.memory) == VK_SUCCESS &&
            vkBindBufferMemory(device, g_ring.buffer, g_ring.memory, 0) == VK_SUCCESS &&
            vkMapMemory(device, g_ring.memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS)
        {
            std::memset(mapped, 0, (size_t) info.size);
        }
    }

    if (mapped == nullptr)
    {
        LOG_WARN("DLSS-NR exposure scan (Vulkan): could not make the readback buffer, values are not read");
        DestroyRing();
        g_ring.device = device;
        g_ring.failed = true;
        return false;
    }

    g_ring.mapped = (const unsigned char*) mapped;
    return true;
}

void GlobalBarrier(VkCommandBuffer cmd, VkPipelineStageFlags from, VkAccessFlags fromAccess, VkPipelineStageFlags to,
                   VkAccessFlags toAccess)
{
    VkMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = fromAccess;
    barrier.dstAccessMask = toAccess;
    vkCmdPipelineBarrier(cmd, from, to, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags fromAccess,
                  VkAccessFlags toAccess)
{
    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = fromAccess;
    barrier.dstAccessMask = toAccess;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
}

// A layout an image can be copied from, after a transition when it is not GENERAL. Not one never seen (0), and not
// UNDEFINED or PREINITIALIZED: there is no picture in those to keep.
bool CopyableLayout(uint32_t layout)
{
    return layout != 0 && layout != VK_IMAGE_LAYOUT_UNDEFINED && layout != VK_IMAGE_LAYOUT_PREINITIALIZED;
}

void NoteLayout(VkImage image, VkImageLayout layout)
{
    const uint64_t key = Key(image);

    for (size_t i = 0; i < Core::kMaxCandidates; ++i)
    {
        if (g_imageHandle[i].load(std::memory_order_relaxed) == key)
            g_imageLayout[i].store((uint32_t) layout, std::memory_order_relaxed);
    }
}

// Read back the values copied kSlots rounds ago, then copy this round's: the shared part of Tick and Record
void ReadAndCopy(VkCommandBuffer cmd, VkDevice device, VkPhysicalDevice physical)
{
    if (cmd == VK_NULL_HANDLE || device == VK_NULL_HANDLE || g_watched.load(std::memory_order_relaxed) == 0)
        return;

    std::lock_guard<std::mutex> ringLock(g_ringMutex);

    if (!EnsureRing(device, physical))
        return;

    std::lock_guard<std::mutex> lock(Core::Mutex());
    Core::HeldScope held;
    std::vector<Tracked>& list = Core::ListLocked();
    const unsigned int slot = (unsigned int) (g_ring.frames % Core::kSlots);

    // Host-coherent memory, kSlots rounds old: retired by the time the slot comes round again
    for (size_t i = 0; i < list.size() && i < Core::kMaxCandidates; ++i)
    {
        if (!g_ring.copied[slot][i])
            continue;

        g_ring.copied[slot][i] = false;
        Core::ConsumeLocked(i, g_ring.generation[slot][i], g_ring.mapped + (slot * Core::kMaxCandidates + i) * kStride);
    }

    bool any = false;

    for (size_t i = 0; i < list.size() && i < Core::kMaxCandidates; ++i)
    {
        const Tracked& t = list[i];

        if (!t.alive || t.api != Api::Vulkan || t.device != (void*) device)
            continue;

        const VkDeviceSize offset = (slot * Core::kMaxCandidates + i) * kStride;
        const uint32_t layout = t.isBuffer ? 0 : g_imageLayout[i].load(std::memory_order_relaxed);

        if (!t.isBuffer && !CopyableLayout(layout))
            continue;

        // The game's writes before this point, whatever wrote them, are visible to the copies
        if (!any)
        {
            GlobalBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            any = true;
        }

        if (t.isBuffer)
        {
            const VkBufferCopy region { 0, offset, t.bytes };
            vkCmdCopyBuffer(cmd, (VkBuffer) t.handle, g_ring.buffer, 1, &region);
        }
        else
        {
            const VkImage image = (VkImage) t.handle;
            const VkImageLayout from = (VkImageLayout) layout;
            const bool general = from == VK_IMAGE_LAYOUT_GENERAL;

            if (!general)
                ImageBarrier(cmd, image, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT);

            VkBufferImageCopy region {};
            region.bufferOffset = offset;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { 1, 1, 1 };
            vkCmdCopyImageToBuffer(cmd, image, general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   g_ring.buffer, 1, &region);

            // Back to where the game left it, for whatever it records next
            if (!general)
                ImageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, from, VK_ACCESS_TRANSFER_READ_BIT,
                             VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        }

        g_ring.generation[slot][i] = t.generation;
        g_ring.copied[slot][i] = true;
    }

    if (any)
        GlobalBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                      VK_ACCESS_HOST_READ_BIT);

    g_ring.frames++;
}
} // namespace

namespace DlssNr
{
namespace ExposureScanVk
{
PFN_vkVoidFunction GetProcAddr(PFN_vkVoidFunction original, const char* name)
{
    if (original == nullptr || name == nullptr || !Core::Wanted())
        return nullptr;

    for (const auto& hook : kHooks)
    {
        if (std::strcmp(name, hook.name) == 0)
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
    if (vulkanModule == nullptr || o_vkCreateBuffer != nullptr || !Core::Wanted())
        return;

    for (const auto& hook : kHooks)
        *hook.original = (PFN_vkVoidFunction) GetProcAddress(vulkanModule, hook.name);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    for (const auto& hook : kHooks)
    {
        if (*hook.original != nullptr)
            DetourAttach(&(PVOID&) *hook.original, (PVOID) hook.hook);
    }

    if (const auto result = DetourTransactionCommit(); result != NO_ERROR)
    {
        LOG_ERROR("DLSS-NR exposure scan (Vulkan): hooking the buffer and memory functions failed ({:X})",
                  (UINT) result);

        for (const auto& hook : kHooks)
            *hook.original = nullptr;

        return;
    }

    LOG_INFO("DLSS-NR exposure scan (Vulkan): watching the game's buffer and image creation");
}

bool WantsImage(const VkImageCreateInfo& info)
{
    return info.imageType == VK_IMAGE_TYPE_2D && info.samples == VK_SAMPLE_COUNT_1_BIT && info.extent.depth == 1 &&
           (info.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0 &&
           DlssNrExposureScanShape::VkFloatFormat((uint32_t) info.format).bytes != 0 &&
           DlssNrExposureScanShape::SmallTexture(info.extent.width, info.extent.height) && Core::Active();
}

void OnImageCreated(VkDevice device, const VkImageCreateInfo& info, VkImage image, void* caller)
{
    if (image == VK_NULL_HANDLE)
        return;

    std::lock_guard<std::mutex> lock(Core::Mutex());
    Core::ExaminedLocked();

    if (!Core::FromGameLocked(caller))
        return;

    const DlssNrExposureScanShape::Format format = DlssNrExposureScanShape::VkFloatFormat((uint32_t) info.format);
    g_pending[Key(image)] = { device,
                              std::to_string(info.extent.width) + "x" + std::to_string(info.extent.height) + " " +
                                  format.name,
                              format.bytes, false, (uint32_t) info.format };
    CountWatchedLocked();
}

void OnImageDestroyed(VkImage image)
{
    if (image == VK_NULL_HANDLE || g_watched.load(std::memory_order_relaxed) == 0)
        return;

    std::lock_guard<std::mutex> lock(Core::Mutex());
    DropHandleLocked(image);
}

void NoteBarriers(uint32_t count, const VkImageMemoryBarrier* barriers)
{
    if (barriers == nullptr || g_images.load(std::memory_order_relaxed) == 0)
        return;

    for (uint32_t i = 0; i < count; ++i)
        NoteLayout(barriers[i].image, barriers[i].newLayout);
}

void NoteBarriers2(const VkDependencyInfo* info)
{
    if (info == nullptr || info->pImageMemoryBarriers == nullptr || g_images.load(std::memory_order_relaxed) == 0)
        return;

    for (uint32_t i = 0; i < info->imageMemoryBarrierCount; ++i)
        NoteLayout(info->pImageMemoryBarriers[i].image, info->pImageMemoryBarriers[i].newLayout);
}

void Tick(VkCommandBuffer cmd, VkDevice device, VkPhysicalDevice physical, const ExposureScan::FrameExposure& frame)
{
    if (!Core::Active())
        return;

    ReadAndCopy(cmd, device, physical);

    std::lock_guard<std::mutex> lock(Core::Mutex());
    Core::EndFrameLocked(frame);
}

void Record(VkCommandBuffer cmd, VkDevice device, VkPhysicalDevice physical)
{
    if (Core::Active())
        ReadAndCopy(cmd, device, physical);
}

void OnDeviceDestroyed(VkDevice device)
{
    if (device == VK_NULL_HANDLE)
        return;

    {
        std::lock_guard<std::mutex> lock(Core::Mutex());
        std::vector<Tracked>& list = Core::ListLocked();

        for (size_t i = 0; i < list.size(); ++i)
        {
            if (list[i].alive && list[i].api == Api::Vulkan && list[i].device == (void*) device)
                DropVkLocked(i);
        }

        for (auto it = g_pending.begin(); it != g_pending.end();)
            it = it->second.device == device ? g_pending.erase(it) : std::next(it);

        CountWatchedLocked();
    }

    // Outside the scan's lock: the ring's own destroy and free calls reach the hooks
    std::lock_guard<std::mutex> ringLock(g_ringMutex);

    if (g_ring.device == device)
        DestroyRing();
}
} // namespace ExposureScanVk
} // namespace DlssNr
