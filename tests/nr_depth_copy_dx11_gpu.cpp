// GPU test for native/DepthCopyDx11.cpp, the D3D11 depth finder's copy, no game: depth buffers of every family the finder reads
// are cleared to a value while bound for depth writes (as a game has them when the copy is taken) and copied; the copy must
// read that value back.
//   - D32_FLOAT, R32_TYPELESS with mips (copied straight), D24S8 and D32S8 typed and typeless with mips, D16 (converted),
//   - the game's compute shader, CS read view 0 and CS write view 0 are what they were after the copy, set or empty,
//   - a multisampled buffer with an edge (4 samples) is resolved to the nearest sample: the larger value with reversed Z, the
//     smaller with standard Z, for D32 and D24S8,
//   - a buffer that cannot be copied (a colour format, a deferred context) leaves no copy, even after a good one,
//   - a new size, and a buffer of another device, make the copy again on the right device,
//   - with the debug layer installed, the runtime reports no error or warning for any of it.
//
//   vcvars64, then from the repo root:
//   cl /std:c++20 /EHsc /O2 tests\nr_depth_copy_dx11_gpu.cpp OptiScaler\native\DepthCopyDx11.cpp OptiScaler\native\SharedFrame.cpp d3d11.lib d3d12.lib d3dcompiler.lib dxgi.lib

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "../OptiScaler/native/DepthCopyDx11.h"

using Microsoft::WRL::ComPtr;

namespace
{
int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

struct Device
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11InfoQueue> info; // only with the debug layer installed
};

bool MakeDevice(Device* out)
{
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;

    if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_DEBUG, &level, 1,
                                    D3D11_SDK_VERSION, &out->device, nullptr, &out->context)))
    {
        // Errors, warnings and corruption only: the debug layer also stores plain information messages.
        if (SUCCEEDED(out->device.As(&out->info)))
        {
            D3D11_MESSAGE_SEVERITY deny[] = { D3D11_MESSAGE_SEVERITY_INFO, D3D11_MESSAGE_SEVERITY_MESSAGE };
            D3D11_INFO_QUEUE_FILTER filter {};
            filter.DenyList.NumSeverities = 2;
            filter.DenyList.pSeverityList = deny;
            out->info->PushStorageFilter(&filter);
        }

        return true;
    }

    return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                                       &out->device, nullptr, &out->context));
}

// A depth buffer of `resource` format, bound for depth writes through a `dsv` view and cleared to `value`. Left bound.
ComPtr<ID3D11Texture2D> BoundDepth(Device& d, DXGI_FORMAT resource, DXGI_FORMAT dsv, UINT width, UINT height, UINT mips,
                                   float value)
{
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = mips;
    desc.ArraySize = 1;
    desc.Format = resource;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    ComPtr<ID3D11Texture2D> tex;

    if (FAILED(d.device->CreateTexture2D(&desc, nullptr, &tex)))
        return nullptr;

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc {};
    dsvDesc.Format = dsv;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;

    ComPtr<ID3D11DepthStencilView> view;

    if (FAILED(d.device->CreateDepthStencilView(tex.Get(), &dsvDesc, &view)))
        return nullptr;

    d.context->OMSetRenderTargets(0, nullptr, view.Get());
    d.context->ClearDepthStencilView(view.Get(), D3D11_CLEAR_DEPTH, value, 0);
    return tex;
}

