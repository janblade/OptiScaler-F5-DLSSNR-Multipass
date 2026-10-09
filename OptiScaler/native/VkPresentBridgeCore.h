#pragma once

// The mechanics of the Vulkan present bridge: the game's VkSwapchain lives on a hidden window's surface, and the real
// window gets a D3D12 swapchain on the private D3D12 device (the D3D11 model, with_dx12/dx11_with_dx12_sc.h, applied to
// Vulkan). Per frame the swapchain image is copied into the shared picture (native/VkFrameSource.h), the producer works
// on it, and CopyFrame puts the result into the D3D12 swapchain's back buffer; the hidden Vulkan present only keeps the
// game's image cycle going. A D3D12 swapchain made through frame generation then presents it.
//
// This file knows nothing of OptiScaler (no State, no hooks): the glue (native/VkPresentBridge.cpp) decides when the
// bridge is wanted and makes the D3D12 swapchain through frame generation; tests/nr_vk_present_bridge_gpu.cpp drives it
// with a plain swapchain. Not built with the precompiled header.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <cstdint>
#include <functional>
#include <string>

namespace native
{

// A game swapchain format the bridge carries, as the flip-model D3D12 swapchain's format (sRGB forms through UNORM: the
// bytes are the same and a flip swapchain takes no sRGB format). False for any other.
bool BridgeSwapchainFormat(VkFormat format, DXGI_FORMAT* dxgi);

// The colour space the D3D12 swapchain is told (SetColorSpace1). False for a colour space the bridge does not know.
bool BridgeColorSpace(VkColorSpaceKHR space, DXGI_COLOR_SPACE_TYPE* dxgi);

// What the bridge is made from.
struct BridgeTarget
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    PFN_vkCreateWin32SurfaceKHR createSurface = nullptr; // the loader's own, not a hooked one
    HWND realWindow = nullptr;                           // the game's window: the D3D12 swapchain goes here
    ID3D12Device* device12 = nullptr;
    ID3D12CommandQueue* queue12 = nullptr;
    bool tearing = false; // DXGI allows tearing: an IMMEDIATE game gets ALLOW_TEARING presents
};

// Makes the D3D12 swapchain on the real window from `desc` (windowed, flip model, `desc.OutputWindow` set). The glue
// goes through frame generation here; the test makes a plain one.
using MakeOutputFn = std::function<bool(DXGI_SWAP_CHAIN_DESC& desc, Microsoft::WRL::ComPtr<IDXGISwapChain4>& out,
                                        std::string& why)>;
// Called with the D3D12 swapchain just before the bridge lets go of it (frame generation's bookkeeping goes here).
using ReleaseOutputFn = std::function<void(IDXGISwapChain4* output)>;

class VkPresentBridgeCore
{
  public:
    ~VkPresentBridgeCore() { Release(); }

    static constexpr uint32_t kRing = 3;

    // For the game's first swapchain: the hidden window and surface, the D3D12 swapchain and the copy ring. Nothing is
    // left behind on failure (`why` says what went wrong).
    bool Create(const BridgeTarget& target, const VkSwapchainCreateInfoKHR& game, MakeOutputFn make,
                ReleaseOutputFn release, std::string& why);

    // The game makes a new swapchain (a resize): the hidden window and the D3D12 swapchain follow its extent and format.
    // Does nothing when they have not changed.
    bool Resize(const VkSwapchainCreateInfoKHR& game, std::string& why);

    // Resize would resize the D3D12 swapchain (the extent, format or colour space changed). The glue asks first, to keep
    // frame generation out of the swapchain (switched off, its presents waited for) before the bridge's lock is taken.
    bool ResizeNeeded(const VkSwapchainCreateInfoKHR& game) const;

    // What the game's swapchain is made with instead: the hidden surface, the surface's own extent, transform and
    // composite alpha, IMMEDIATE else MAILBOX else FIFO, and a format and colour space the hidden surface offers (the
    // game's format always; its colour space when offered, else any). False when the hidden surface cannot carry it.
    bool Patch(const VkSwapchainCreateInfoKHR& game, VkSwapchainCreateInfoKHR* out, std::string& why) const;

