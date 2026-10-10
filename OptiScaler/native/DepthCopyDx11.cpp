// Not built with the precompiled header: Direct3D only, so tests/nr_depth_copy_dx11_gpu.cpp compiles it alone.
#include "DepthCopyDx11.h"

#include "SharedFrame.h"
#include "F5LowShaderBytecode.h"

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace native
{
void DepthCopyDx11::Release()
{
    _copyUav.Reset();
    _copy.Reset();
    _copyWidth = _copyHeight = 0;
    _copyFailed = false;
    _stageSrv.Reset();
    _stage.Reset();
    _stageWidth = _stageHeight = 0;
    _stageFormat = DXGI_FORMAT_UNKNOWN;
    _stageSamples = { 1, 0 };
    _stageFailed = false;
    _shader.Reset();
    _shaderFailed = false;

    for (int i = 0; i < 2; ++i)
    {
        _resolve[i].Reset();
        _resolveFailed[i] = false;
    }

    _device.Reset();
    _taken = false;
}

ID3D11ComputeShader* DepthCopyDx11::Shader()
{
    if (_shader != nullptr || _shaderFailed)
        return _shader.Get();

    // Compiled ahead of time (native/DepthCopyDx11_Hlsl.h -> native/F5LowShaderBytecode.h): a compile here ran in the
    // game's bind hook, under the depth finder's lock, the first time a non-R32 depth buffer was copied.
    const auto* code = F5LowShaderBytecode::Find("DepthCopy", "CSMain");

    if (code == nullptr ||
        FAILED(_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &_shader)))
    {
        _shader.Reset();
        _shaderFailed = true;
    }

    return _shader.Get();
}

ID3D11ComputeShader* DepthCopyDx11::ResolveShader(bool reversed)
{
    auto& shader = _resolve[reversed ? 1 : 0];
    bool& failed = _resolveFailed[reversed ? 1 : 0];

    if (shader != nullptr || failed)
        return shader.Get();

    const auto* code = F5LowShaderBytecode::Find("DepthResolve", reversed ? "NearestReversed" : "NearestStandard");

    if (code == nullptr ||
        FAILED(_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader)))
    {
        shader.Reset();
        failed = true;
    }

    return shader.Get();
}

const char* DepthCopyDx11::MakeCopy(uint32_t width, uint32_t height)
{
    if (_copy != nullptr && _copyWidth == width && _copyHeight == height)
        return nullptr;

    if (_copyFailed && _copyWidth == width && _copyHeight == height)
        return "making the copy texture failed";

    _copyUav.Reset();
    _copy.Reset();
    _copyWidth = width;
    _copyHeight = height;
    _copyFailed = false;

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc {};
    uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

    if (FAILED(_device->CreateTexture2D(&desc, nullptr, &_copy)) ||
        FAILED(_device->CreateUnorderedAccessView(_copy.Get(), &uavDesc, &_copyUav)))
    {
        _copyUav.Reset();
        _copy.Reset();
        _copyFailed = true;
        return "making the copy texture failed";
    }

    return nullptr;
}

const char* DepthCopyDx11::MakeStage(uint32_t width, uint32_t height, DXGI_FORMAT typeless, DXGI_FORMAT view,
                                     DXGI_SAMPLE_DESC samples)
{
    const bool same = _stageWidth == width && _stageHeight == height && _stageFormat == typeless &&
                      _stageSamples.Count == samples.Count && _stageSamples.Quality == samples.Quality;

    if (_stage != nullptr && same)
        return nullptr;

    if (_stageFailed && same)
        return "making the read copy failed";

    _stageSrv.Reset();
    _stage.Reset();
    _stageWidth = width;
    _stageHeight = height;
    _stageFormat = typeless;
    _stageSamples = samples;
    _stageFailed = false;

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = typeless;
    // A copy between multisampled textures needs the same samples on both sides.
    desc.SampleDesc = samples;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc {};
    srvDesc.Format = view;

    if (samples.Count > 1)
    {
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    }
    else
    {
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
    }

    if (FAILED(_device->CreateTexture2D(&desc, nullptr, &_stage)) ||
        FAILED(_device->CreateShaderResourceView(_stage.Get(), &srvDesc, &_stageSrv)))
    {
        _stageSrv.Reset();
        _stage.Reset();
        _stageFailed = true;
        return "making the read copy failed";
    }

    return nullptr;
}

