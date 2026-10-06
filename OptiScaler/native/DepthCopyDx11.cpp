// Not built with the precompiled header: Direct3D only, so tests/nr_depth_copy_dx11_gpu.cpp compiles it alone.
#include "DepthCopyDx11.h"

#include "SharedFrame.h"

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace native
{
namespace
{
// Reads the read copy (whatever its depth format family) through a single-channel view and writes the plain R32_FLOAT copy. A
// typeless depth-stencil format (R32G8X24_TYPELESS and the like) can fail to make a cross-API (D3D11<->D3D12) shared NT handle
// outright (CreateTexture2D returns E_INVALIDARG for such a format with D3D11_RESOURCE_MISC_SHARED_NTHANDLE, even though the same
// device shares an ordinary colour texture of that size), where a plain float texture shares without issue.
const char* kConvertSource = R"HLSL(
Texture2D<float> Src : register(t0);
RWTexture2D<float> Dst : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    Dst.GetDimensions(w, h);

    if (id.x >= w || id.y >= h)
        return;

    Dst[id.xy] = Src.Load(int3(id.xy, 0));
}
)HLSL";
} // namespace

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
    _stageFailed = false;
    _shader.Reset();
    _shaderFailed = false;
    _device.Reset();
    _taken = false;
}

ID3D11ComputeShader* DepthCopyDx11::Shader()
{
    if (_shader != nullptr || _shaderFailed || _compileFailed)
        return _shader.Get();

    if (_code == nullptr &&
        FAILED(D3DCompile(kConvertSource, strlen(kConvertSource), "DepthCopy", nullptr, nullptr, "CSMain", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &_code, nullptr)))
    {
        _code.Reset();
        _compileFailed = true;
        return nullptr;
    }

    if (FAILED(_device->CreateComputeShader(_code->GetBufferPointer(), _code->GetBufferSize(), nullptr, &_shader)))
    {
        _shader.Reset();
        _shaderFailed = true;
    }

    return _shader.Get();
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

const char* DepthCopyDx11::MakeStage(uint32_t width, uint32_t height, DXGI_FORMAT typeless, DXGI_FORMAT view)
{
    const bool same = _stageWidth == width && _stageHeight == height && _stageFormat == typeless;

    if (_stage != nullptr && same)
        return nullptr;

    if (_stageFailed && same)
        return "making the read copy failed";

    _stageSrv.Reset();
    _stage.Reset();
    _stageWidth = width;
    _stageHeight = height;
    _stageFormat = typeless;
    _stageFailed = false;

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = typeless;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc {};
    srvDesc.Format = view;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

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

const char* DepthCopyDx11::Take(ID3D11DeviceContext* context, ID3D11Resource* source)
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

    // A multisampled buffer would need a resolve.
    if (sourceDesc.SampleDesc.Count > 1)
        return "the picked buffer is multisampled";

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
    const bool direct = typeless == DXGI_FORMAT_R32_TYPELESS;
    ID3D11ComputeShader* shader = direct ? nullptr : Shader();

    if (!direct && shader == nullptr)
        return _compileFailed ? "compiling the conversion pass failed" : "making the conversion pass failed";

    if (const char* failed = MakeCopy(sourceDesc.Width, sourceDesc.Height))
        return failed;

    // Only the first subresource is copied: a whole-resource copy of a buffer with mips or slices into one with neither would
    // be dropped by the runtime without a word.
    if (direct)
    {
        context->CopySubresourceRegion(_copy.Get(), 0, 0, 0, 0, source, 0, nullptr);
        _taken = true;
        _converted = false;
        return nullptr;
    }

    if (const char* failed = MakeStage(sourceDesc.Width, sourceDesc.Height, typeless, view))
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
    return nullptr;
}
} // namespace native
