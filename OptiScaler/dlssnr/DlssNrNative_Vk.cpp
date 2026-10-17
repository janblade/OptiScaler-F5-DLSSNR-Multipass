#include "pch.h"

// Reuse bottleneck and kernel-set detection on Vulkan (DlssNrVitReuse.h has the decision and why it is safe).
//
// On D3D12 the model creates and launches its CUDA kernels through NvAPI, which DlssNrNative.cpp wraps. On Vulkan the
// same model uses VK_NVX_binary_import: vkCreateCuFunctionNVX names each kernel (-> its ViT role and kernel set),
// vkCmdCuLaunchKernelNVX launches one kernel per call. The model resolves those through vkGetDeviceProcAddr, which
// OptiScaler already answers from its Vulkan hook table (VulkanwDx12_Hooks.cpp); those hooks call in here. Only
// launches inside our own evaluate bracket (DlssNrNative::BeginEvaluate / EndEvaluate around each pass, on the
// evaluating thread) are filtered; everything else on those functions -- the game's own DLSS SR -- goes out untouched.
//
// Reuse runs in copy mode on both kernel sets here (DlssNrVitReuse.h): with the run skipped, its kept result was overwritten
// between frames on Vulkan by something outside the model's own launches (see DlssNrFeature_Vk.cpp). So after a computed run our copy
// kernel (DlssNrVitCopyKernel.h, through vkCreateCuModuleNVX) saves its 2-D output into our own buffer (a device address, which the
// model's kernels use too), and a skipped run's last kernel is replaced by the restore, each between barriers.

#include "DlssNrNative.h"
#include "DlssNrVitReuse.h"
#include "DlssNrVitCopyKernel.h"

#include <hooks/Vulkan_Hooks.h>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace DlssNrNative
{
DlssNrVitReuse::Filter& VitFilter(); // DlssNrNative.cpp

namespace
{
// Never destroyed, like DlssNrNative.cpp's state: a destroy call can still arrive while the process unloads.
DlssNrVitReuse::Kernels<uint64_t>& Kernels()
{
    static auto* kernels = new DlssNrVitReuse::Kernels<uint64_t>;
    return *kernels;
}

std::atomic<bool> g_runSeen { false };

// Copy mode state. Like the rest, never destroyed while the process runs: recorded command buffers can still use it. A lost
// device forgets it (its objects went with the device).
// The NVX entry points are taken past OptiScaler's own hooks (Proc), so our module, kernel and launches stay out of the model's filter.
struct CopyKernel
{
    VkCuModuleNVX module = VK_NULL_HANDLE;
    VkCuFunctionNVX function = VK_NULL_HANDLE;
    PFN_vkCmdCuLaunchKernelNVX launch = nullptr;
    PFN_vkDestroyCuFunctionNVX destroyFunction = nullptr;
    PFN_vkDestroyCuModuleNVX destroyModule = nullptr;
    bool failed = false; // the load failed once: not tried again
};

struct CopyBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    uint64_t capacity = 0;
    uint64_t saved = 0, dst = 0; // what the last save held, and where it came from
};

struct CopyState
{
    std::mutex mutex;
    VkDevice device = VK_NULL_HANDLE; // of the evaluations in progress (VkEvaluateDevice)
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    std::map<VkDevice, CopyKernel> kernels;
    std::map<std::pair<VkDevice, const void*>, CopyBuffer> buffers;
    std::vector<std::pair<VkDevice, CopyBuffer>> retired; // outgrown, kept until their device is destroyed
};

CopyState& Copies()
{
    static auto* state = new CopyState;
    return *state;
}

// The run's last launch whose save comes right after it went out (VkLaunched), on the evaluating thread.
struct PendingSave
{
    bool pending = false;
    std::vector<unsigned char> params;
    uint64_t threads = 0;
    DlssNrVitReuse::KernelSet set = DlssNrVitReuse::KernelSet::Unknown;
    uint32_t extraCount = 1;
};

thread_local PendingSave t_save;
std::atomic<bool> g_copyLogged { false };

// A device function past OptiScaler's own hooks; through them only when that lookup is not there (our launches then pass the model's
// filter as a kernel of no role, which changes nothing).
PFN_vkVoidFunction Proc(VkDevice device, const char* name)
{
    const PFN_vkVoidFunction raw = VulkanHooks::GetDeviceProcAddr(device, name);
    return raw != nullptr ? raw : vkGetDeviceProcAddr(device, name);
}

bool LaunchParams(const VkCuLaunchInfoNVX& launch, const void*& params, size_t& size)
{
    return DlssNrVitReuse::ExtraParams(launch.pExtras, launch.extraCount, params, size);
}

uint64_t Threads(const VkCuLaunchInfoNVX& launch)
{
    return (uint64_t) launch.gridDimX * launch.gridDimY * launch.gridDimZ * launch.blockDimX * launch.blockDimY * launch.blockDimZ;
}

// The run's first launch, identified by its parameters and geometry (FNV-1a), so a moved or resized network is computed again.
uint64_t Sig(const VkCuLaunchInfoNVX& launch)
{
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&](const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i)
        {
            h ^= static_cast<const unsigned char*>(p)[i];
            h *= 1099511628211ull;
        }
    };
    const void* params = nullptr;
    size_t size = 0;

    if (LaunchParams(launch, params, size))
        mix(params, size);

    const uint32_t dims[6] = { launch.gridDimX, launch.gridDimY, launch.gridDimZ, launch.blockDimX, launch.blockDimY, launch.blockDimZ };
    mix(dims, sizeof dims);
    return h;
}

