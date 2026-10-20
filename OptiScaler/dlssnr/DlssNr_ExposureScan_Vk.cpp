#include "pch.h"

#include "DlssNr_ExposureScan_Vk.h"
#include "DlssNr_ExposureScanCore.h"
#include "DlssNr_ExposureScanShape.h"

#include <Config.h>
#include <hooks/Vulkan_Hooks.h>

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
    uint64_t size = 0; // bytes for a buffer, texels for an image
};

std::unordered_map<uint64_t, Pending> g_pending;

// Every candidate-shaped image and every 1x1 image the game makes (an exposure image is 1x1; RDR2 makes thousands of
// other small textures, so not every small one), so a handed exposure image that is not a candidate can be told why:
// the filter, a full list, or a binding the scan does not see. Under Core::Mutex().
struct Seen
{
    VkDevice device = VK_NULL_HANDLE; // its entries go with the device (OnDeviceDestroyed)
    VkImageCreateInfo info {};
    bool wanted = false;  // candidate-shaped (WantsImage)
    bool notGame = false; // ... but made by a module that is not the game's (Core::FromGameLocked)
    bool capped = false;  // ... and bound while the list was full
};

constexpr size_t kMaxSeen = 4096;
std::unordered_map<uint64_t, Seen> g_seen;
std::atomic<int> g_seenCount = 0;

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

// The scan's lock, unless this thread already holds it (Core::Held): a destroy or free reached from inside the scan's
// own API calls -- dxvk or vkd3d-proton making Vulkan calls for a D3D11 or D3D12 call the scan made under the lock --
// must still drop what it destroys, and this thread owns the state already.
std::unique_lock<std::mutex> LockUnlessHeld()
{
    return Core::Held() ? std::unique_lock<std::mutex>() : std::unique_lock<std::mutex>(Core::Mutex());
}

// The native frame source submits its copy in (with the scan's copies, Record) itself, after the game's own frame: a
// game that frees a candidate once its own frame fence passed can still have that copy queued. Each round's submit is
// noted (Submitted) and a dropped candidate's last such copy is waited for before the game's destroy or free goes on.
// The NR pass (Tick) and VkwDx12 record into the game's own command buffer, which the game's own fences cover. Under
// Core::Mutex().
struct Round
{
    unsigned long long frame = ~0ull;
    VkDevice device = VK_NULL_HANDLE;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    uint64_t value = 0;
};

Round g_rounds[Core::kSlots];
unsigned long long g_recordedRound = ~0ull;               // the last round Record copied in, until Submitted
unsigned long long g_lastCopy[Core::kMaxCandidates] = {}; // the ring round of each slot's last copy
bool g_lastCopyOwnSubmit[Core::kMaxCandidates] = {};      // ... and whether the frame source submitted it

void WaitForLastCopyLocked(size_t slot, VkDevice device)
{
    if (slot >= Core::kMaxCandidates || !g_lastCopyOwnSubmit[slot])
        return;

    g_lastCopyOwnSubmit[slot] = false;
    const unsigned long long frame = g_lastCopy[slot];
    const Round& round = g_rounds[frame % Core::kSlots];

    if (round.frame != frame || round.semaphore == VK_NULL_HANDLE || round.device != device)
        return;

    static PFN_vkWaitSemaphores wait = nullptr;
    static VkDevice waitDevice = VK_NULL_HANDLE;

    if (waitDevice != device)
    {
        waitDevice = device;
        wait = (PFN_vkWaitSemaphores) VulkanHooks::GetDeviceProcAddr(device, "vkWaitSemaphores");

        if (wait == nullptr)
            wait = (PFN_vkWaitSemaphores) VulkanHooks::GetDeviceProcAddr(device, "vkWaitSemaphoresKHR");
    }

    if (wait == nullptr)
        return;

    VkSemaphoreWaitInfo info {};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    info.semaphoreCount = 1;
    info.pSemaphores = &round.semaphore;
    info.pValues = &round.value;

    if (wait(device, &info, 100'000'000ull) != VK_SUCCESS)
    {
        static bool said = false;

        if (!said)
        {
            said = true;
            LOG_WARN(
                "DLSS-NR exposure scan (Vulkan): a copy of a resource the game is freeing did not finish in 100 ms");
        }
    }
}

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

    WaitForLastCopyLocked(slot, (VkDevice) list[slot].device);
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

    const int slot = Core::AdoptLocked(Api::Vulkan, (void*) handle, p.device, p.shape, p.bytes, p.isBuffer, p.format,
                                       p.size, DropVkLocked);

    if (slot < 0 && !p.isBuffer)
    {
        if (const auto seen = g_seen.find(Key(handle)); seen != g_seen.end())
            seen->second.capped = true;
    }

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
        DlssNrExposureScanShape::SmallBuffer(info->size) && !Core::Held() && Core::Active();
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
                                        4, true, 0, (uint64_t) info->size };
            CountWatchedLocked();
        }
    }

    return result;
}

