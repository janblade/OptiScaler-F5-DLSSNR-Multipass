#pragma once

// The Vulkan present bridge, OptiScaler's side: a Vulkan game that gets frame generation from Optical F5Low's "NR +
// upscaler & frame generation" mode presents through a D3D12 swapchain, the way a D3D11 game does (with_dx12/
// dx11_with_dx12_sc.h). When the game makes its swapchain with the mode and a frame generation output chosen at start
// (OptiFG Upscaler input), the VkSwapchain is made on a hidden window's surface, and the real window gets a D3D12 swapchain
// from frame generation, on the private D3D12 device that native/VkFrameSource.h already shares the picture with. Per
// present the picture goes through the producer into that swapchain's back buffer (CopyToOutput) and the D3D12 swapchain
// presents it (PresentOutput); the game's own present only cycles its images on the hidden window. Any failure leaves the
// game's swapchain on the real window, as it was.
//
// native/VkPresentBridgeCore.h has the mechanics (it is tested alone); this file decides when and wires the rest of
// OptiScaler in: the surface-to-window map, frame generation, the menu, State.

#include <windows.h>
#include <d3d12.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace VkPresentBridge
{

// The fullScreenExclusive mode of a VkSurfaceFullScreenExclusiveInfoEXT in a create info's chain, or null. The swapchain
// made on the hidden surface is asked for DISALLOWED through it (and the game's value put back after the call): the hidden
// window must never take the screen, and the D3D12 swapchain on the real window handles full screen as for a D3D12 game.
// Null when there is none, and when the game's struct is in read-only memory (`readOnly` then true): it is not written.
int32_t* FullScreenExclusiveMode(const void* chain, bool* readOnly = nullptr);
inline constexpr int32_t kFullScreenExclusiveDisallowed = 2;

// vkCreateWin32SurfaceKHR / vkDestroySurfaceKHR: which window a surface is on.
void NoteSurface(VkSurfaceKHR surface, HWND window);
void ForgetSurface(VkSurfaceKHR surface);

// The game is making a swapchain. True when it is to be made from `out` instead of `in` (on the hidden surface); `out` is
// then the whole create info. False: make it as the game asked, from `out` (the same, but for an old swapchain that was
// the bridge's and cannot be continued on the game's own surface). Never fails the game's call.
bool OnCreateSwapchain(VkInstance instance, VkPhysicalDevice physical, VkDevice device, const VkSwapchainCreateInfoKHR& in,
                       VkSwapchainCreateInfoKHR* out);

// The result of the original vkCreateSwapchainKHR for a create info OnCreateSwapchain returned true for. On failure the
// bridge is let go, and the caller makes the game's swapchain as it asked (a bridged old swapchain dropped).
void OnSwapchainCreated(bool bridged, VkResult result, VkSwapchainKHR created);

// After the original vkDestroySwapchainKHR.
void OnSwapchainDestroyed(VkDevice device, VkSwapchainKHR swapchain);

// vkDestroyDevice: before the original call the D3D12 swapchain goes; after it the hidden surface and window.
void OnDeviceDestroying(VkDevice device);
void OnDeviceDestroyed(VkDevice device);

// The swapchain was made on a hidden surface (it cannot be continued on the game's own surface).
bool Owns(VkSwapchainKHR swapchain);

// The swapchain is the game's, bridged: its presents go to the D3D12 swapchain.
bool Active(VkSwapchainKHR swapchain);

// Any bridge is up (the menu, the frame limiter and the Vulkan menu overlay ask).
bool IsUp();

// The bridge's private D3D12 device while its output is up, else null. Not a reference: it lives as long as the bridge.
ID3D12Device* Device();

// The frame source has processed this present: puts the shared picture into the D3D12 swapchain's back buffer. `picture`
// (null: none this frame) is ready once `copied` is reached on `fence` and the producer's `doneFence` (null: none) has
// reached `doneValue`; `signalValue` is signalled on `fence` once the copy is done. False when the copy could not be queued.
bool CopyToOutput(ID3D12Resource* picture, ID3D12Fence* fence, uint64_t copied, ID3D12Fence* doneFence,
                  uint64_t doneValue, uint64_t signalValue);

// The D3D12 swapchain presents what CopyToOutput gave it (after the game's hidden present has been queued). Returns on the
// game's present thread, paced by the D3D12 present.
void PresentOutput();

// The real window is not the swapchain's size and the game has not recreated the swapchain yet: its present should say so.
bool WindowResized();

} // namespace VkPresentBridge
