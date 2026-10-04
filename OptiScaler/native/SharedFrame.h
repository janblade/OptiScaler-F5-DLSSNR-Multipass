#pragma once

// The shared-texture transport between a D3D11 game device and the D3D12 device the native input producer runs on (the same
// two devices IFeature_Dx11wDx12 bridges for a game that calls an upscaler). Shared by every adapter whose game API is not
// D3D12: a texture made on the game's device that the D3D12 device opens, and one fence both devices use to order their work
// on the GPU, with no CPU wait.
//
// Textures: made on the D3D11 side with NT shared handles, opened on the D3D12 side on first use. The D3D12 resource rests in
// COMMON; the producer transitions it as it needs.
//
// The fence: made on the D3D12 device (shared), opened on the D3D11 side (ID3D11Device5::OpenSharedFence, Windows 10 1703+).
// One timeline serves both directions, as long as every signal is a higher value than the last: D3D11 signals "the copies are
// done", D3D12 waits; D3D12 signals "NR is done", D3D11 waits. Both calls are GPU-side.

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace native
{

// The format a picture is copied through: sRGB and typeless forms of a 4-channel format become the plain UNORM one (a copy
// between them is allowed, the bits are the same, and the producer reads the picture as encoded values).
DXGI_FORMAT SharedPictureFormat(DXGI_FORMAT backBufferFormat);

// The typeless format a depth format is copied in, and the format its depth plane is read through on the D3D12 side. False for
// a format that is not depth.
bool SharedDepthFormats(DXGI_FORMAT depth, DXGI_FORMAT* typeless, DXGI_FORMAT* view);

class SharedTexture
{
  public:
    // Makes a texture on `device11` another device can open. Replaces what was there.
    bool Create(ID3D11Device* device11, uint32_t width, uint32_t height, DXGI_FORMAT format, UINT bindFlags);

    // Opens it on the D3D12 device (once per device).
    bool Open(ID3D12Device* device12);

    void Reset();

    ID3D11Texture2D* Tex11() const { return _tex11.Get(); }
    ID3D12Resource* Res12() const { return _res12.Get(); }
    uint32_t Width() const { return _width; }
    uint32_t Height() const { return _height; }
    DXGI_FORMAT Format() const { return _format; }
    bool Matches(uint32_t width, uint32_t height, DXGI_FORMAT format) const
    {
        return _tex11 != nullptr && _width == width && _height == height && _format == format;
    }

    const std::string& Error() const { return _error; }

  private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _tex11;
    Microsoft::WRL::ComPtr<ID3D12Resource> _res12;
    HANDLE _handle = nullptr;
    ID3D12Device* _opened = nullptr;
    uint32_t _width = 0;
    uint32_t _height = 0;
    DXGI_FORMAT _format = DXGI_FORMAT_UNKNOWN;
    std::string _error;
};

class SharedFence
{
  public:
    // Makes the fence on `device12` and opens it on `device11`. False when the D3D11 device cannot open shared fences.
    bool Create(ID3D12Device* device12, ID3D11Device* device11);
    void Reset();

    bool Ready() const { return _fence12 != nullptr && _fence11 != nullptr; }
    ID3D12Fence* Fence12() const { return _fence12.Get(); }
    ID3D11Fence* Fence11() const { return _fence11.Get(); }

    // The next value to signal: always higher than any before.
    uint64_t Next() { return ++_value; }
    uint64_t Value() const { return _value; }

    const std::string& Error() const { return _error; }

  private:
    Microsoft::WRL::ComPtr<ID3D12Fence> _fence12;
    Microsoft::WRL::ComPtr<ID3D11Fence> _fence11;
    uint64_t _value = 0;
    std::string _error;
};

} // namespace native