void VKAPI_CALL hkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* allocator)
{
    if (buffer != VK_NULL_HANDLE && g_watched.load(std::memory_order_relaxed) > 0)
    {
        const auto lock = LockUnlessHeld();
        DropHandleLocked(buffer);
    }

    o_vkDestroyBuffer(device, buffer, allocator);
}

VkResult VKAPI_CALL hkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
                                       VkDeviceSize memoryOffset)
{
    const VkResult result = o_vkBindBufferMemory(device, buffer, memory, memoryOffset);

    // A binding made from inside the scan's own calls is never the game's candidate
    if (result == VK_SUCCESS && g_watched.load(std::memory_order_relaxed) > 0 && !Core::Held())
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

    if (result == VK_SUCCESS && infos != nullptr && g_watched.load(std::memory_order_relaxed) > 0 && !Core::Held())
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

    if (result == VK_SUCCESS && g_watched.load(std::memory_order_relaxed) > 0 && !Core::Held())
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

    if (result == VK_SUCCESS && infos != nullptr && g_watched.load(std::memory_order_relaxed) > 0 && !Core::Held())
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
        const auto lock = LockUnlessHeld();
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

// The layout a candidate image is left in by a barrier the game records. One handing the image to another queue family
// makes it unknown: a copy on this queue would read it without the ownership transfer. A slot dropped and taken again
// while this ran must not keep the old image's layout: the handle is checked again after the store.
void NoteLayout(VkImage image, VkImageLayout layout, uint32_t srcFamily, uint32_t dstFamily)
{
    const uint64_t key = Key(image);
    const bool otherQueue =
        srcFamily != dstFamily && srcFamily != VK_QUEUE_FAMILY_IGNORED && dstFamily != VK_QUEUE_FAMILY_IGNORED;

    for (size_t i = 0; i < Core::kMaxCandidates; ++i)
    {
        if (g_imageHandle[i].load(std::memory_order_relaxed) != key)
            continue;

        g_imageLayout[i].store(otherQueue ? 0u : (uint32_t) layout, std::memory_order_relaxed);

        if (g_imageHandle[i].load(std::memory_order_relaxed) != key)
            g_imageLayout[i].store(0u, std::memory_order_relaxed);
    }
}

// Read back the values copied kSlots rounds ago, then copy this round's: the shared part of Tick and Record
void ReadAndCopy(VkCommandBuffer cmd, VkDevice device, VkPhysicalDevice physical, bool ownSubmit)
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

        if (!t.isBuffer && g_imageLayout[i].load(std::memory_order_relaxed) != (uint32_t) VK_IMAGE_LAYOUT_GENERAL)
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
            // Only in GENERAL, where a copy needs no transition: a layout noted from recorded barriers is the last one
            // RECORDED, which another thread's or a later frame's recording can make wrong, and a transition from a
            // wrong layout can corrupt a compressed image. Exposure images written by compute sit in GENERAL.
            VkBufferImageCopy region {};
            region.bufferOffset = offset;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { 1, 1, 1 };
            vkCmdCopyImageToBuffer(cmd, (VkImage) t.handle, VK_IMAGE_LAYOUT_GENERAL, g_ring.buffer, 1, &region);
        }

        g_ring.generation[slot][i] = t.generation;
        g_ring.copied[slot][i] = true;
        g_lastCopy[i] = g_ring.frames;
        g_lastCopyOwnSubmit[i] = ownSubmit;
    }

    if (ownSubmit && any)
        g_recordedRound = g_ring.frames;

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
           info.mipLevels == 1 && info.arrayLayers == 1 && (info.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0 &&
           DlssNrExposureScanShape::VkFloatFormat((uint32_t) info.format).bytes != 0 &&
           DlssNrExposureScanShape::SmallTexture(info.extent.width, info.extent.height) && Core::Active();
}

