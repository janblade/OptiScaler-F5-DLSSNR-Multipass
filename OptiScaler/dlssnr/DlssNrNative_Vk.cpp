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
// Reuse itself is off on Vulkan for now: DlssNrFeature_Vk.cpp asks for every = 1, so nothing is dropped and this only
// tells the kernel set. With the run skipped, its kept result was overwritten between frames there (see that call).

#include "DlssNrNative.h"
#include "DlssNrVitReuse.h"

#include <vulkan/vulkan.h>

#include <atomic>

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
} // namespace

void VkFunctionCreated(uint64_t function, const char* name) { Kernels().Created(function, name); }

void VkFunctionDestroyed(uint64_t function) { Kernels().Destroyed(function); }

void VkModuleDestroyed() { VitFilter().Clear(); }

void VkDeviceLost()
{
    VitFilter().Clear();
    Kernels().Clear();
}

bool VkLaunch(void* commandBuffer, uint64_t function)
{
    DlssNrVitReuse::Filter& filter = VitFilter();

    if (!filter.Evaluating())
        return true;

    const auto [role, set] = Kernels().Find(function);

    if (role == DlssNrVitReuse::Role::Start && !g_runSeen.exchange(true))
        LOG_INFO("DLSS-NR Vulkan: the model's ViT run seen ({} kernels); Reuse bottleneck stays off on Vulkan",
                 set == DlssNrVitReuse::KernelSet::Plain ? "plain FP16" : "FP8");

    const DlssNrVitReuse::Filter::Single one = filter.One(role, set);

    // Where a skipped run was (before its kept last kernel on the plain set): the kernels on either side of the gap
    // must not overlap, which the run's own kernels used to ensure. The pipeline stage of an NVX launch is not
    // specified, so the barrier covers all commands.
    if (one.barrier)
    {
        VkMemoryBarrier barrier {};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                                VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(static_cast<VkCommandBuffer>(commandBuffer), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    return one.launch;
}
} // namespace DlssNrNative
