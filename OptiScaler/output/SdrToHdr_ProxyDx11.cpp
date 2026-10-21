// Not built with the precompiled header: Direct3D and DXGI only, so tests/sdr_to_hdr_swapchain_dx11_gpu.cpp compiles it
// alone.
#include "SdrToHdr_ProxyDx11.h"

namespace SdrToHdr
{
namespace
{
bool IsSrgb(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}
} // namespace

bool ProxyChainDx11::OwnsResource(const void* resource, bool& relaxedCasting)
{
    std::lock_guard<std::recursive_mutex> lock(_lock);
    relaxedCasting = false; // none on Direct3D 11

    return resource != nullptr && resource == _proxy;
}

void ProxyChainDx11::QuiesceLocked()
{
    // Nothing of ours may hold the proxy when the game's holders are counted
    _pass.ReleaseViews();

    if (_context != nullptr)
        _context->Flush();
}

void ProxyChainDx11::ReleaseResourcesLocked()
{
    _pass.ReleaseViews();

    if (_proxy != nullptr)
    {
        _proxy->Release();
        _proxy = nullptr;
    }

    // The runtime frees what was released at the next flush, and a resize wants every reference to the buffers gone
    if (_context != nullptr)
        _context->Flush();

    _pass.Release();
    _context.Reset();
    _device.Reset();
    _typedSrgb = false;
}

bool ProxyChainDx11::HeldResourcesLocked()
{
    if (_proxy == nullptr)
        return false;

    _proxy->AddRef();
    return _proxy->Release() > 1;
}

HRESULT ProxyChainDx11::GetBufferLocked(UINT index, REFIID riid, void** surface)
{
    // A flip chain shows buffer 0 only
    if (index != 0 || _proxy == nullptr)
    {
        *surface = nullptr;
        return DXGI_ERROR_INVALID_CALL;
    }

    return _proxy->QueryInterface(riid, surface);
}

const char* ProxyChainDx11::BuildResourcesLocked(IUnknown* deviceUnknown, IDXGISwapChain3* real, DXGI_FORMAT gameFormat)
{
    ID3D11Device* device = nullptr;

    if (deviceUnknown == nullptr || FAILED(deviceUnknown->QueryInterface(IID_PPV_ARGS(&device))) || device == nullptr)
        return "the HDR proxy was given no Direct3D 11 device";

    _device.Attach(device);
    _device->GetImmediateContext(&_context);

    DXGI_SWAP_CHAIN_DESC desc {};

    if (FAILED(real->GetDesc(&desc)) || desc.BufferDesc.Width == 0 || desc.BufferDesc.Height == 0 ||
        desc.SampleDesc.Count != 1)
        return "the HDR proxy could not read the swap chain's size";

    // The game's size and format, the binds a back buffer needs: it draws to it, and the pass reads it (plus what the game's
    // BufferUsage asks for)
    D3D11_TEXTURE2D_DESC texture {};
    texture.Width = desc.BufferDesc.Width;
    texture.Height = desc.BufferDesc.Height;
    texture.MipLevels = 1;
    texture.ArraySize = 1;
    texture.Format = gameFormat;
    texture.SampleDesc.Count = 1;
    texture.Usage = D3D11_USAGE_DEFAULT;
    texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if ((desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) != 0)
        texture.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

    if (FAILED(_device->CreateTexture2D(&texture, nullptr, &_proxy)) || _proxy == nullptr)
        return "the HDR proxy back buffer could not be made";

    _realFormat = desc.BufferDesc.Format;
    _typedSrgb = IsSrgb(gameFormat);

    return _pass.Init(_device.Get());
}

const char* ProxyChainDx11::Present(IUnknown* queueOrContext, IDXGISwapChain3* real, PassConstants constants)
{
    std::lock_guard<std::recursive_mutex> lock(_lock);

    if (_proxy == nullptr || !_pass.Ready())
        return "the HDR proxy is not set up";

    ID3D11DeviceContext* context = nullptr;

    if (queueOrContext == nullptr || FAILED(queueOrContext->QueryInterface(IID_PPV_ARGS(&context))) || context == nullptr)
        return "the HDR pass was given no Direct3D 11 context";

    context->Release(); // the caller's object keeps it alive

    ID3D11Texture2D* realBuffer = nullptr;

    if (FAILED(real->GetBuffer(0, IID_PPV_ARGS(&realBuffer))) || realBuffer == nullptr)
        return "the HDR pass could not get the real back buffer";

    constants.srcLinear = _typedSrgb ? 1u : 0u;
    constants.mode = _realFormat == DXGI_FORMAT_R10G10B10A2_UNORM ? 1u : 0u; // follows the chain, not the live setting
    const char* why = _pass.Run(context, _proxy, realBuffer, constants);
    realBuffer->Release(); // the pass holds no reference between frames: a resize must find none
    return why;
}
} // namespace SdrToHdr
