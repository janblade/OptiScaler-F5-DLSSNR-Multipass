#pragma once

// The exposure scan on Vulkan (plan 2026-10-14-exposure-scan-all-apis, story 4), for a native Vulkan game, a D3D11 game
// on dxvk read through Vulkan, and VkwDx12: the same candidates as on D3D12 (DlssNr_ExposureScan.h) -- a small storage
// buffer or a small float storage image the game makes -- found on its creation calls and copied into a host-visible
// ring on a command buffer NR records anyway. They join the one list the readout and the recording show.
//
// Vulkan has no reference counts, so nothing is held: a candidate exists from its memory binding until the game
// destroys it or frees its memory (those calls drop it first). Copying needs TRANSFER_SRC, which the scan adds to a
// candidate-shaped buffer or image at creation (as the depth finder does for depth images). An image is copied only in
// the layout the game's last recorded barrier left it in; one never seen in a barrier is skipped.

#include <windows.h>
#include <vulkan/vulkan.h>

#include "DlssNr_ExposureScan.h"

namespace DlssNr
{
namespace ExposureScanVk
{
// From the device and instance proc address hooks (spoofing/Vulkan_Spoofing.cpp): the buffer and memory hooks, while
// the scan is wanted; null for every other name.
PFN_vkVoidFunction GetProcAddr(PFN_vkVoidFunction original, const char* name);

// vulkan-1's exports, for a game that links them directly (spoofing/Vulkan_Spoofing.cpp), while the scan is wanted.
void Hook(HMODULE vulkanModule);

// From the depth finder's vkCreateImage / vkDestroyImage hooks (resource_tracking/GenericDepth_Vk.cpp). Before
// creation: whether `info` is candidate-shaped, then the caller adds TRANSFER_SRC to its copy. After: note the image.
bool WantsImage(const VkImageCreateInfo& info);
void OnImageCreated(VkDevice device, const VkImageCreateInfo& info, VkImage image, void* caller);
void OnImageDestroyed(VkImage image);

// From the game's pipeline barriers (hooks/VulkanwDx12_Hooks.cpp): the layout each candidate image is left in.
void NoteBarriers(uint32_t count, const VkImageMemoryBarrier* barriers);
void NoteBarriers2(const VkDependencyInfo* info);

// NR on Vulkan (dlssnr/DlssNrFeature_Vk.cpp), once per NR frame on its command buffer: reads back the values copied
// kSlots frames ago, writes the frame's bookkeeping (the recording's row, the movers) and copies this frame's.
void Tick(VkCommandBuffer cmd, VkDevice device, VkPhysicalDevice physical, const ExposureScan::FrameExposure& frame);

// NR on D3D12 for a Vulkan game (native/VkFrameSource.cpp, at the copy in): reads and copies only; the D3D12 pass
// writes the frame's bookkeeping.
void Record(VkCommandBuffer cmd, VkDevice device, VkPhysicalDevice physical);

// The game destroys a device: its candidates and the ring made on it go.
void OnDeviceDestroyed(VkDevice device);
} // namespace ExposureScanVk
} // namespace DlssNr