void OnImageCreated(VkDevice device, const VkImageCreateInfo& info, VkImage image, void* caller)
{
    if (image == VK_NULL_HANDLE || Core::Held())
        return;

    std::lock_guard<std::mutex> lock(Core::Mutex());
    Core::ExaminedLocked();

    if (!Core::FromGameLocked(caller))
    {
        if (const auto seen = g_seen.find(Key(image)); seen != g_seen.end())
            seen->second.notGame = true;

        return;
    }

    const DlssNrExposureScanShape::Format format = DlssNrExposureScanShape::VkFloatFormat((uint32_t) info.format);
    g_pending[Key(image)] = { device,
                              std::to_string(info.extent.width) + "x" + std::to_string(info.extent.height) + " " +
                                  format.name,
                              format.bytes, false, (uint32_t) info.format,
                              (uint64_t) info.extent.width * info.extent.height };
    CountWatchedLocked();
}

void RememberImage(VkDevice device, const VkImageCreateInfo& info, VkImage image, bool wanted)
{
    if (image == VK_NULL_HANDLE || Core::Held() || !Core::Active())
        return;

    if (!wanted && (info.extent.width != 1 || info.extent.height != 1))
        return;

    std::lock_guard<std::mutex> lock(Core::Mutex());

    if (g_seen.size() >= kMaxSeen)
    {
        static bool said = false;

        if (!said)
        {
            said = true;
            LOG_INFO("DLSS-NR exposure scan (Vulkan): {} candidate-shaped or 1x1 images remembered, no more for the "
                     "handed exposure check",
                     kMaxSeen);
        }

        return;
    }

    Seen seen;
    seen.device = device;
    seen.info = info;
    seen.info.pNext = nullptr;
    seen.info.pQueueFamilyIndices = nullptr;
    seen.wanted = wanted;
    g_seen[Key(image)] = seen;
    g_seenCount = (int) g_seen.size();
}

void OnImageDestroyed(VkImage image)
{
    if (image == VK_NULL_HANDLE ||
        (g_watched.load(std::memory_order_relaxed) == 0 && g_seenCount.load(std::memory_order_relaxed) == 0))
        return;

    const auto lock = LockUnlessHeld();

    if (g_seen.erase(Key(image)) != 0)
        g_seenCount = (int) g_seen.size();

    DropHandleLocked(image);
}

void NoteBarriers(uint32_t count, const VkImageMemoryBarrier* barriers)
{
    if (barriers == nullptr || g_images.load(std::memory_order_relaxed) == 0)
        return;

    for (uint32_t i = 0; i < count; ++i)
        NoteLayout(barriers[i].image, barriers[i].newLayout, barriers[i].srcQueueFamilyIndex,
                   barriers[i].dstQueueFamilyIndex);
}

void NoteBarriers2(const VkDependencyInfo* info)
{
    if (info == nullptr || info->pImageMemoryBarriers == nullptr || g_images.load(std::memory_order_relaxed) == 0)
        return;

    for (uint32_t i = 0; i < info->imageMemoryBarrierCount; ++i)
    {
        const VkImageMemoryBarrier2& b = info->pImageMemoryBarriers[i];
        NoteLayout(b.image, b.newLayout, b.srcQueueFamilyIndex, b.dstQueueFamilyIndex);
    }
}

void Tick(VkCommandBuffer cmd, VkDevice device, VkPhysicalDevice physical, const ExposureScan::FrameExposure& frame)
{
    if (!Core::Active())
        return;

    ReadAndCopy(cmd, device, physical, false);

    std::lock_guard<std::mutex> lock(Core::Mutex());
    Core::EndFrameLocked(frame);
}

void Record(VkCommandBuffer cmd, VkDevice device, VkPhysicalDevice physical, bool ownSubmit)
{
    if (Core::Active())
        ReadAndCopy(cmd, device, physical, ownSubmit);
}