void Barrier(VkCommandBuffer commandBuffer)
{
    // The pipeline stage of an NVX launch is not specified, so the barrier covers all commands.
    VkMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0,
                         nullptr, 0, nullptr);
}

// The copy kernel of `device`, loaded once. Caller holds Copies().mutex.
const CopyKernel* Kernel(CopyState& c, VkDevice device)
{
    CopyKernel& k = c.kernels[device];

    if (k.function != VK_NULL_HANDLE || k.failed)
        return k.failed ? nullptr : &k;

    const auto createModule = (PFN_vkCreateCuModuleNVX) Proc(device, "vkCreateCuModuleNVX");
    const auto createFunction = (PFN_vkCreateCuFunctionNVX) Proc(device, "vkCreateCuFunctionNVX");
    k.destroyModule = (PFN_vkDestroyCuModuleNVX) Proc(device, "vkDestroyCuModuleNVX");
    k.destroyFunction = (PFN_vkDestroyCuFunctionNVX) Proc(device, "vkDestroyCuFunctionNVX");
    k.launch = (PFN_vkCmdCuLaunchKernelNVX) Proc(device, "vkCmdCuLaunchKernelNVX");

    VkCuModuleCreateInfoNVX moduleInfo { VK_STRUCTURE_TYPE_CU_MODULE_CREATE_INFO_NVX };
    moduleInfo.dataSize = sizeof DlssNrVitCopyKernel::fatbin;
    moduleInfo.pData = DlssNrVitCopyKernel::fatbin;

    if (createModule == nullptr || createFunction == nullptr || k.launch == nullptr ||
        createModule(device, &moduleInfo, nullptr, &k.module) != VK_SUCCESS)
    {
        k.failed = true;
        return nullptr;
    }

    VkCuFunctionCreateInfoNVX functionInfo { VK_STRUCTURE_TYPE_CU_FUNCTION_CREATE_INFO_NVX };
    functionInfo.module = k.module;
    functionInfo.pName = "nr_vit_copy16";

    if (createFunction(device, &functionInfo, nullptr, &k.function) != VK_SUCCESS)
    {
        if (k.destroyModule != nullptr)
            k.destroyModule(device, k.module, nullptr);
        k.module = VK_NULL_HANDLE;
        k.function = VK_NULL_HANDLE;
        k.failed = true;
        return nullptr;
    }

    return &k;
}

