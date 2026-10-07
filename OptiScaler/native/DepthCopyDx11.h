#pragma once

// Direct3D only (no OptiScaler headers but native/SharedFrame.h), so tests/nr_depth_copy_dx11_gpu.cpp builds it with
// native/SharedFrame.cpp alone.
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

namespace native
{
// One plain R32_FLOAT copy of a game's depth buffer, taken on a D3D11 immediate context in the middle of the game's frame: the
// D3D11 depth finder's copy (resource_tracking/GenericDepth_Dx11.cpp), moved here so it can be checked on its own.
//
// The buffer is most often still bound for depth writes when it is copied, and D3D11 binds no read view of a resource bound for
// writing (the view is quietly made null), so a pass reading it directly reads nothing. It is copied as it is first (a copy may
// read a bound depth buffer) into a texture of its own format family that can be read, then a small pass converts that into the
// R32_FLOAT copy. An R32 depth buffer is already the copy's format family and is copied straight into it. Only the first
// subresource (top mip, first slice) is copied.
//
// The game's compute shader (with its class instances), CS read view 0 and CS write view 0 are put back after the pass; nothing
// else of the game's state is touched. Not thread-safe: the caller guards it.
class DepthCopyDx11
{
  public:
    DepthCopyDx11() = default;
    DepthCopyDx11(const DepthCopyDx11&) = delete;
    DepthCopyDx11& operator=(const DepthCopyDx11&) = delete;

    // Copies `source` (a depth buffer made on the device `context` belongs to) into the copy. Returns nullptr when the copy was
    // taken, otherwise why not (a fixed string: the same reason gives the same pointer, so a caller can log each one once); a
    // copy not taken is not offered (Taken() is false) until a later Take succeeds. What is kept between copies is made again
    // when the size, format or device changes; a texture that could not be made is not tried again until one of those changes.
    const char* Take(ID3D11DeviceContext* context, ID3D11Resource* source);

    // Lets go of everything made on the device, and of the device.
    void Release();

    // No copy for the frame (the caller decided not to take one): the copy of an earlier buffer is not offered for it.
    void Forget() { _taken = false; }

    bool Taken() const { return _taken; }
    ID3D11Texture2D* Copy() const { return _copy.Get(); }
    uint32_t Width() const { return _copyWidth; }
    uint32_t Height() const { return _copyHeight; }
    // Whether the copy just taken went through the conversion pass (false: copied straight).
    bool Converted() const { return _converted; }

  private:
    const char* MakeCopy(uint32_t width, uint32_t height);
    const char* MakeStage(uint32_t width, uint32_t height, DXGI_FORMAT typeless, DXGI_FORMAT view);
    ID3D11ComputeShader* Shader();

    Microsoft::WRL::ComPtr<ID3D11Device> _device; // what everything below was made on
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _copy;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _copyUav;
    uint32_t _copyWidth = 0, _copyHeight = 0;
    bool _copyFailed = false; // making the copy at _copyWidth x _copyHeight failed

    Microsoft::WRL::ComPtr<ID3D11Texture2D> _stage;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _stageSrv;
    uint32_t _stageWidth = 0, _stageHeight = 0;
    DXGI_FORMAT _stageFormat = DXGI_FORMAT_UNKNOWN;
    bool _stageFailed = false; // making the read copy at the size and format above failed

    Microsoft::WRL::ComPtr<ID3D11ComputeShader> _shader; // made on _device
    bool _shaderFailed = false;

    bool _taken = false;
    bool _converted = false;
};
} // namespace native
