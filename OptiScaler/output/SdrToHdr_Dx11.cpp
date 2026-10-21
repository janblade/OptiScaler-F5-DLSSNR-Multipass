// Not built with the precompiled header: Direct3D only, so tests/sdr_to_hdr_dx11_gpu.cpp compiles it alone.
#include "SdrToHdr_Dx11.h"

#include "../native/F5LowShaderBytecode.h"

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace SdrToHdr
{
namespace
{
// A shader stage as the game had it bound, class instances included (as native/MsaaPictureDx11.cpp keeps them).
template <typename Shader> struct SavedShader
{
    ComPtr<Shader> shader;
    ID3D11ClassInstance* instances[D3D11_SHADER_MAX_INTERFACES] {};
    UINT instanceCount = D3D11_SHADER_MAX_INTERFACES;

    ~SavedShader()
    {
        for (UINT i = 0; i < instanceCount; ++i)
        {
            if (instances[i] != nullptr)
                instances[i]->Release();
        }
    }
};
} // namespace

void Dx11Pass::ReleaseViews()
{
    _srv.Reset();
    _srvProxy = nullptr;
}

void Dx11Pass::Release()
{
    ReleaseViews();
    _vs.Reset();
    _ps.Reset();
    _constants.Reset();
    _rasterizer.Reset();
    _blend.Reset();
    _depth.Reset();
    _device.Reset();
}

const char* Dx11Pass::Init(ID3D11Device* device)
{
    if (device == nullptr)
        return "the HDR pass was given no Direct3D 11 device";

    if (_device.Get() == device && _ps != nullptr)
        return nullptr;

    Release();
    _device = device;

    // Compiled ahead of time (output/SdrToHdr_Hlsl.h -> native/F5LowShaderBytecode.h): a compile here would run on the game's
    // present thread.
    const auto* vsCode = F5LowShaderBytecode::Find("SdrToHdr", "FullScreenVS");
    const auto* psCode = F5LowShaderBytecode::Find("SdrToHdr", "SdrToHdrPS");

    if (vsCode == nullptr || psCode == nullptr)
    {
        Release();
        return "the HDR pass's shaders are missing";
    }

    D3D11_BUFFER_DESC bufferDesc {};
    bufferDesc.ByteWidth = sizeof(PassConstants);
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_RASTERIZER_DESC rasterizer {};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;

    D3D11_BLEND_DESC blend {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    D3D11_DEPTH_STENCIL_DESC depth {};
    depth.DepthEnable = FALSE;
    depth.StencilEnable = FALSE;

    if (FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &_vs)) ||
        FAILED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &_ps)) ||
        FAILED(device->CreateBuffer(&bufferDesc, nullptr, &_constants)) ||
        FAILED(device->CreateRasterizerState(&rasterizer, &_rasterizer)) ||
        FAILED(device->CreateBlendState(&blend, &_blend)) || FAILED(device->CreateDepthStencilState(&depth, &_depth)))
    {
        Release();
        return "the HDR pass's pipeline could not be made";
    }

    return nullptr;
}