void Submitted(VkDevice device, VkSemaphore timeline, uint64_t value)
{
    if (timeline == VK_NULL_HANDLE || g_watched.load(std::memory_order_relaxed) == 0)
        return;

    std::lock_guard<std::mutex> lock(Core::Mutex());

    if (g_recordedRound == ~0ull)
        return;

    g_rounds[g_recordedRound % Core::kSlots] = { g_recordedRound, device, timeline, value };
    g_recordedRound = ~0ull;
}

void NoteHanded(VkImage exposure, uint32_t width, uint32_t height, VkFormat format, uint32_t mip, uint32_t layer)
{
    if (exposure == VK_NULL_HANDLE || !Core::Active() || Core::Held())
        return;

    {
        std::lock_guard<std::mutex> lock(Core::Mutex());
        Core::NoteHandedLocked(Api::Vulkan, exposure);
    }

    ExplainHanded(exposure, width, height, format, mip, layer);
}

void ExplainHanded(VkImage exposure, uint32_t width, uint32_t height, VkFormat format, uint32_t mip, uint32_t layer)
{
    if (exposure == VK_NULL_HANDLE || !Core::Active() || Core::Held())
        return;

    std::lock_guard<std::mutex> lock(Core::Mutex());

    // Once, a while after NR started: why the handed image is not a candidate, when it is not
    static unsigned int calls = 0;
    static bool said = false;

    if (said || ++calls < 600)
        return;

    said = true;

    for (const Tracked& t : Core::ListLocked())
    {
        if (t.alive && t.api == Api::Vulkan && t.handle == (const void*) exposure)
            return;
    }

    const auto seen = g_seen.find(Key(exposure));

    if (seen == g_seen.end())
    {
        LOG_WARN("DLSS-NR exposure scan (Vulkan): the handed exposure image (view {}x{}, format {}, mip {}, layer {}) "
                 "was not seen made while the scan watched ({} remembered{}): made before the scan started, by a path "
                 "it does not hook, or neither candidate-shaped nor 1x1",
                 width, height, (int) format, mip, layer, g_seen.size(),
                 g_seen.size() >= kMaxSeen ? ", the list is full, so it may have been made after that" : "");
        return;
    }

    const VkImageCreateInfo& i = seen->second.info;
    const char* why = !seen->second.wanted ? "the filter rejects it"
                      : seen->second.notGame
                          ? "it is candidate-shaped, but made by a module that is not the game's"
                      : seen->second.capped
                          ? "it is candidate-shaped, but the candidate list was already full when it was bound"
                          : "it is candidate-shaped, but its memory binding was never seen";

    LOG_WARN("DLSS-NR exposure scan (Vulkan): the handed exposure image is not a candidate: {} (type {}, {}x{}x{}, "
             "format {}, mips {}, layers {}, samples {}, usage {:#x}, flags {:#x}; the view: mip {}, layer {})",
             why, (int) i.imageType, i.extent.width, i.extent.height, i.extent.depth, (int) i.format, i.mipLevels,
             i.arrayLayers, (int) i.samples, (unsigned int) i.usage, (unsigned int) i.flags, mip, layer);
}

void OnDeviceDestroyed(VkDevice device)
{
    if (device == VK_NULL_HANDLE)
        return;

    {
        const auto lock = LockUnlessHeld();
        std::vector<Tracked>& list = Core::ListLocked();

        for (size_t i = 0; i < list.size(); ++i)
        {
            if (list[i].alive && list[i].api == Api::Vulkan && list[i].device == (void*) device)
                DropVkLocked(i);
        }

        for (auto it = g_pending.begin(); it != g_pending.end();)
            it = it->second.device == device ? g_pending.erase(it) : std::next(it);

        // A device can go without its images being destroyed one by one: theirs must not fill the remembered list
        for (auto it = g_seen.begin(); it != g_seen.end();)
            it = it->second.device == device ? g_seen.erase(it) : std::next(it);

        g_seenCount = (int) g_seen.size();
        CountWatchedLocked();
    }

    // Outside the scan's lock: the ring's own destroy and free calls reach the hooks
    std::lock_guard<std::mutex> ringLock(g_ringMutex);

    if (g_ring.device == device)
        DestroyRing();
}
} // namespace ExposureScanVk
} // namespace DlssNr