// A device-local buffer of at least `bytes` with a device address, on the current device. Caller holds Copies().mutex.
bool Allocate(CopyState& c, CopyBuffer& b, uint64_t bytes)
{
    // The address the way the device has it: a game that enabled VK_EXT_buffer_device_address itself keeps it (OptiScaler then adds no KHR
    // form), and the EXT form takes no allocation flag; otherwise the KHR / core form, whose memory needs the device-address flag.
    // The driver's lookup gives no extension function the device did not enable.
    const auto addressExt = (PFN_vkGetBufferDeviceAddress) Proc(c.device, "vkGetBufferDeviceAddressEXT");
    const auto addressKhr = (PFN_vkGetBufferDeviceAddress) Proc(c.device, "vkGetBufferDeviceAddressKHR");
    const auto addressCore = (PFN_vkGetBufferDeviceAddress) Proc(c.device, "vkGetBufferDeviceAddress");
    const bool ext = addressExt != nullptr;
    const PFN_vkGetBufferDeviceAddress getAddress = ext ? addressExt : addressKhr ? addressKhr : addressCore;

    if (getAddress == nullptr || c.physical == VK_NULL_HANDLE)
        return false;

    // The one being replaced stays alive until its device is destroyed: recorded command buffers can still use it.
    VkBufferCreateInfo info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = bytes;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CopyBuffer n;

    if (vkCreateBuffer(c.device, &info, nullptr, &n.buffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req {};
    vkGetBufferMemoryRequirements(c.device, n.buffer, &req);
    VkPhysicalDeviceMemoryProperties props {};
    vkGetPhysicalDeviceMemoryProperties(c.physical, &props);
    uint32_t type = UINT32_MAX;

    for (uint32_t i = 0; i < props.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = i;

    VkMemoryAllocateFlagsInfo flags { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo alloc { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, ext ? nullptr : &flags };
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;

    if (type == UINT32_MAX || vkAllocateMemory(c.device, &alloc, nullptr, &n.memory) != VK_SUCCESS)
    {
        vkDestroyBuffer(c.device, n.buffer, nullptr);
        return false;
    }

    VkBufferDeviceAddressInfo where { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    where.buffer = n.buffer;

    // the copy moves 16 bytes at a time
    if (vkBindBufferMemory(c.device, n.buffer, n.memory, 0) != VK_SUCCESS || (n.address = getAddress(c.device, &where)) == 0 ||
        (n.address & 15) != 0)
    {
        vkDestroyBuffer(c.device, n.buffer, nullptr);
        vkFreeMemory(c.device, n.memory, nullptr);
        return false;
    }

    n.capacity = bytes;

    if (b.buffer != VK_NULL_HANDLE)
        c.retired.push_back({ c.device, b });

    b = n;
    return true;
}

// Record the save (the run's 2-D output into our buffer) or the restore (back into it). Returns why it could not, or nullptr.
const char* Record(VkCommandBuffer commandBuffer, DlssNrVitReuse::Copy kind, const void* params, size_t size, uint64_t threads,
                   DlssNrVitReuse::KernelSet set, uint32_t extraCount)
{
    uint64_t dst = 0, bytes = 0;

    if (const char* why = DlssNrVitReuse::CheckOutput(params, size, threads, set, dst, bytes))
        return why;

    CopyState& c = Copies();
    std::lock_guard<std::mutex> lock(c.mutex);

    if (c.device == VK_NULL_HANDLE)
        return "no Vulkan device for the copy";

    const CopyKernel* k = Kernel(c, c.device);

    if (k == nullptr)
        return "the copy kernel could not be loaded";

    CopyBuffer& b = c.buffers[{ c.device, VitFilter().Feature() }];

    if (kind == DlssNrVitReuse::Copy::Save)
    {
        if (b.capacity < bytes && !Allocate(c, b, bytes))
            return "no buffer for the copy";

        b.dst = dst;
        b.saved = bytes;
    }
    else if (b.buffer == VK_NULL_HANDLE || b.dst != dst || b.saved != bytes)
    {
        return "the saved ViT output does not match the network";
    }

    struct
    {
        uint64_t dst, src, n;
    } copy { kind == DlssNrVitReuse::Copy::Save ? b.address : dst, kind == DlssNrVitReuse::Copy::Save ? dst : b.address, bytes / 16 };
    size_t copySize = sizeof copy;
    // The same extra-list form as the model's own launches
    const void* extras[] = { (const void*) 1, &copy, (const void*) 2, &copySize, nullptr };
    VkCuLaunchInfoNVX launch { VK_STRUCTURE_TYPE_CU_LAUNCH_INFO_NVX };
    launch.function = k->function;
    launch.gridDimX = (uint32_t) std::min<uint64_t>((copy.n + 255) / 256, 1024);
    launch.gridDimY = launch.gridDimZ = 1;
    launch.blockDimX = 256;
    launch.blockDimY = launch.blockDimZ = 1;
    launch.extraCount = extraCount;
    launch.pExtras = extras;
    k->launch(commandBuffer, &launch);

    if (!g_copyLogged.exchange(true))
        LOG_INFO("DLSS-NR Vulkan: Reuse bottleneck runs through our own copy of the ViT output ({} bytes, {} kernels)", bytes,
                 set == DlssNrVitReuse::KernelSet::Plain ? "plain FP16" : "FP8");

    return nullptr;
}

// Reuse goes off for the session; DlssNrFeature_Vk.cpp logs the reason when the evaluation ends.
void Off(const char* why) { VitFilter().Disable(why); }
} // namespace

void VkFunctionCreated(uint64_t function, const char* name) { Kernels().Created(function, name); }

void VkFunctionDestroyed(uint64_t function) { Kernels().Destroyed(function); }

void VkModuleDestroyed() { VitFilter().Clear(); }

void VkEvaluateDevice(void* device, void* physicalDevice)
{
    CopyState& c = Copies();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.device = static_cast<VkDevice>(device);
    c.physical = static_cast<VkPhysicalDevice>(physicalDevice);
}

void VkDeviceDestroying(void* device)
{
    const VkDevice d = static_cast<VkDevice>(device);
    CopyState& c = Copies();
    std::lock_guard<std::mutex> lock(c.mutex);
    const auto destroy = [d](CopyBuffer& b) {
        if (b.buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(d, b.buffer, nullptr);
        if (b.memory != VK_NULL_HANDLE)
            vkFreeMemory(d, b.memory, nullptr);
        b = CopyBuffer {};
    };

    for (auto it = c.buffers.begin(); it != c.buffers.end();)
    {
        if (it->first.first != d)
        {
            ++it;
            continue;
        }

        destroy(it->second);
        it = c.buffers.erase(it);
    }

    for (auto it = c.retired.begin(); it != c.retired.end();)
    {
        if (it->first != d)
        {
            ++it;
            continue;
        }

        destroy(it->second);
        it = c.retired.erase(it);
    }

    if (const auto k = c.kernels.find(d); k != c.kernels.end())
    {
        if (k->second.function != VK_NULL_HANDLE && k->second.destroyFunction != nullptr)
            k->second.destroyFunction(d, k->second.function, nullptr);
        if (k->second.module != VK_NULL_HANDLE && k->second.destroyModule != nullptr)
            k->second.destroyModule(d, k->second.module, nullptr);
        c.kernels.erase(k);
    }

    if (c.device == d)
        c.device = VK_NULL_HANDLE;
}

void VkDeviceLost()
{
    VitFilter().Clear();
    Kernels().Clear();
    {
        CopyState& c = Copies();
        std::lock_guard<std::mutex> lock(c.mutex);
        c.kernels.clear();
        c.buffers.clear();
        c.retired.clear();
        c.device = VK_NULL_HANDLE;
        c.physical = VK_NULL_HANDLE;
    }
}

bool VkLaunch(void* commandBuffer, const void* launchInfo)
{
    DlssNrVitReuse::Filter& filter = VitFilter();

    if (!filter.Evaluating() || launchInfo == nullptr)
        return true;

    const auto& launch = *static_cast<const VkCuLaunchInfoNVX*>(launchInfo);
    const VkCommandBuffer cmd = static_cast<VkCommandBuffer>(commandBuffer);
    const auto [role, set] = Kernels().Find((uint64_t) launch.function);

    if (role == DlssNrVitReuse::Role::Start && !g_runSeen.exchange(true))
        LOG_INFO("DLSS-NR Vulkan: the model's ViT run seen ({} kernels)", set == DlssNrVitReuse::KernelSet::Plain ? "plain FP16" : "FP8");

    const DlssNrVitReuse::Filter::Single one = filter.One(role, set, role == DlssNrVitReuse::Role::Start ? Sig(launch) : 0);

    // Where a skipped run was: the kernels on either side of the gap must not overlap, which the run's own kernels used to ensure.
    if (one.barrier)
        Barrier(cmd);

    const void* params = nullptr;
    size_t size = 0;

    if (one.copy == DlssNrVitReuse::Copy::Restore)
    {
        // In place of the run's last kernel, then a barrier so the decoder reads it complete. If it cannot be recorded, Reuse goes off and
        // the last kernel goes out after all (right where the DLL keeps the 1-D result, one stale frame where it does not).
        const char* why = LaunchParams(launch, params, size)
                              ? Record(cmd, one.copy, params, size, Threads(launch), set, launch.extraCount)
                              : "unexpected parameters of the ViT run's last kernel";
        Barrier(cmd);

        if (why != nullptr)
        {
            Off(why);
            return true;
        }
    }
    else if (one.copy == DlssNrVitReuse::Copy::Save)
    {
        t_save = PendingSave {};

        if (LaunchParams(launch, params, size))
        {
            t_save.params.assign(static_cast<const unsigned char*>(params), static_cast<const unsigned char*>(params) + size);
            t_save.threads = Threads(launch);
            t_save.set = set;
            t_save.extraCount = launch.extraCount;
            t_save.pending = true;
        }
        else
        {
            Off("unexpected parameters of the ViT run's last kernel");
        }
    }

    return one.launch;
}

void VkLaunched(void* commandBuffer)
{
    if (!t_save.pending)
        return;

    t_save.pending = false;
    const VkCommandBuffer cmd = static_cast<VkCommandBuffer>(commandBuffer);

    // Right after the run's last kernel: a barrier so the copy sees its output complete, and one after it so nothing later overwrites
    // that output before the copy has read it.
    Barrier(cmd);
    const char* why = Record(cmd, DlssNrVitReuse::Copy::Save, t_save.params.data(), t_save.params.size(), t_save.threads, t_save.set,
                             t_save.extraCount);
    Barrier(cmd);

    if (why != nullptr)
        Off(why);
}
} // namespace DlssNrNative