const char* Dx11Pass::Run(ID3D11DeviceContext* context, ID3D11Texture2D* proxy, ID3D11Texture2D* real,
                          const PassConstants& constants)
{
    if (_ps == nullptr)
        return "the HDR pass is not ready";

    if (context == nullptr || proxy == nullptr || real == nullptr)
        return "the HDR pass was given nothing to convert";

    D3D11_TEXTURE2D_DESC proxyDesc {}, realDesc {};
    proxy->GetDesc(&proxyDesc);
    real->GetDesc(&realDesc);

    if (realDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && realDesc.Format != DXGI_FORMAT_R10G10B10A2_UNORM)
        return "the HDR pass cannot write this back buffer format";

    if (proxyDesc.Width != realDesc.Width || proxyDesc.Height != realDesc.Height)
        return "the HDR pass's back buffers differ in size";

    if ((proxyDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 || (realDesc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0)
        return "the HDR pass's back buffers lack a read or a render bind";

    // The proxy's read view is kept while the proxy stays the same. Its own format: raw bits for a UNORM proxy, the _SRGB view
    // (decoded, constants.srcLinear says so) for a typed _SRGB one -- Direct3D 11 does not let either be viewed as the other.
    if (_srv == nullptr || _srvProxy != proxy)
    {
        ReleaseViews();

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc {};
        srvDesc.Format = proxyDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        if (FAILED(_device->CreateShaderResourceView(proxy, &srvDesc, &_srv)))
        {
            _srv.Reset();
            return "the HDR pass could not read the proxy back buffer";
        }

        _srvProxy = proxy;
    }

    // The real buffer's view is made for this draw and let go of at the end: the swap chain cannot be resized while any
    // view of its buffer lives
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc {};
    rtvDesc.Format = realDesc.Format;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> rtv;

    if (FAILED(_device->CreateRenderTargetView(real, &rtvDesc, &rtv)))
        return "the HDR pass could not write the real back buffer";

    D3D11_MAPPED_SUBRESOURCE mapped {};

    if (FAILED(context->Map(_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return "the HDR pass could not set its constants";

    std::memcpy(mapped.pData, &constants, sizeof(PassConstants));
    context->Unmap(_constants.Get(), 0);

    // Everything the draw changes, as the game had it
    ComPtr<ID3D11InputLayout> gameLayout;
    D3D11_PRIMITIVE_TOPOLOGY gameTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    SavedShader<ID3D11VertexShader> gameVs;
    SavedShader<ID3D11HullShader> gameHs;
    SavedShader<ID3D11DomainShader> gameDs;
    SavedShader<ID3D11GeometryShader> gameGs;
    SavedShader<ID3D11PixelShader> gamePs;
    ComPtr<ID3D11ShaderResourceView> gameSrv;
    ComPtr<ID3D11Buffer> gameConstants;
    ComPtr<ID3D11RasterizerState> gameRasterizer;
    ComPtr<ID3D11Predicate> gamePredicate;
    BOOL gamePredicateValue = FALSE;
    ComPtr<ID3D11BlendState> gameBlend;
    FLOAT gameBlendFactor[4] {};
    UINT gameSampleMask = 0xffffffff;
    ComPtr<ID3D11DepthStencilState> gameDepthState;
    UINT gameStencilRef = 0;
    UINT viewportCount = 0;
    context->RSGetViewports(&viewportCount, nullptr);
    D3D11_VIEWPORT gameViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] {};
    viewportCount = viewportCount < D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE
                        ? viewportCount
                        : D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    context->RSGetViewports(&viewportCount, gameViewports);

    ComPtr<ID3D11RenderTargetView> gameRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ComPtr<ID3D11DepthStencilView> gameDsv;
    ComPtr<ID3D11UnorderedAccessView> gameUavs[D3D11_PS_CS_UAV_REGISTER_COUNT];
    ID3D11RenderTargetView* rawRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] {};

    context->IAGetInputLayout(&gameLayout);
    context->IAGetPrimitiveTopology(&gameTopology);
    context->VSGetShader(&gameVs.shader, gameVs.instances, &gameVs.instanceCount);
    context->HSGetShader(&gameHs.shader, gameHs.instances, &gameHs.instanceCount);
    context->DSGetShader(&gameDs.shader, gameDs.instances, &gameDs.instanceCount);
    context->GSGetShader(&gameGs.shader, gameGs.instances, &gameGs.instanceCount);
    context->PSGetShader(&gamePs.shader, gamePs.instances, &gamePs.instanceCount);
    context->PSGetShaderResources(0, 1, &gameSrv);
    context->PSGetConstantBuffers(0, 1, &gameConstants);
    context->RSGetState(&gameRasterizer);
    context->GetPredication(&gamePredicate, &gamePredicateValue);
    context->OMGetBlendState(&gameBlend, gameBlendFactor, &gameSampleMask);
    context->OMGetDepthStencilState(&gameDepthState, &gameStencilRef);

    // Render targets and pixel shader UAVs share their slots; read separately, since each read leaves the other out.
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rawRtvs, &gameDsv);

    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        gameRtvs[i].Attach(rawRtvs[i]);

    ID3D11UnorderedAccessView* rawUavs[D3D11_PS_CS_UAV_REGISTER_COUNT] {};
    context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, D3D11_PS_CS_UAV_REGISTER_COUNT, rawUavs);

    for (UINT i = 0; i < D3D11_PS_CS_UAV_REGISTER_COUNT; ++i)
        gameUavs[i].Attach(rawUavs[i]);

    D3D11_VIEWPORT viewport {};
    viewport.Width = (FLOAT) realDesc.Width;
    viewport.Height = (FLOAT) realDesc.Height;
    viewport.MaxDepth = 1.0f;

    const FLOAT blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    ID3D11RenderTargetView* const target = rtv.Get();
    ID3D11ShaderResourceView* const read = _srv.Get();
    ID3D11Buffer* const constantBuffer = _constants.Get();

    // The depth view is left out (a game's depth buffer may not match the back buffer, and a mismatch drops the draw); no
    // stage but the vertex shader and the pixel shader runs.
    // The game's own outputs come off first. At present it usually still has a view of the proxy bound as its render target
    // (or a pixel shader UAV on it): binding the proxy as a read view while that is bound makes Direct3D 11 force the read
    // view to NULL ("still bound on output") and the pass would read black. OMSetRenderTargets(0, ...) drops the targets and
    // the pixel shader UAVs; the real buffer's target view and then the read view go on after.
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->SetPredication(nullptr, FALSE); // (a game's predicate would skip the draw)

    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(_vs.Get(), nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->PSSetShader(_ps.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &constantBuffer);
    context->RSSetState(_rasterizer.Get());
    context->RSSetViewports(1, &viewport);
    context->OMSetBlendState(_blend.Get(), blendFactor, 0xffffffff);
    context->OMSetDepthStencilState(_depth.Get(), 0);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->PSSetShaderResources(0, 1, &read); // (after the outputs: the proxy is no longer bound as one)
    context->Draw(3, 0);

    // Back as the game had them. The render targets and the UAVs share slots: the targets are the leading slots, the UAVs start
    // at the first one bound (a game that binds both has them apart, or the runtime would not have taken them).
    context->SetPredication(gamePredicate.Get(), gamePredicateValue);
    // (the read view of the proxy comes off before the game's targets, which may be views of the proxy, go back on)
    context->PSSetShaderResources(0, 1, gameSrv.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, gameConstants.GetAddressOf());
    context->OMSetDepthStencilState(gameDepthState.Get(), gameStencilRef);
    context->OMSetBlendState(gameBlend.Get(), gameBlendFactor, gameSampleMask);
    context->RSSetViewports(viewportCount, gameViewports);
    context->RSSetState(gameRasterizer.Get());
    context->PSSetShader(gamePs.shader.Get(), gamePs.instances, gamePs.instanceCount);
    context->GSSetShader(gameGs.shader.Get(), gameGs.instances, gameGs.instanceCount);
    context->DSSetShader(gameDs.shader.Get(), gameDs.instances, gameDs.instanceCount);
    context->HSSetShader(gameHs.shader.Get(), gameHs.instances, gameHs.instanceCount);
    context->VSSetShader(gameVs.shader.Get(), gameVs.instances, gameVs.instanceCount);
    context->IASetPrimitiveTopology(gameTopology);
    context->IASetInputLayout(gameLayout.Get());

    UINT rtvCount = 0, firstUav = D3D11_PS_CS_UAV_REGISTER_COUNT, lastUav = 0;

    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
    {
        if (gameRtvs[i] != nullptr)
            rtvCount = i + 1;

        rawRtvs[i] = gameRtvs[i].Get();
    }

    for (UINT i = 0; i < D3D11_PS_CS_UAV_REGISTER_COUNT; ++i)
    {
        rawUavs[i] = gameUavs[i].Get();

        if (rawUavs[i] != nullptr)
        {
            firstUav = firstUav < i ? firstUav : i;
            lastUav = i;
        }
    }

    if (firstUav > lastUav)
    {
        context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rawRtvs, gameDsv.Get());
    }
    else
    {
        // -1 keeps an append or counter UAV's hidden count as it is.
        const UINT keepCounts[D3D11_PS_CS_UAV_REGISTER_COUNT] = { (UINT) -1, (UINT) -1, (UINT) -1, (UINT) -1,
                                                                    (UINT) -1, (UINT) -1, (UINT) -1, (UINT) -1 };
        context->OMSetRenderTargetsAndUnorderedAccessViews(rtvCount < firstUav ? rtvCount : firstUav, rawRtvs,
                                                           gameDsv.Get(), firstUav, lastUav - firstUav + 1,
                                                           rawUavs + firstUav, keepCounts);
    }

    return nullptr;
}
} // namespace SdrToHdr
