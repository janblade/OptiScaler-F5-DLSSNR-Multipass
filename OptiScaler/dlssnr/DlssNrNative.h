#pragma once
#include <cstdint>
#include <string>
#include <vector>
struct ID3D12GraphicsCommandList;
namespace DlssNrNative {
void* WrapNvapi(unsigned id,void* original);
void SetEnabled(bool enabled);
void SetPrecision(unsigned precision);
bool IsActive();
std::string Status();
// ViT reuse (see DlssNrVitReuse.h): bracket one model evaluation of `feature`. `every` = how often the ViT bottleneck is computed (1 = always) with the fp8 kernel set, `everyPlain` with the plain fp16 one; `slot` = frame number, the same for every pass of a frame, so the passes compute on the same frame.
void BeginEvaluate(const void* feature, bool reset, unsigned every, unsigned everyPlain, long long slot, ID3D12GraphicsCommandList* cmd, bool profile);
// True when this evaluation's launch order was not the expected one and turned Reuse off for the session (log it once).
bool EndEvaluate(ID3D12GraphicsCommandList* cmd);
// Kernel census / per-group GPU timing lines finished since the last call (ini [DlssNr] KernelProfile), for the log.
std::vector<std::string> TakeProfileReports();
std::string VitStatus();
// The kernel set of the last ViT run seen: "FP8", "plain FP16" or "not seen yet".
const char* VitKernelSet();
// True when the last ViT run seen used the plain fp16 kernels.
bool VitPlainKernels();
// Vulkan (DlssNrNative_Vk.cpp): the model's VK_NVX_binary_import calls, from the Vulkan hook table
// (VulkanwDx12_Hooks.cpp), so Reuse bottleneck and the kernel set work there too. `function` is a VkCuFunctionNVX,
// `commandBuffer` a VkCommandBuffer.
void VkFunctionCreated(uint64_t function, const char* name);
void VkFunctionDestroyed(uint64_t function);
// A CUDA module was destroyed (any caller's, as on D3D12): no kept bottleneck result can be trusted any more. The kernel
// handles stay: each kernel's own destroy removes it.
void VkModuleDestroyed();
// The device was lost and its kernels abandoned without their destroy calls: forget the kept results and the handles too
// (a new device can hand them out again).
void VkDeviceLost();
// One launch: records the barrier where a skipped ViT run was, and says whether the launch goes out. Inside our own
// evaluate bracket only; any other launch (the game's own DLSS SR) goes out untouched.
bool VkLaunch(void* commandBuffer, uint64_t function);
}
