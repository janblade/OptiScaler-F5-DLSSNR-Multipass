#pragma once

// The D3D11 adapter's frame source: the game's picture and the depth finder's copy (resource_tracking/GenericDepth_Dx11.h) are
// shared across to the D3D12 device the producer runs on (native/SharedFrame.h), and the processed picture is shared back. Uses
// its own D3D12 device/queue, paired with the game's D3D11 device the same way IFeature_Dx11wDx12 pairs one for a game that does
// call an upscaler (with_dx12/with_dx12.h); no game call is needed to start it here.

#include "FrameContract.h"
#include "SharedFrame.h"

#include <d3d11_4.h>
#include <wrl/client.h>

namespace native
{

class Dx11FrameSource : public IFrameSource
{
  public:
    // The present being processed: the swap chain and the game's D3D11 device. Call before Acquire().
    void SetPresent(IDXGISwapChain* swapChain, ID3D11Device* device11);

    Api GetApi() const override { return Api::D3D11; }
    AcquireStatus Acquire(FrameInput& input) override;
    void Return(const FrameInput& input, const FrameOutput& output) override;
    void OnResize() override;

    ID3D12Device* Device12() const { return _device12; }
    ID3D12CommandQueue* Queue12() const { return _queue12; }
    const std::string& Error() const { return _error; }

  private:
    bool EnsureDevices(ID3D11Device* device11);

    IDXGISwapChain* _swapChain = nullptr;
    ID3D11Device* _device11 = nullptr;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> _context11;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> _context4;
    ID3D12Device* _device12 = nullptr;    // owned by WithDx12, not released here
    ID3D12CommandQueue* _queue12 = nullptr;

    SharedFence _fence;
    SharedTexture _picture;
    SharedTexture _depth;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _backBuffer; // held between Acquire and Return

    uint64_t _frame = 0;
    std::string _error;
};

} // namespace native