// The copy's values at three places (corner, centre, far corner), all expected to be `value`.
bool ReadsBack(Device& d, ID3D11Texture2D* copy, float value)
{
    D3D11_TEXTURE2D_DESC desc {};
    copy->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> readback;

    if (FAILED(d.device->CreateTexture2D(&desc, nullptr, &readback)))
        return false;

    d.context->CopyResource(readback.Get(), copy);

    D3D11_MAPPED_SUBRESOURCE mapped {};

    if (FAILED(d.context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;

    const auto at = [&](UINT x, UINT y) { return ((const float*) ((const uint8_t*) mapped.pData + y * mapped.RowPitch))[x]; };
    const float tolerance = 1.0f / 65535.0f * 2.0f; // D16 is the coarsest
    const bool ok = std::fabs(at(0, 0) - value) < tolerance && std::fabs(at(desc.Width / 2, desc.Height / 2) - value) < tolerance &&
                    std::fabs(at(desc.Width - 1, desc.Height - 1) - value) < tolerance;

    if (!ok)
        std::printf("      read %.6f %.6f %.6f, wanted %.6f\n", at(0, 0), at(desc.Width / 2, desc.Height / 2),
                    at(desc.Width - 1, desc.Height - 1), value);

    d.context->Unmap(readback.Get(), 0);
    return ok;
}

ComPtr<ID3D11Device> DeviceOf(ID3D11Resource* resource)
{
    ComPtr<ID3D11Device> device;
    resource->GetDevice(&device);
    return device;
}

// Errors and warnings the debug layer has stored since the last call (0 without the debug layer), printed.
UINT64 DebugMessages(Device& d)
{
    if (d.info == nullptr)
        return 0;

    const UINT64 count = d.info->GetNumStoredMessages();

    for (UINT64 i = 0; i < count; ++i)
    {
        SIZE_T size = 0;
        d.info->GetMessage(i, nullptr, &size);
        auto* message = (D3D11_MESSAGE*) std::malloc(size);

        if (message != nullptr && SUCCEEDED(d.info->GetMessage(i, message, &size)))
            std::printf("      debug layer: %.*s\n", (int) message->DescriptionByteLength, message->pDescription);

        std::free(message);
    }

    d.info->ClearStoredMessages();
    return count;
}

struct FormatCase
{
    const char* name;
    DXGI_FORMAT resource, dsv;
    UINT mips;
    bool converted;
};

void Formats(Device& d)
{
    const FormatCase cases[] = {
        { "D32_FLOAT, copied straight", DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT, 1, false },
        { "R32_TYPELESS, 4 mips, copied straight", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, 4, false },
        { "D24_UNORM_S8_UINT typed, converted", DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D24_UNORM_S8_UINT, 1, true },
        { "R24G8_TYPELESS, 3 mips, converted", DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, 3, true },
        { "D32_FLOAT_S8X24_UINT typed, converted", DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 1,
          true },
        { "R32G8X24_TYPELESS, 2 mips, converted", DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 2, true },
        { "D16_UNORM, converted", DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_D16_UNORM, 1, true },
    };

    native::DepthCopyDx11 copy;
    float value = 0.25f;

    for (const auto& c : cases)
    {
        auto depth = BoundDepth(d, c.resource, c.dsv, 320, 180, c.mips, value);

        if (depth == nullptr)
        {
            Check(false, c.name);
            continue;
        }

        const char* failed = copy.Take(d.context.Get(), depth.Get(), true);
        char what[160];
        std::snprintf(what, sizeof(what), "%s: taken, reads %.3f back while still bound", c.name, value);
        Check(failed == nullptr && copy.Taken() && copy.Converted() == c.converted && ReadsBack(d, copy.Copy(), value), what);

        if (failed != nullptr)
            std::printf("      not taken: %s\n", failed);

        value += 0.0625f; // each case its own value, so a copy left from the case before cannot pass
        d.context->OMSetRenderTargets(0, nullptr, nullptr);
    }
}

void GameState(Device& d)
{
    // The game's own compute shader, read view and write view at slot 0.
    const char* source = "RWTexture2D<float> U : register(u0); Texture2D<float> T : register(t0);\n"
                         "[numthreads(1, 1, 1)] void CSMain(uint3 id : SV_DispatchThreadID) { U[id.xy] = T[id.xy]; }";
    ComPtr<ID3DBlob> code;
    D3DCompile(source, std::strlen(source), "game", nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &code, nullptr);
    ComPtr<ID3D11ComputeShader> shader;
    d.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader);

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = 16;
    desc.Height = 16;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> readTex, writeTex;
    d.device->CreateTexture2D(&desc, nullptr, &readTex);
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    d.device->CreateTexture2D(&desc, nullptr, &writeTex);
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    d.device->CreateShaderResourceView(readTex.Get(), nullptr, &srv);
    d.device->CreateUnorderedAccessView(writeTex.Get(), nullptr, &uav);

    native::DepthCopyDx11 copy;

    for (const bool set : { true, false })
    {
        ID3D11ShaderResourceView* const srvs[] = { set ? srv.Get() : nullptr };
        ID3D11UnorderedAccessView* const uavs[] = { set ? uav.Get() : nullptr };
        d.context->CSSetShader(set ? shader.Get() : nullptr, nullptr, 0);
        d.context->CSSetShaderResources(0, 1, srvs);
        d.context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

        auto depth = BoundDepth(d, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D24_UNORM_S8_UINT, 64, 64, 1, 0.5f);
        const char* failed = copy.Take(d.context.Get(), depth.Get(), true);

        ComPtr<ID3D11ComputeShader> nowShader;
        ComPtr<ID3D11ShaderResourceView> nowSrv;
        ComPtr<ID3D11UnorderedAccessView> nowUav;
        d.context->CSGetShader(&nowShader, nullptr, nullptr);
        d.context->CSGetShaderResources(0, 1, &nowSrv);
        d.context->CSGetUnorderedAccessViews(0, 1, &nowUav);

        Check(failed == nullptr && copy.Converted() && ReadsBack(d, copy.Copy(), 0.5f),
              set ? "converted copy with the game's compute state set: taken" : "converted copy with nothing bound: taken");
        Check(nowShader.Get() == (set ? shader.Get() : nullptr) && nowSrv.Get() == (set ? srv.Get() : nullptr) &&
                  nowUav.Get() == (set ? uav.Get() : nullptr),
              set ? "the game's compute shader, read view 0 and write view 0 are put back"
                  : "an empty compute shader, read view 0 and write view 0 stay empty");

        d.context->OMSetRenderTargets(0, nullptr, nullptr);
    }

    d.context->CSSetShader(nullptr, nullptr, 0);
    ID3D11UnorderedAccessView* const none[] = { nullptr };
    d.context->CSSetUnorderedAccessViews(0, 1, none, nullptr);
}

// How many of the copy's pixels read `value`, and how many read neither `value` nor `other`.
void Count(Device& d, ID3D11Texture2D* copy, float value, float other, int* matching, int* neither)
{
    D3D11_TEXTURE2D_DESC desc {};
    copy->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    *matching = *neither = 0;

    ComPtr<ID3D11Texture2D> readback;

    if (FAILED(d.device->CreateTexture2D(&desc, nullptr, &readback)))
    {
        *neither = -1;
        return;
    }

    d.context->CopyResource(readback.Get(), copy);
    D3D11_MAPPED_SUBRESOURCE mapped {};

    if (FAILED(d.context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
    {
        *neither = -1;
        return;
    }

    const float tolerance = 1.0f / 16777215.0f * 4.0f; // D24 is the coarsest here

    for (UINT y = 0; y < desc.Height; ++y)
    {
        for (UINT x = 0; x < desc.Width; ++x)
        {
            const float v = ((const float*) ((const uint8_t*) mapped.pData + y * mapped.RowPitch))[x];

            if (std::fabs(v - value) < tolerance)
                ++*matching;
            else if (std::fabs(v - other) >= tolerance)
                ++*neither;
        }
    }

    d.context->Unmap(readback.Get(), 0);
}

// A multisampled buffer with an edge across it: cleared to 0.25, then a triangle at 0.75 over its upper-left half, with 4
// samples, so the pixels along the triangle's diagonal hold both depths. The copy keeps each pixel's nearest sample: with
// reversed Z (near is 1) the 0.75 of the triangle reaches out over the edge, with standard Z the 0.25 does.
void Multisampled(Device& d)
{
    const char* vsSource = "float4 VSMain(uint id : SV_VertexID) : SV_Position {\n"
                           "  float2 p = id == 0 ? float2(-1, 1) : id == 1 ? float2(1, 1) : float2(-1, -1);\n"
                           "  return float4(p, 0.75, 1); }";
    ComPtr<ID3DBlob> code;
    D3DCompile(vsSource, std::strlen(vsSource), "edge", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &code, nullptr);
    ComPtr<ID3D11VertexShader> vs;
    d.device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs);

    D3D11_DEPTH_STENCIL_DESC dsDesc {};
    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ComPtr<ID3D11DepthStencilState> always;
    d.device->CreateDepthStencilState(&dsDesc, &always);

    D3D11_RASTERIZER_DESC rsDesc {};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_NONE;
    rsDesc.DepthClipEnable = TRUE;
    rsDesc.MultisampleEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster;
    d.device->CreateRasterizerState(&rsDesc, &raster);

    struct Case
    {
        const char* name;
        DXGI_FORMAT resource, dsv;
    };

    const Case cases[] = {
        { "D32_FLOAT, 4 samples", DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT },
        { "R24G8_TYPELESS as D24S8, 4 samples", DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT },
    };

    for (const auto& c : cases)
    {
        UINT quality = 0;

        if (FAILED(d.device->CheckMultisampleQualityLevels(c.dsv, 4, &quality)) || quality == 0)
        {
            std::printf("      %s: 4 samples not supported here, skipped\n", c.name);
            continue;
        }

        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = 96;
        desc.Height = 64;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = c.resource;
        desc.SampleDesc.Count = 4;
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11DepthStencilView> view;
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc {};
        dsvDesc.Format = c.dsv;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;

        if (FAILED(d.device->CreateTexture2D(&desc, nullptr, &tex)) ||
            FAILED(d.device->CreateDepthStencilView(tex.Get(), &dsvDesc, &view)))
        {
            Check(false, c.name);
            continue;
        }

        D3D11_VIEWPORT viewport { 0, 0, (float) desc.Width, (float) desc.Height, 0, 1 };
        d.context->OMSetRenderTargets(0, nullptr, view.Get());
        d.context->ClearDepthStencilView(view.Get(), D3D11_CLEAR_DEPTH, 0.25f, 0);
        d.context->OMSetDepthStencilState(always.Get(), 0);
        d.context->RSSetState(raster.Get());
        d.context->RSSetViewports(1, &viewport);
        d.context->IASetInputLayout(nullptr);
        d.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d.context->VSSetShader(vs.Get(), nullptr, 0);
        d.context->PSSetShader(nullptr, nullptr, 0);
        d.context->Draw(3, 0);

        native::DepthCopyDx11 copy;
        int nearReversed = 0, strayReversed = 0, nearStandard = 0, strayStandard = 0;

        const char* failed = copy.Take(d.context.Get(), tex.Get(), true);
        const bool takenReversed = failed == nullptr && copy.Taken() && copy.Samples() == 4;

        if (takenReversed)
            Count(d, copy.Copy(), 0.75f, 0.25f, &nearReversed, &strayReversed);
        else if (failed != nullptr)
            std::printf("      not taken: %s\n", failed);

        failed = copy.Take(d.context.Get(), tex.Get(), false);
        const bool takenStandard = failed == nullptr && copy.Taken() && copy.Samples() == 4;

        if (takenStandard)
            Count(d, copy.Copy(), 0.75f, 0.25f, &nearStandard, &strayStandard);

        char what[200];
        std::snprintf(what, sizeof(what), "%s: taken and resolved, every pixel one of the two depths", c.name);
        Check(takenReversed && takenStandard && strayReversed == 0 && strayStandard == 0, what);
        // Half the picture is the triangle; the edge pixels go to 0.75 with reversed Z only.
        std::snprintf(what, sizeof(what), "%s: the edge keeps its nearest sample (0.75 on %d pixels reversed, %d standard)",
                      c.name, nearReversed, nearStandard);
        Check(nearStandard > 0 && nearReversed > nearStandard + (int) desc.Height / 2, what);

        d.context->OMSetRenderTargets(0, nullptr, nullptr);
    }

    d.context->VSSetShader(nullptr, nullptr, 0);
    d.context->OMSetDepthStencilState(nullptr, 0);
    d.context->RSSetState(nullptr);
}

void NoCopy(Device& d)
{
    native::DepthCopyDx11 copy;
    auto good = BoundDepth(d, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT, 64, 64, 1, 0.5f);
    Check(copy.Take(d.context.Get(), good.Get(), true) == nullptr && copy.Taken(), "a good copy first");

    // A colour format.
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = 64;
    desc.Height = 64;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    Check(copy.Take(d.context.Get(), good.Get(), true) == nullptr && copy.Taken(), "the good copy again");
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> colour;
    d.device->CreateTexture2D(&desc, nullptr, &colour);
    Check(copy.Take(d.context.Get(), colour.Get(), true) != nullptr && !copy.Taken(),
          "a colour buffer: not taken, the good copy before is not offered");

    // A deferred context.
    Check(copy.Take(d.context.Get(), good.Get(), true) == nullptr && copy.Taken(), "the good copy once more");
    ComPtr<ID3D11DeviceContext> deferred;

    if (SUCCEEDED(d.device->CreateDeferredContext(0, &deferred)))
        Check(copy.Take(deferred.Get(), good.Get(), true) != nullptr && !copy.Taken(),
              "a deferred context: not taken, the good copy before is not offered");

    // Forget().
    Check(copy.Take(d.context.Get(), good.Get(), true) == nullptr && copy.Taken(), "the good copy a last time");
    copy.Forget();
    Check(!copy.Taken(), "Forget(): the copy is not offered");

    d.context->OMSetRenderTargets(0, nullptr, nullptr);
}

void SizeAndDevice(Device& a, Device& b)
{
    native::DepthCopyDx11 copy;

    auto first = BoundDepth(a, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D24_UNORM_S8_UINT, 320, 180, 1, 0.25f);
    Check(copy.Take(a.context.Get(), first.Get(), true) == nullptr && copy.Width() == 320 && ReadsBack(a, copy.Copy(), 0.25f),
          "320x180 on the first device");

    auto bigger = BoundDepth(a, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D24_UNORM_S8_UINT, 640, 360, 1, 0.375f);
    Check(copy.Take(a.context.Get(), bigger.Get(), true) == nullptr && copy.Width() == 640 && copy.Height() == 360 &&
              ReadsBack(a, copy.Copy(), 0.375f),
          "a new size: the copy is made again at 640x360");

    auto other = BoundDepth(b, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D24_UNORM_S8_UINT, 640, 360, 1, 0.75f);
    Check(copy.Take(b.context.Get(), other.Get(), true) == nullptr && DeviceOf(copy.Copy()) == b.device &&
              ReadsBack(b, copy.Copy(), 0.75f),
          "a buffer of another device: the copy is made again on that device");

    auto straight = BoundDepth(b, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT, 640, 360, 1, 0.125f);
    Check(copy.Take(b.context.Get(), straight.Get(), true) == nullptr && !copy.Converted() && ReadsBack(b, copy.Copy(), 0.125f),
          "then a D32 buffer of the same size: copied straight into the same copy");

    copy.Release();
    Check(!copy.Taken() && copy.Copy() == nullptr, "Release(): nothing kept");

    a.context->OMSetRenderTargets(0, nullptr, nullptr);
    b.context->OMSetRenderTargets(0, nullptr, nullptr);
}
} // namespace

int main()
{
    Device a, b;

    if (!MakeDevice(&a) || !MakeDevice(&b))
    {
        std::printf("no D3D11 device\n");
        return 2;
    }

    std::printf("debug layer: %s\n", a.info != nullptr ? "on" : "not installed (its check is skipped)");

    Formats(a);
    GameState(a);
    Multisampled(a);
    // The debug layer warns about the deferred context and the colour buffer only if they are copied: they must not be.
    NoCopy(a);
    SizeAndDevice(a, b);

    Check(DebugMessages(a) == 0 && DebugMessages(b) == 0, "the debug layer reported nothing");

    std::printf("%s (%d failed)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
