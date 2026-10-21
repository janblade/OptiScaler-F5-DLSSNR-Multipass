#pragma once

// Direct3D 11 only (no OptiScaler headers but SdrToHdr_Pass.h and native/F5LowShaderBytecode.h), so
// tests/sdr_to_hdr_dx11_gpu.cpp builds it alone.
#include "SdrToHdr_Pass.h"

#include <d3d11.h>
#include <wrl/client.h>

namespace SdrToHdr
{
// One full-screen draw per frame on the game's immediate context: the proxy back buffer the game drew in (read raw through a
// view of its own format; a typed _SRGB proxy is read through its _SRGB view and PassConstants::srcLinear says so) -> the
// real back buffer (R16G16B16A16_FLOAT for scRGB, R10G10B10A2_UNORM for HDR10). The same shaders as the Direct3D 12 pass
// (native/F5LowShaderBytecode.h group "SdrToHdr"); b0 is a 32 byte constant buffer instead of root constants.
//
// The context is the game's, so the pass puts back everything it touches: input layout, topology, the shader stages with their
// class instances, pixel shader read view 0 and constant buffer 0, the rasterizer, blend and depth-stencil states, the
// viewports, the render targets, the depth view and the pixel shader UAVs. The same list, and the same way of reading and
// restoring it, as native/MsaaPictureDx11.cpp's WriteBack (the existing Direct3D 11 pass of the codebase).
//
// It keeps the proxy's read view between frames (ReleaseViews drops it: a swap chain cannot be resized while anything holds one
// of its buffers, and the proxy is the game's buffer to release); the real buffer's target view lives only inside Run. Not
// thread-safe: the caller guards it (the swap chain's present).
class Dx11Pass
{
  public:
    Dx11Pass() = default;
    ~Dx11Pass() { Release(); }
    Dx11Pass(const Dx11Pass&) = delete;
    Dx11Pass& operator=(const Dx11Pass&) = delete;

    // Makes the shaders, the states and the constant buffer on `device`. nullptr when ready, otherwise why not (a fixed string).
    // Again for the same device is a no-op.
    const char* Init(ID3D11Device* device);

    // Converts proxy -> real on `context` (the immediate context of the device). nullptr when done.
    const char* Run(ID3D11DeviceContext* context, ID3D11Texture2D* proxy, ID3D11Texture2D* real,
                    const PassConstants& constants);

    // Lets go of the proxy's read view (before the proxy is released or the chain resized).
    void ReleaseViews();

    // Lets go of everything, and of the device.
    void Release();

    bool Ready() const { return _ps != nullptr; }

  private:
    Microsoft::WRL::ComPtr<ID3D11Device> _device; // what everything below was made on
    Microsoft::WRL::ComPtr<ID3D11VertexShader> _vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer> _constants;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer;
    Microsoft::WRL::ComPtr<ID3D11BlendState> _blend;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> _depth;
    ID3D11Texture2D* _srvProxy = nullptr; // the proxy _srv reads (identity only: the view holds it)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _srv;
};
} // namespace SdrToHdr
