#pragma once

#include <vulkan/vulkan.h>

#include <shaders/dlssnr/DlssNr_Common.h>
#include <shaders/dlssnr/DlssNr_Spatial.h>

#include <optional>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>

// DLSS 5 Neural Rendering on Vulkan, natively.
//
// The model ships a complete Vulkan surface -- fourteen exported entry points against D3D12's ten,
// including both extension requirement queries -- so a Vulkan game has no need of the D3D12 bridge
// this pass reached it through before. What stopped it was never the model; it was the device. NGX
// loads its kernels through two NVIDIA vendor extensions no game enables, and a Vulkan device's
// extension list is fixed at creation, so asking afterwards gets a permanent no. OptiScaler already
// appends them in its vkCreateDevice hook, and Enshrouded confirmed the device is created with them.
//
// What differs from the D3D12 path, and why:
//
//   * The game's colour, depth and motion arrive already wrapped. NGX hands Vulkan resources over as
//     NVSDK_NGX_Resource_VK, so only this pass's own images need wrapping.
//   * Layouts are explicit. There is no equivalent of a D3D12 resource state promotion, so every
//     image is moved to the layout each dispatch needs and moved back.
//   * There is no root signature to save and restore, so no envelope. The bindless hazard that cost
//     007 First Light a device on D3D12 has no Vulkan counterpart.
//
// The composition shader is shared, compiled from the same source to SPIR-V. Any behavioural
// difference between the two backends is a bug rather than a design.

class Config;

namespace DlssNr
{

// Runs the model over what the upscaler just wrote, on the same command buffer.
//
// Everything Vulkan needs that D3D12 does not is passed rather than looked up: the device handles
// belong to the game's instance and there is no ambient place to find them from here.
//
// Safe to call every frame. It builds what it needs on first use and disables itself for the session
// rather than retrying into a crash.
void EvaluateAfterUpscaleVk(VkCommandBuffer cmdBuffer, NVSDK_NGX_Parameter* params, VkInstance instance,
                            VkPhysicalDevice physicalDevice, VkDevice device, bool rayReconstruction = false,
                            bool ranBefore = false);

// Returns an owned, readable replacement Color, or null to leave the game's input unchanged.
// The caller must restore the original Color parameter after the upscaler, on every exit.
NVSDK_NGX_Resource_VK* EvaluateBeforeUpscaleVk(VkCommandBuffer cmdBuffer, NVSDK_NGX_Parameter* params,
                                             VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
                                             bool& handled, bool rayReconstruction = false);

// Whether the native Vulkan path is up, and why not if it is not.
bool IsRunningVk();
const char* FailureReasonVk();

// Whether a retry could help with the failure FailureReasonVk reports (not when the model has no usable Vulkan surface
// or NGX will not start on the device). RequestRetryVk asks for one; the next evaluate does it on the NR thread: drains
// the device, lets go of NR's own objects (never the present path's), clears the failure and any compression fallback.
bool RetryableVk();
void RequestRetryVk();

// The size the model works on (the packed size while Compress screen edges is on), or 0x0 before the first frame.
void CurrentModelSizeVk(unsigned int& width, unsigned int& height);

// How many frames it has actually composed. The menu needs this to tell "up but nothing has come
// through yet" apart from "running", and the D3D12 counters say nothing about this path.
unsigned long long FramesVk();

// What the pass last cost on the GPU, in milliseconds, or nothing if it has not been measured yet.
// A timestamp pair either side of the whole pass, read three frames later so the query is retired.
std::optional<double> LastGpuTimeVk();

// The Vulkan frame clock: Vulkan presents where the present hook counts them (it comes with the overlay), else NR's own
// frames. What the frame generation gate and the cadence count in on Vulkan; one clock for stamp and reading.
unsigned long long VkFrameClock();

// Reuse detail between frames on this path (DlssNr_DetailReuse_Vk.inl), in the shape the D3D12 accessor returns.
struct DetailReuseInfo;
DetailReuseInfo DetailReuseStatusVk();

// Compress screen edges on this path (DlssNr_Spatial_Vk.inl), in the shape the D3D12 accessor returns
// (DlssNr::EdgeCompressionStatus): the status code, the sizes and the boxes, as of the last frame NR ran.
Spatial::Published EdgeCompressionStatusVk();

// Whether the game offers an exposure texture on this path. Observed only: it is not read, because
// binding the game's image means naming a layout this side cannot know. For the menu, and to settle
// whether reading it is worth the risk on any real Vulkan game.
bool ExposureOfferedVk();

// What has been read of the game's exposure on this path, in the shape the D3D12 accessor returns, so
// the menu can show the base white point and capture Trim anchors against it.
struct ExposureStatus;
ExposureStatus GameExposureStatusVk();
ExposureStatus AutoExposureStatusVk();

// Automatic following the game's own exposure on an unexposed frame; same shape as the D3D12 accessor.
struct FollowGameStatus;
FollowGameStatus FollowGameExposureStatusVk();

void ShutdownVk(bool deviceAlive = true);

// The game is about to destroy `device` (the vkDestroyDevice hook, Vulkan_Hooks.cpp) or shut its NGX down
// (NVNGX_DLSS_Vk.cpp; VK_NULL_HANDLE there means whichever device NR runs on). If NR runs on it, everything is
// released while the device and the game's NGX still live -- the model's features, the parameter block, NGX itself
// (Shutdown1) -- as NGX asks for, instead of being abandoned with them. `why` is for the log.
void ShutdownVkForDevice(VkDevice device, const char* why);

} // namespace DlssNr
