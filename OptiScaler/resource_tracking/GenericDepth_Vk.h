#pragma once

// Finds the scene's depth buffer in a Vulkan game by watching how the game uses its depth images: the Vulkan adapter's
// depth observer (docs/NATIVE-INPUT-ADAPTERS.md), on the same native::DepthFinderCore as the D3D12 and D3D11 ones.
// GenericDepth_VkEvents.h holds the bookkeeping; this file is the hooks and the copies.
//
// Off unless [DlssNr] NativeDepthFinder is set, read when the game makes its Vulkan device. When on:
//   - vkCreateImage, vkCreateImageView, vkCreateFramebuffer and vkCreateRenderPass(2) are watched (with their
//     destroys), and depth images made for rendering get TRANSFER_SRC added to their usage so they can be copied
//     ([DlssNr] NativeDepthVkCopyUsage, on by default; off, depth is never copied);
//   - the command hooks Vulkan_wDx12 already has (hooks/VulkanwDx12_Hooks.cpp) report render passes, dynamic rendering,
//     clears, draws and viewports;
//   - the picked buffer's depth is copied into a buffer of the finder's own, in the game's command buffer, right after
//     the pass that drew it ends. Three buffers rotate per present; the frame source (native/VkFrameSource) copies the
//     one of the frame being presented across to D3D12.
// Multisampled depth is not copied (vkCmdCopyImageToBuffer cannot read it).

#include <vulkan/vulkan.h>

#include <cstdint>

namespace GenericDepthVk
{
// From vkCreateDevice: installs once when the key is on. `physical` answers the format questions.
void OnDevice(VkDevice device, VkPhysicalDevice physical);
void OnDeviceDestroyed(VkDevice device);
bool Installed();
bool InstallFailed();

// The resource hooks, for vkGetDeviceProcAddr/vkGetInstanceProcAddr (null when `name` is not one of them) and for the
// loader's exports (detoured once).
PFN_vkVoidFunction GetProcAddr(PFN_vkVoidFunction original, const char* name);
void Hook(HMODULE vulkanModule);

// From the command hooks (hooks/VulkanwDx12_Hooks.cpp). A load and a return while not installed or stood down.
void BeginCommandBuffer(VkCommandBuffer cmd, const VkCommandBufferBeginInfo* info);
void EndCommandBuffer(VkCommandBuffer cmd);
void ForgetCommandBuffers(uint32_t count, const VkCommandBuffer* cmds);
void BeginRenderPass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info);
void NextSubpass(VkCommandBuffer cmd);
void EndRenderPass(VkCommandBuffer cmd);
void BeginRendering(VkCommandBuffer cmd, const VkRenderingInfo* info);
void EndRendering(VkCommandBuffer cmd);
void ClearDepthStencilImage(VkCommandBuffer cmd, VkImage image, VkImageLayout layout,
                            const VkClearDepthStencilValue* value, uint32_t rangeCount,
                            const VkImageSubresourceRange* ranges);
void ClearAttachments(VkCommandBuffer cmd, uint32_t count, const VkClearAttachment* attachments);
void Draw(VkCommandBuffer cmd, uint32_t vertices, uint32_t instances);
void Indirect(VkCommandBuffer cmd, uint32_t maxCount);
void Viewport(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkViewport* viewports);
void ExecuteCommands(VkCommandBuffer primary, uint32_t count, const VkCommandBuffer* secondaries);

// Once per present, from native/NativeDriverVk.cpp before the frame source runs: closes the frame's counts, picks,
// and makes the frame's copy the one BestSnapshot hands out.
void OnPresent(VkDevice device, uint32_t width, uint32_t height);

// The picked buffer's depth from the frame being presented, in the finder's own buffer (rows DepthRowPitch apart).
struct Snapshot
{
    bool valid = false;
    VkBuffer buffer = VK_NULL_HANDLE;
    uint64_t size = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    bool reversed = false;
};
Snapshot BestSnapshot();

// Why nothing can be copied (the picked buffer is multisampled, or was made without TRANSFER_SRC), or null.
const char* NotReadableReason();

// A Vulkan upscaler call: the finder stands down (the same rule and wake-up as the D3D12 one).
void NoteUpscalerCall();
bool GameCallsUpscaler();
bool Armed();

// ImGui: the menu's one depth line, and the "Use the game's depth" checkbox.
void DrawStatus();
void DrawAdvancedUi();
} // namespace GenericDepthVk
