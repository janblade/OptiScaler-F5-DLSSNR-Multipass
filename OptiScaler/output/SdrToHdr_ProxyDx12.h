#pragma once

// Direct3D 12 and DXGI only (no OptiScaler headers but SdrToHdr_Common.h, SdrToHdr_ProxyBase.h, SdrToHdr_Dx12.h and
// native/F5LowShaderBytecode.h), so tests/sdr_to_hdr_swapchain_dx12_gpu.cpp builds it alone.
#include "SdrToHdr_Dx12.h"
#include "SdrToHdr_ProxyBase.h"

#include <d3d12.h>

namespace SdrToHdr
{
// The Direct3D 12 proxy back buffers: one per back buffer of the real chain, in the game's format, resting in PRESENT, and the
// pass (output/SdrToHdr_Dx12.h) that converts proxy[current] -> real[current] on the game's queue.
class ProxyChain final : public ProxyChainBase
{
  public:
    ProxyChain() = default;
    ~ProxyChain() override { Release(); }

    const char* Present(IUnknown* queueOrContext, IDXGISwapChain3* real, PassConstants constants) override;
    bool OwnsResource(const void* resource, bool& relaxedCasting) override;

  protected:
    const char* BuildResourcesLocked(IUnknown* device, IDXGISwapChain3* real, DXGI_FORMAT gameFormat) override;
    void ReleaseResourcesLocked() override;
    bool ResourcesLocked() const override { return !_proxies.empty() && _pass.Ready(); }
    bool HeldResourcesLocked() override;
    void QuiesceLocked() override { _pass.WaitForIdle(); }
    HRESULT GetBufferLocked(UINT index, REFIID riid, void** surface) override;

  private:
    std::vector<ID3D12Resource*> _proxies;
    Dx12Pass _pass;
    DXGI_FORMAT _realFormat = DXGI_FORMAT_UNKNOWN; // the real chain's: scRGB (FP16) or PQ (10 bit) follows from it
    bool _relaxed = false;     // made with castable formats: GetDesc shows the game's format and the pass reads it raw
    bool _typedSrgb = false;   // a typed _SRGB proxy: read through an _SRGB view, the shader encodes it again
};
} // namespace SdrToHdr
