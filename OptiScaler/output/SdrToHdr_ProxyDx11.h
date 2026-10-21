#pragma once

// Direct3D 11 and DXGI only (no OptiScaler headers but SdrToHdr_Common.h, SdrToHdr_ProxyBase.h, SdrToHdr_Dx11.h and
// native/F5LowShaderBytecode.h), so tests/sdr_to_hdr_swapchain_dx11_gpu.cpp builds it alone.
#include "SdrToHdr_Dx11.h"
#include "SdrToHdr_ProxyBase.h"

#include <d3d11.h>

namespace SdrToHdr
{
// The Direct3D 11 proxy back buffer.
//
// A Direct3D 11 flip-model chain shows the game one buffer: GetBuffer(0) is the one being drawn, DXGI rotates the others
// behind it, and GetBuffer(1..) fail (read from DXGI on WARP and a hardware adapter, tests/sdr_to_hdr_swapchain_dx11_gpu.cpp).
// So the proxy is one texture, GetBuffer(0) returns it and any other index is INVALID_CALL, as the real chain says. At present
// the pass converts it into the real chain's buffer 0 (which is whichever back buffer is current).
//
// The proxy is a typed texture in the game's own format, not a TYPELESS one: a game that makes its target view with a null
// description (the usual way for a UNORM chain) needs a typed resource, and so does a GetDesc that shows the game's format.
// Direct3D 11 has no relaxed format casting: it refuses a UNORM <-> _SRGB view of a typed texture (E_INVALIDARG on WARP and
// hardware), so a game that makes an _SRGB view of a UNORM flip chain's buffer, which a real flip chain allows, gets that
// refusal from the proxy. That is the one known difference; the other choice (TYPELESS) would break the null-description case
// and the format the game reads back, which is far more common.
class ProxyChainDx11 final : public ProxyChainBase
{
  public:
    ProxyChainDx11() = default;
    ~ProxyChainDx11() override { Release(); }

    const char* Present(IUnknown* queueOrContext, IDXGISwapChain3* real, PassConstants constants) override;
    bool OwnsResource(const void* resource, bool& relaxedCasting) override;

  protected:
    const char* BuildResourcesLocked(IUnknown* device, IDXGISwapChain3* real, DXGI_FORMAT gameFormat) override;
    void ReleaseResourcesLocked() override;
    bool ResourcesLocked() const override { return _proxy != nullptr && _pass.Ready(); }
    bool HeldResourcesLocked() override;
    void QuiesceLocked() override;
    HRESULT GetBufferLocked(UINT index, REFIID riid, void** surface) override;

  private:
    Microsoft::WRL::ComPtr<ID3D11Device> _device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> _context;
    ID3D11Texture2D* _proxy = nullptr;
    Dx11Pass _pass;
    DXGI_FORMAT _realFormat = DXGI_FORMAT_UNKNOWN; // the real chain's: scRGB (FP16) or PQ (10 bit) follows from it
    bool _typedSrgb = false;                       // a typed _SRGB proxy: read through its _SRGB view, encoded again
};
} // namespace SdrToHdr