    // After the producer, on the D3D12 queue: waits for `waitValue` on `waitFence` (the shared fence: the copy into
    // `source` is done) and for `afterValue` on `afterFence` (the producer's own done point; null: none), copies `source`
    // into the swapchain's current back buffer and signals `signalValue` on `signalFence` (Vulkan waits for it before it
    // overwrites `source` again). `source` rests in COMMON and the same size and format family as the back buffer. The
    // back buffer stays in PRESENT.
    bool CopyFrame(ID3D12Resource* source, ID3D12Fence* waitFence, uint64_t waitValue, ID3D12Fence* afterFence,
                   uint64_t afterValue, ID3D12Fence* signalFence, uint64_t signalValue, std::string& why);

    // The real window's client area is not the swapchain's size (the game has not resized yet): a present that tells the
    // game so (VK_ERROR_OUT_OF_DATE_KHR) makes it recreate the swapchain. False while the window is minimised.
    bool RealWindowResized() const;

    // The present's question: tell the game its swapchain is out of date? True while the real window is not the
    // swapchain's size, for at most kMaxOutOfDate presents in a row: a window the game never gets to match (the
    // extent it asks the hidden surface for is not the window's client size) would otherwise be told so forever and
    // recreate its swapchain every frame. A resize of the swapchain, or the window matching, starts the count again.
    static constexpr uint32_t kMaxOutOfDate = 6;
    bool ShouldReportOutOfDate();

    // The count ran out while the window still does not match.
    bool OutOfDateGivenUp() const { return _outOfDate >= kMaxOutOfDate && RealWindowResized(); }

    // Waits for the copies in flight.
    void WaitIdle();

    // The D3D12 swapchain goes (frame generation is told first); then the hidden surface and window, which only after
    // every game swapchain made on it is destroyed. Release does both.
    void ReleaseOutput();
    void ReleaseSurface();
    void Release();

    bool Created() const { return _surface != VK_NULL_HANDLE || _output != nullptr; }
    IDXGISwapChain4* Output() const { return _output.Get(); }
    HWND RealWindow() const { return _target.realWindow; }
    HWND HiddenWindow() const { return _hidden; }
    VkSurfaceKHR HiddenSurface() const { return _surface; }
    uint32_t Width() const { return _width; }
    uint32_t Height() const { return _height; }
    VkFormat Format() const { return _format; }
    UINT SyncInterval() const { return _syncInterval; }
    UINT PresentFlags() const { return _syncInterval == 0 && _target.tearing && _tearingSwapchain ? DXGI_PRESENT_ALLOW_TEARING : 0; }

    // The copy ring's fence and the value the last CopyFrame signalled on it: a present queue other than the D3D12 queue
    // waits for it before it reads the back buffer.
    ID3D12Fence* CopyFence() const { return _copyFence.Get(); }
    uint64_t LastCopyValue() const { return _copyValue; }

  private:
    bool MakeOutput(const VkSwapchainCreateInfoKHR& game, std::string& why);
    bool MakeRing(std::string& why);
    void SetHiddenSize(uint32_t width, uint32_t height);
    void DropBackBufferReferences();

    BridgeTarget _target;
    MakeOutputFn _make;
    ReleaseOutputFn _release;

    HWND _hidden = nullptr;
    VkSurfaceKHR _surface = VK_NULL_HANDLE;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> _output;
    bool _tearingSwapchain = false;
    uint32_t _width = 0;
    uint32_t _height = 0;
    VkFormat _format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR _space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    DXGI_FORMAT _dxgi = DXGI_FORMAT_UNKNOWN;
    UINT _syncInterval = 1;
    UINT _outputFlags = 0;

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> _allocators[kRing];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> _list;
    Microsoft::WRL::ComPtr<ID3D12Fence> _copyFence;
    uint64_t _copyValue = 0;
    uint64_t _slotValue[kRing] = {};
    uint32_t _slot = 0;
    uint32_t _outOfDate = 0; // presents in a row told the swapchain is out of date
};

} // namespace native