const char* DepthCopyDx11::Take(ID3D11DeviceContext* context, ID3D11Resource* source, bool reversed)
{
    // Every way out below but the last leaves no copy for this buffer: the copy of an earlier one is not offered for it.
    _taken = false;

    // A deferred context only records: a copy made there would run when the game executes its list, if it does.
    if (context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED)
        return "the context is a deferred one";

    ComPtr<ID3D11Texture2D> sourceTex;

    if (FAILED(source->QueryInterface(IID_PPV_ARGS(&sourceTex))))
        return "the picked buffer is not a 2D texture";

    D3D11_TEXTURE2D_DESC sourceDesc {};
    sourceTex->GetDesc(&sourceDesc);

    DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN, view = DXGI_FORMAT_UNKNOWN;

    if (!SharedDepthFormats(sourceDesc.Format, &typeless, &view))
        return "the picked buffer's format does not map to a depth format that can be read";

    const bool multisampled = sourceDesc.SampleDesc.Count > 1;

    // What is kept between copies is made on the device the buffer was made on, and made again when the game makes a new one.
    ComPtr<ID3D11Device> device;
    source->GetDevice(&device);

    if (device == nullptr)
        return "the picked buffer has no device";

    if (device != _device)
    {
        Release();
        _device = device;
    }

    // An R32 depth (D32_FLOAT, R32_TYPELESS: the most common) is already the copy's format family: copied straight, no pass.
    // A multisampled one is resolved by the pass whatever its format.
    const bool direct = typeless == DXGI_FORMAT_R32_TYPELESS && !multisampled;
    ID3D11ComputeShader* shader = direct ? nullptr : multisampled ? ResolveShader(reversed) : Shader();

    if (!direct && shader == nullptr)
        return multisampled ? "making the resolve pass failed" : "making the conversion pass failed";

    if (const char* failed = MakeCopy(sourceDesc.Width, sourceDesc.Height))
        return failed;

    // Only the first subresource is copied: a whole-resource copy of a buffer with mips or slices into one with neither would
    // be dropped by the runtime without a word.
    if (direct)
    {
        context->CopySubresourceRegion(_copy.Get(), 0, 0, 0, 0, source, 0, nullptr);
        _taken = true;
        _converted = false;
        _samples = 1;
        return nullptr;
    }

    if (const char* failed = MakeStage(sourceDesc.Width, sourceDesc.Height, typeless, view, sourceDesc.SampleDesc))
        return failed;

    context->CopySubresourceRegion(_stage.Get(), 0, 0, 0, 0, source, 0, nullptr);

    // The game's own compute state: this runs in the middle of its frame, so what it had bound at slot 0 is put back after.
    ComPtr<ID3D11ComputeShader> gameShader;
    ID3D11ClassInstance* gameInstances[D3D11_SHADER_MAX_INTERFACES] {};
    UINT gameInstanceCount = D3D11_SHADER_MAX_INTERFACES;
    ComPtr<ID3D11ShaderResourceView> gameSrv;
    ComPtr<ID3D11UnorderedAccessView> gameUav;
    context->CSGetShader(&gameShader, gameInstances, &gameInstanceCount);
    context->CSGetShaderResources(0, 1, &gameSrv);
    context->CSGetUnorderedAccessViews(0, 1, &gameUav);

    // -1 keeps an append or counter UAV's hidden count as it is.
    const UINT keepCount = (UINT) -1;
    ID3D11ShaderResourceView* const stageSrv = _stageSrv.Get();
    ID3D11UnorderedAccessView* const copyUav = _copyUav.Get();

    context->CSSetShader(shader, nullptr, 0);
    context->CSSetShaderResources(0, 1, &stageSrv);
    context->CSSetUnorderedAccessViews(0, 1, &copyUav, &keepCount);
    context->Dispatch((sourceDesc.Width + 7) / 8, (sourceDesc.Height + 7) / 8, 1);

    context->CSSetShaderResources(0, 1, gameSrv.GetAddressOf());
    context->CSSetUnorderedAccessViews(0, 1, gameUav.GetAddressOf(), &keepCount);
    context->CSSetShader(gameShader.Get(), gameInstances, gameInstanceCount);

    for (UINT i = 0; i < gameInstanceCount; ++i)
    {
        if (gameInstances[i] != nullptr)
            gameInstances[i]->Release();
    }

    _taken = true;
    _converted = true;
    _samples = sourceDesc.SampleDesc.Count;
    return nullptr;
}
} // namespace native
