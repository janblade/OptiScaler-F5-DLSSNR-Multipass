// Not built with the precompiled header: Direct3D only, so tests/nr_shared_frame_gpu.cpp compiles it alone.
#include "SharedFrame.h"

#include <format>

using Microsoft::WRL::ComPtr;

namespace native
{

DXGI_FORMAT SharedPictureFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return format;
    }
}

bool SharedDepthFormats(DXGI_FORMAT depth, DXGI_FORMAT* typeless, DXGI_FORMAT* view)
{
    switch (depth)
    {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
        *typeless = DXGI_FORMAT_R32_TYPELESS;
        *view = DXGI_FORMAT_R32_FLOAT;
        return true;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        *typeless = DXGI_FORMAT_R32G8X24_TYPELESS;
        *view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        return true;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
        *typeless = DXGI_FORMAT_R24G8_TYPELESS;
        *view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        return true;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
        *typeless = DXGI_FORMAT_R16_TYPELESS;
        *view = DXGI_FORMAT_R16_UNORM;
        return true;
    default:
        return false;
    }
}

bool SharedTexture::Create(ID3D11Device* device11, uint32_t width, uint32_t height, DXGI_FORMAT format, UINT bindFlags)
{
    Reset();

    if (device11 == nullptr)
    {
        _error = "no D3D11 device";
        return false;
    }

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bindFlags;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    HRESULT hr = device11->CreateTexture2D(&desc, nullptr, &_tex11);

    if (FAILED(hr))
    {
        _error = std::format("creating a shared texture {}x{} format {}: {:X}", width, height, (int) format, (UINT) hr);
        _tex11.Reset();
        return false;
    }

    ComPtr<IDXGIResource1> dxgi;
    hr = _tex11.As(&dxgi);

    if (SUCCEEDED(hr))
        hr = dxgi->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &_handle);

    if (FAILED(hr) || _handle == nullptr)
    {
        _error = std::format("making the shared handle: {:X}", (UINT) hr);
        Reset();
        return false;
    }

    _width = width;
    _height = height;
    _format = format;
    return true;
}

bool SharedTexture::Open(ID3D12Device* device12)
{
    if (_res12 != nullptr && _opened == device12)
        return true;

    if (_handle == nullptr || device12 == nullptr)
        return false;

    _res12.Reset();
    const HRESULT hr = device12->OpenSharedHandle(_handle, IID_PPV_ARGS(&_res12));

    if (FAILED(hr))
    {
        _error = std::format("opening the shared texture on D3D12: {:X}", (UINT) hr);
        _res12.Reset();
        return false;
    }

    _opened = device12;
    return true;
}

void SharedTexture::Reset()
{
    _res12.Reset();
    _tex11.Reset();

    if (_handle != nullptr)
    {
        CloseHandle(_handle);
        _handle = nullptr;
    }

    _opened = nullptr;
    _width = _height = 0;
    _format = DXGI_FORMAT_UNKNOWN;
}

bool SharedFence::Create(ID3D12Device* device12, ID3D11Device* device11)
{
    Reset();

    if (device12 == nullptr || device11 == nullptr)
    {
        _error = "no device";
        return false;
    }

    ComPtr<ID3D11Device5> device5;

    if (FAILED(device11->QueryInterface(IID_PPV_ARGS(&device5))))
    {
        _error = "the D3D11 device cannot open shared fences (needs Windows 10 1703 or later)";
        return false;
    }

    HRESULT hr = device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&_fence12));

    if (FAILED(hr))
    {
        _error = std::format("creating the shared fence: {:X}", (UINT) hr);
        return false;
    }

    HANDLE handle = nullptr;
    hr = device12->CreateSharedHandle(_fence12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);

    if (SUCCEEDED(hr))
    {
        hr = device5->OpenSharedFence(handle, IID_PPV_ARGS(&_fence11));
        CloseHandle(handle);
    }

    if (FAILED(hr))
    {
        _error = std::format("opening the shared fence on D3D11: {:X}", (UINT) hr);
        Reset();
        return false;
    }

    return true;
}

void SharedFence::Reset()
{
    _fence11.Reset();
    _fence12.Reset();
    _value = 0;
}

} // namespace native
