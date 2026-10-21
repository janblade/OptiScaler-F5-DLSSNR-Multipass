// GPU test for output/SdrToHdr_Dx11.cpp and output/SdrToHdr_ProxyDx11.cpp, HDR output on Direct3D 11, no game:
//   - the facts the proxy format decision rests on, read from Direct3D 11 itself (a typed texture refuses the other of
//     UNORM / _SRGB for a view, a TYPELESS one refuses a null-description view, a flip chain shows buffer 0 only),
//   - the pass maths against a CPU reference, scRGB and HDR10, gamma 2.2 and sRGB, 203 and 80 nits, a peak clamp, Convert off,
//     R8G8B8A8 / B8G8R8A8 UNORM proxies and a typed R8G8B8A8_SRGB one (PassConstants::srcLinear),
//   - the game's pipeline state is the same after the pass as before it (the context is the game's),
//   - a real windowed flip-model chain: Start, frames, resize, buffer count change, a proxy held by the game (a reference, a
//     bound view) refused, unproxied and back, the failure revert (and the one that cannot revert), GetBuffer and a nested
//     resize refused while a resize runs, GameBuffer and the registry,
//   - the live objects are the same after everything is released, and the debug layer reports nothing.
//
// Run on the hardware adapter or on WARP (argument "warp").
//
//   vcvars64, then from the repo root:
//   cl /std:c++20 /EHsc /O2 tests\sdr_to_hdr_dx11_gpu.cpp OptiScaler\output\SdrToHdr_ProxyDx11.cpp OptiScaler\output\SdrToHdr_Dx11.cpp
//      OptiScaler\output\SdrToHdr_ProxyBase.cpp d3d11.lib dxgi.lib d3dcompiler.lib user32.lib
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

#include "../OptiScaler/output/SdrToHdr_ProxyDx11.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace
{
int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

constexpr UINT kWidth = 16, kHeight = 2;
constexpr DXGI_FORMAT kGame = DXGI_FORMAT_R8G8B8A8_UNORM;

struct Gpu
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11InfoQueue> info;
    ComPtr<ID3D11Debug> debug;
    ComPtr<IDXGIFactory2> factory;
    bool debugLayer = false;
};

bool MakeGpu(Gpu& g, bool warp)
{
    const D3D_DRIVER_TYPE type = warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG;
    HRESULT hr = D3D11CreateDevice(nullptr, type, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &g.device, nullptr,
                                   &g.context);

    if (FAILED(hr))
    {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, type, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &g.device, nullptr,
                               &g.context);
    }

    if (FAILED(hr))
        return false;

    g.debugLayer = (flags & D3D11_CREATE_DEVICE_DEBUG) != 0;
    g.device.As(&g.info);
    g.device.As(&g.debug);

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    g.device.As(&dxgiDevice);
    dxgiDevice->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&g.factory));
    return g.factory != nullptr;
}

int DebugMessages(Gpu& g)
{
    if (g.info == nullptr)
        return 0;

    int count = 0;

    for (UINT64 i = 0; i < g.info->GetNumStoredMessages(); ++i)
    {
        SIZE_T size = 0;
        g.info->GetMessage(i, nullptr, &size);
        auto* message = (D3D11_MESSAGE*) malloc(size);

        if (message != nullptr && SUCCEEDED(g.info->GetMessage(i, message, &size)) &&
            (message->Severity == D3D11_MESSAGE_SEVERITY_ERROR || message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION ||
             message->Severity == D3D11_MESSAGE_SEVERITY_WARNING))
        {
            std::printf("      debug layer: %s\n", message->pDescription);
            ++count;
        }

        free(message);
    }

    g.info->ClearStoredMessages();
    return count;
}

// How many of each live object the device reports (ReportLiveDeviceObjects writes INFO messages: "Live Texture2D: 3")
std::vector<std::pair<std::string, int>> LiveObjects(Gpu& g)
{
    std::vector<std::pair<std::string, int>> live;

    if (g.debug == nullptr || g.info == nullptr)
        return live;

    g.info->ClearStoredMessages();
    g.debug->ReportLiveDeviceObjects(D3D11_RLDO_SUMMARY);

    for (UINT64 i = 0; i < g.info->GetNumStoredMessages(); ++i)
    {
        SIZE_T size = 0;
        g.info->GetMessage(i, nullptr, &size);
        auto* message = (D3D11_MESSAGE*) malloc(size);

        if (message != nullptr && SUCCEEDED(g.info->GetMessage(i, message, &size)))
        {
            const std::string text = message->pDescription;
            const auto colon = text.find(": ");

            if (text.rfind("Live ", 0) == 0 && colon != std::string::npos)
                live.emplace_back(text.substr(5, colon - 5), std::atoi(text.c_str() + colon + 2));
        }

        free(message);
    }

    g.info->ClearStoredMessages();
    return live;
}

struct Rgb
{
    uint8_t r, g, b;
};

// Known values: grey steps (0, 0.25, 0.5, 0.735, 1) and colours
std::vector<Rgb> Picture()
{
    const Rgb list[] = { { 0, 0, 0 },     { 64, 64, 64 },  { 128, 128, 128 }, { 187, 187, 187 }, { 255, 255, 255 },
                         { 255, 0, 0 },   { 0, 255, 0 },   { 0, 0, 255 },     { 204, 102, 26 },  { 51, 153, 230 },
                         { 10, 200, 90 }, { 255, 255, 0 }, { 128, 0, 255 },   { 250, 250, 250 }, { 5, 5, 5 },
                         { 32, 32, 32 } };
    std::vector<Rgb> picture;

    for (UINT y = 0; y < kHeight; ++y)
        for (UINT x = 0; x < kWidth; ++x)
            picture.push_back(list[(x + y * 7) % (sizeof(list) / sizeof(list[0]))]);

    return picture;
}

ComPtr<ID3D11Texture2D> Texture(Gpu& g, DXGI_FORMAT format, UINT width, UINT height, UINT bind,
                                D3D11_USAGE usage = D3D11_USAGE_DEFAULT, UINT cpu = 0)
{
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = usage;
    desc.BindFlags = bind;
    desc.CPUAccessFlags = cpu;
    ComPtr<ID3D11Texture2D> texture;
    g.device->CreateTexture2D(&desc, nullptr, &texture);
    return texture;
}

// Writes the picture into `proxy` as `format` stores it: B8G8R8A8 swaps red and blue in memory
void Fill(Gpu& g, ID3D11Texture2D* proxy, bool bgra, const std::vector<Rgb>& picture)
{
    std::vector<uint8_t> bytes(kWidth * kHeight * 4);

    for (size_t i = 0; i < picture.size(); ++i)
    {
        bytes[i * 4 + 0] = bgra ? picture[i].b : picture[i].r;
        bytes[i * 4 + 1] = picture[i].g;
        bytes[i * 4 + 2] = bgra ? picture[i].r : picture[i].b;
        bytes[i * 4 + 3] = 255;
    }

    g.context->UpdateSubresource(proxy, 0, nullptr, bytes.data(), kWidth * 4, 0);
}

// The first subresource as bytes, row pitch removed
std::vector<uint8_t> Read(Gpu& g, ID3D11Texture2D* texture, UINT bytesPerPixel)
{
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    auto staging = Texture(g, desc.Format, desc.Width, desc.Height, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
    g.context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    std::vector<uint8_t> bytes;

    if (FAILED(g.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return bytes;

    for (UINT y = 0; y < desc.Height; ++y)
    {
        const uint8_t* line = (const uint8_t*) mapped.pData + (size_t) y * mapped.RowPitch;
        bytes.insert(bytes.end(), line, line + (size_t) desc.Width * bytesPerPixel);
    }

    g.context->Unmap(staging.Get(), 0);
    return bytes;
}

float HalfToFloat(uint16_t h)
{
    const int sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
    float value = exponent == 0 ? std::ldexp((float) mantissa, -24) : std::ldexp((float) (mantissa + 1024), exponent - 25);
    return sign != 0 ? -value : value;
}

// The size of one step of a half at `value`: the best an FP16 buffer can do
float HalfUlp(float value)
{
    int exponent = 0;
    std::frexp(std::fabs(value) < 6.1e-5f ? 6.1e-5f : std::fabs(value), &exponent);
    return std::ldexp(1.0f, exponent - 11);
}

// ---- the reference (independent of the shader's code) ----
double SrgbDecode(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }

double PqEncode(double nits)
{
    const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    const double p = std::pow(std::clamp(nits / 10000.0, 0.0, 1.0), m1);
    return std::pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

void Reference(const SdrToHdr::PassConstants& k, const Rgb& p, double out[3])
{
    const double c[3] = { p.r / 255.0, p.g / 255.0, p.b / 255.0 };
    double nits[3];

    for (int i = 0; i < 3; ++i)
    {
        double lin;

        if (k.convert != 0)
            lin = k.decode == 0 ? std::pow(c[i], 2.2) : SrgbDecode(c[i]);
        else
            lin = SrgbDecode(c[i]);

        nits[i] = std::min(lin * (k.convert != 0 ? k.gameNits : k.sdrWhiteNits), (double) k.peakNits);
    }

    if (k.mode == 0)
    {
        for (int i = 0; i < 3; ++i)
            out[i] = nits[i] / 80.0;

        return;
    }

    out[0] = PqEncode(0.6274039 * nits[0] + 0.3292830 * nits[1] + 0.0433131 * nits[2]);
    out[1] = PqEncode(0.0690973 * nits[0] + 0.9195404 * nits[1] + 0.0113623 * nits[2]);
    out[2] = PqEncode(0.0163914 * nits[0] + 0.0880133 * nits[1] + 0.8955953 * nits[2]);
}

// `relative`: an extra allowance as a fraction of the value, for a typed _SRGB proxy on real hardware (below)
bool Compare(const std::vector<uint8_t>& out, const SdrToHdr::PassConstants& k, const std::vector<Rgb>& picture,
             double& worst, double relative = 0.0)
{
    worst = 0.0;
    bool ok = out.size() == (size_t) kWidth * kHeight * (k.mode == 0 ? 8 : 4);

    for (size_t i = 0; ok && i < picture.size(); ++i)
    {
        double expected[3];
        Reference(k, picture[i], expected);

        for (int channel = 0; channel < 3; ++channel)
        {
            if (k.mode == 0)
            {
                uint16_t half;
                std::memcpy(&half, &out[i * 8 + channel * 2], 2);
                const double error = std::fabs(HalfToFloat(half) - expected[channel]);
                // FP16 near 2.5 cannot be closer than a step of 2e-3: allow one step where that exceeds 1e-3
                const double allowed = std::max({ 1e-3, (double) HalfUlp((float) expected[channel]), relative * expected[channel] });
                worst = std::max(worst, error);
                ok = ok && error <= allowed;
            }
            else
            {
                uint32_t texel;
                std::memcpy(&texel, &out[i * 4], 4);
                const double code = (double) ((texel >> (channel * 10)) & 1023);
                const double error = std::fabs(code - expected[channel] * 1023.0);
                worst = std::max(worst, error);
                ok = ok && error <= 1.0;
            }
        }

        if (k.mode == 0)
        {
            uint16_t half;
            std::memcpy(&half, &out[i * 8 + 6], 2);
            ok = ok && HalfToFloat(half) == 1.0f;
        }
        else
        {
            uint32_t texel;
            std::memcpy(&texel, &out[i * 4], 4);
            ok = ok && (texel >> 30) == 3u;
        }
    }

    return ok;
}

struct Case
{
    const char* name;
    SdrToHdr::PassConstants k;
};

Case Cases(int mode, int which)
{
    SdrToHdr::PassConstants k;
    k.mode = (uint32_t) mode;
    k.peakNits = 1000.0f;

    switch (which)
    {
    case 0:
        return { "gamma 2.2, 203 nits", k };
    case 1:
        k.decode = 1;
        return { "sRGB curve, 203 nits", k };
    case 2:
        k.gameNits = 80.0f;
        return { "gamma 2.2, 80 nits", k };
    case 3:
        k.gameNits = 400.0f;
        k.peakNits = 300.0f;
        return { "gamma 2.2, 400 nit white clamped to a 300 nit peak", k };
    case 4:
        k.convert = 0;
        k.sdrWhiteNits = 80.0f;
        return { "Convert off, SDR white 80 nits", k };
    default:
        k.convert = 0;
        k.sdrWhiteNits = 240.0f;
        return { "Convert off, SDR white 240 nits", k };
    }
}

// ---- the facts the proxy format rests on ----
void Facts(Gpu& g)
{
    auto tryViews = [&](DXGI_FORMAT textureFormat, DXGI_FORMAT viewFormat, bool nullDesc)
    {
        auto texture = Texture(g, textureFormat, 16, 16, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        ComPtr<ID3D11RenderTargetView> rtv;
        D3D11_RENDER_TARGET_VIEW_DESC desc {};
        desc.Format = viewFormat;
        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        return SUCCEEDED(g.device->CreateRenderTargetView(texture.Get(), nullDesc ? nullptr : &desc, &rtv));
    };

    Check(tryViews(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, false) &&
              tryViews(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, true),
          "fact: a typed UNORM texture takes a UNORM view and a null-description view");
    Check(!tryViews(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false),
          "fact: a typed UNORM texture REFUSES an _SRGB view (Direct3D 11 has no relaxed casting)");
    Check(!tryViews(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM, false),
          "fact: a typed _SRGB texture refuses a UNORM view");
    Check(tryViews(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, false) &&
              tryViews(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false),
          "fact: a TYPELESS texture takes both a UNORM and an _SRGB view");
    Check(!tryViews(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_UNKNOWN, true),
          "fact: a TYPELESS texture REFUSES a null-description view (and GetDesc would show TYPELESS), which is why the "
          "proxy is typed");
}

// One proxy format, both outputs, every case: the pass on its own textures
void Maths(Gpu& g, SdrToHdr::Dx11Pass& pass, DXGI_FORMAT proxyFormat, const char* label, bool warp)
{
    const bool bgra = proxyFormat == DXGI_FORMAT_B8G8R8A8_UNORM || proxyFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const bool srgbTyped = proxyFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || proxyFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const auto picture = Picture();
    Check(pass.Init(g.device.Get()) == nullptr, "Init");

    for (int mode = 0; mode < 2; ++mode)
    {
        const DXGI_FORMAT realFormat = mode == 0 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R10G10B10A2_UNORM;
        auto proxy = Texture(g, proxyFormat, kWidth, kHeight, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        auto real = Texture(g, realFormat, kWidth, kHeight, D3D11_BIND_RENDER_TARGET);
        Fill(g, proxy.Get(), bgra, picture);

        for (int which = 0; which < 6; ++which)
        {
            Case c = Cases(mode, which);
            c.k.srcLinear = srgbTyped ? 1u : 0u;
            // twice in a row: the second reuses the kept read view
            const char* first = pass.Run(g.context.Get(), proxy.Get(), real.Get(), c.k);
            const char* second = pass.Run(g.context.Get(), proxy.Get(), real.Get(), c.k);
            const auto out = Read(g, real.Get(), mode == 0 ? 8 : 4);
            double worst = 0.0;
            // A typed _SRGB proxy is read through the hardware's sRGB decode and encoded again by the shader: exact on WARP,
            // within half a percent on a real adapter (whose decode is not exact). No flip chain can have such a back buffer
            // (DXGI refuses an _SRGB flip format), so the case only guards the shader flag.
            const double relative = srgbTyped && !warp ? 0.005 : 0.0;
            const bool ok = first == nullptr && second == nullptr && Compare(out, c.k, picture, worst, relative);
            char what[256];
            std::snprintf(what, sizeof(what), "%s -> %s, %s: max error %.5f%s", label, mode == 0 ? "scRGB FP16" : "PQ 10 bit",
                          c.name, worst, mode == 0 ? "" : " codes");
            Check(ok, what);
        }

        pass.ReleaseViews();
    }
}

// The live Convert toggle: the next presented frame has its own values
void ConvertToggle(Gpu& g, SdrToHdr::Dx11Pass& pass)
{
    const auto picture = Picture();
    Check(pass.Init(g.device.Get()) == nullptr, "Init");
    auto proxy = Texture(g, kGame, kWidth, kHeight, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    auto real = Texture(g, DXGI_FORMAT_R16G16B16A16_FLOAT, kWidth, kHeight, D3D11_BIND_RENDER_TARGET);
    Fill(g, proxy.Get(), false, picture);
    std::vector<uint8_t> frames[3];
    SdrToHdr::PassConstants k = Cases(0, 0).k;
    k.sdrWhiteNits = 120.0f;
    const uint32_t converts[3] = { 1, 0, 1 };

    for (int i = 0; i < 3; ++i)
    {
        k.convert = converts[i];
        const char* ran = pass.Run(g.context.Get(), proxy.Get(), real.Get(), k);
        frames[i] = Read(g, real.Get(), 8);
        double worst = 0.0;
        char what[160];
        std::snprintf(what, sizeof(what), "Convert toggle, frame %d (Convert %u) matches its reference", i, converts[i]);
        Check(ran == nullptr && Compare(frames[i], k, picture, worst), what);
    }

    Check(frames[0] != frames[1] && frames[0] == frames[2], "Convert toggle: the next frame changes (on, off, on gives A, B, A)");
}

// ---- the game's pipeline state, before and after the pass ----
void StateKept(Gpu& g, SdrToHdr::Dx11Pass& pass)
{
    ID3D11Device* device = g.device.Get();
    ID3D11DeviceContext* ctx = g.context.Get();

    const char* shaderText = "float4 VSMain(uint id : SV_VertexID) : SV_Position { return float4(0,0,0,1); }\n"
                             "float4 PSMain() : SV_Target { return float4(1,0,0,1); }\n";
    ComPtr<ID3DBlob> vsBlob, psBlob;
    D3DCompile(shaderText, std::strlen(shaderText), "game", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, nullptr);
    D3DCompile(shaderText, std::strlen(shaderText), "game", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, nullptr);
    ComPtr<ID3D11VertexShader> gameVs;
    ComPtr<ID3D11PixelShader> gamePs;
    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &gameVs);
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &gamePs);

    auto gameTarget = Texture(g, kGame, 32, 32, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    auto gameTarget2 = Texture(g, kGame, 32, 32, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    ComPtr<ID3D11RenderTargetView> gameRtv, gameRtv2;
    ComPtr<ID3D11ShaderResourceView> gameSrv;
    device->CreateRenderTargetView(gameTarget.Get(), nullptr, &gameRtv);
    device->CreateRenderTargetView(gameTarget2.Get(), nullptr, &gameRtv2);
    device->CreateShaderResourceView(gameTarget2.Get(), nullptr, &gameSrv);

    D3D11_TEXTURE2D_DESC depthDesc {};
    depthDesc.Width = depthDesc.Height = 32;
    depthDesc.MipLevels = depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11DepthStencilView> gameDsv;
    device->CreateTexture2D(&depthDesc, nullptr, &depth);
    device->CreateDepthStencilView(depth.Get(), nullptr, &gameDsv);

    D3D11_BUFFER_DESC bufferDesc {};
    bufferDesc.ByteWidth = 64;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> gameCb;
    device->CreateBuffer(&bufferDesc, nullptr, &gameCb);

    D3D11_RASTERIZER_DESC rasterizerDesc {};
    rasterizerDesc.FillMode = D3D11_FILL_WIREFRAME;
    rasterizerDesc.CullMode = D3D11_CULL_FRONT;
    rasterizerDesc.ScissorEnable = TRUE;
    rasterizerDesc.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> gameRasterizer;
    device->CreateRasterizerState(&rasterizerDesc, &gameRasterizer);

    D3D11_BLEND_DESC blendDesc {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> gameBlend;
    device->CreateBlendState(&blendDesc, &gameBlend);

    D3D11_DEPTH_STENCIL_DESC depthStateDesc {};
    depthStateDesc.DepthEnable = TRUE;
    depthStateDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthStateDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    ComPtr<ID3D11DepthStencilState> gameDepthState;
    device->CreateDepthStencilState(&depthStateDesc, &gameDepthState);

    // The game's state
    ID3D11RenderTargetView* rtvs[2] = { gameRtv.Get(), gameRtv2.Get() };
    ctx->OMSetRenderTargets(2, rtvs, gameDsv.Get());
    ctx->VSSetShader(gameVs.Get(), nullptr, 0);
    ctx->PSSetShader(gamePs.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = gameSrv.Get();
    // (slot 0 is read while target 1 is written -- unbind that target for the read)
    ID3D11RenderTargetView* one[1] = { gameRtv.Get() };
    ctx->OMSetRenderTargets(1, one, gameDsv.Get());
    ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11Buffer* cb = gameCb.Get();
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->RSSetState(gameRasterizer.Get());
    const FLOAT blendFactor[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
    ctx->OMSetBlendState(gameBlend.Get(), blendFactor, 0x0000ffff);
    ctx->OMSetDepthStencilState(gameDepthState.Get(), 7);
    D3D11_VIEWPORT viewports[2] = { { 1, 2, 30, 28, 0.1f, 0.9f }, { 3, 4, 20, 10, 0.0f, 1.0f } };
    ctx->RSSetViewports(2, viewports);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);

    const FLOAT cleared[4] = { 0.2f, 0.4f, 0.6f, 1.0f };
    ctx->ClearRenderTargetView(gameRtv.Get(), cleared);

    // The pass on its own textures
    Check(pass.Init(device) == nullptr, "Init");
    auto proxy = Texture(g, kGame, kWidth, kHeight, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    auto real = Texture(g, DXGI_FORMAT_R16G16B16A16_FLOAT, kWidth, kHeight, D3D11_BIND_RENDER_TARGET);
    Fill(g, proxy.Get(), false, Picture());
    const char* ran = pass.Run(ctx, proxy.Get(), real.Get(), Cases(0, 0).k);
    Check(ran == nullptr, "the pass ran with a busy game state");

    // The game's state, read back
    ComPtr<ID3D11RenderTargetView> gotRtvs[2];
    ComPtr<ID3D11DepthStencilView> gotDsv;
    ID3D11RenderTargetView* rawRtvs[2] = {};
    ctx->OMGetRenderTargets(2, rawRtvs, &gotDsv);
    gotRtvs[0].Attach(rawRtvs[0]);
    gotRtvs[1].Attach(rawRtvs[1]);
    ComPtr<ID3D11VertexShader> gotVs;
    ComPtr<ID3D11PixelShader> gotPs;
    ctx->VSGetShader(&gotVs, nullptr, nullptr);
    ctx->PSGetShader(&gotPs, nullptr, nullptr);
    ComPtr<ID3D11ShaderResourceView> gotSrv;
    ctx->PSGetShaderResources(0, 1, &gotSrv);
    ComPtr<ID3D11Buffer> gotCb;
    ctx->PSGetConstantBuffers(0, 1, &gotCb);
    ComPtr<ID3D11RasterizerState> gotRasterizer;
    ctx->RSGetState(&gotRasterizer);
    ComPtr<ID3D11BlendState> gotBlend;
    FLOAT gotFactor[4] = {};
    UINT gotMask = 0;
    ctx->OMGetBlendState(&gotBlend, gotFactor, &gotMask);
    ComPtr<ID3D11DepthStencilState> gotDepthState;
    UINT gotRef = 0;
    ctx->OMGetDepthStencilState(&gotDepthState, &gotRef);
    UINT gotViewportCount = 0;
    ctx->RSGetViewports(&gotViewportCount, nullptr);
    D3D11_VIEWPORT gotViewports[2] = {};
    gotViewportCount = std::min(gotViewportCount, 2u);
    ctx->RSGetViewports(&gotViewportCount, gotViewports);
    D3D11_PRIMITIVE_TOPOLOGY gotTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ctx->IAGetPrimitiveTopology(&gotTopology);
    ComPtr<ID3D11InputLayout> gotLayout;
    ctx->IAGetInputLayout(&gotLayout);

    Check(gotRtvs[0].Get() == gameRtv.Get() && gotDsv.Get() == gameDsv.Get(), "state: render target 0 and the depth view are the game's");
    Check(gotVs.Get() == gameVs.Get() && gotPs.Get() == gamePs.Get(), "state: vertex and pixel shaders are the game's");
    Check(gotSrv.Get() == gameSrv.Get(), "state: pixel shader read view 0 is the game's");
    Check(gotCb.Get() == gameCb.Get(), "state: pixel shader constant buffer 0 is the game's");
    Check(gotRasterizer.Get() == gameRasterizer.Get(), "state: the rasterizer state is the game's");
    Check(gotBlend.Get() == gameBlend.Get() && gotMask == 0x0000ffff && gotFactor[0] == 0.25f && gotFactor[3] == 1.0f,
          "state: the blend state, factor and sample mask are the game's");
    Check(gotDepthState.Get() == gameDepthState.Get() && gotRef == 7, "state: the depth-stencil state and its reference are the game's");
    Check(gotViewportCount == 2 && std::memcmp(&gotViewports[0], &viewports[0], sizeof(D3D11_VIEWPORT)) == 0 &&
              std::memcmp(&gotViewports[1], &viewports[1], sizeof(D3D11_VIEWPORT)) == 0,
          "state: both viewports are the game's");
    Check(gotTopology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST && gotLayout == nullptr, "state: topology and input layout are the game's");

    // and the game's render target was not drawn on
    const auto bytes = Read(g, gameTarget.Get(), 4);
    Check(bytes.size() == 32 * 32 * 4 && std::abs((int) bytes[0] - 51) <= 1 && std::abs((int) bytes[1] - 102) <= 1 &&
              std::abs((int) bytes[2] - 153) <= 1,
          "state: the game's own render target still holds what the game cleared it to");

    ctx->ClearState();
    pass.ReleaseViews();
}

// ---- a real windowed flip chain ----
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l) { return DefWindowProcW(window, message, w, l); }

HWND MakeWindow()
{
    WNDCLASSW wc {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SdrToHdrDx11Test";
    RegisterClassW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"sdr_to_hdr dx11 test", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr, nullptr,
                           wc.hInstance, nullptr);
}

ComPtr<IDXGISwapChain3> MakeChain(Gpu& g, HWND window, DXGI_FORMAT format, UINT count, UINT width, UINT height)
{
    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = count;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain1;
    ComPtr<IDXGISwapChain3> chain;

    if (SUCCEEDED(g.factory->CreateSwapChainForHwnd(g.device.Get(), window, &desc, nullptr, nullptr, &chain1)))
        chain1.As(&chain);

    return chain;
}

ComPtr<IDXGISwapChain1> MakeChainEffect(Gpu& g, HWND window, DXGI_SWAP_EFFECT effect)
{
    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Width = 64;
    desc.Height = 32;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = effect;
    ComPtr<IDXGISwapChain1> chain;
    g.factory->CreateSwapChainForHwnd(g.device.Get(), window, &desc, nullptr, nullptr, &chain);
    return chain;
}

// Draws a flat grey into `texture` through a null-description render target view: what a game does with a UNORM chain
void Paint(Gpu& g, ID3D11Texture2D* texture, float value)
{
    ComPtr<ID3D11RenderTargetView> rtv;

    if (FAILED(g.device->CreateRenderTargetView(texture, nullptr, &rtv)))
        return;

    const float color[4] = { value, value, value, 1.0f };
    g.context->ClearRenderTargetView(rtv.Get(), color);
}

// The red channel of the first pixel of a real FP16 back buffer
float ReadFirstPixel(Gpu& g, ID3D11Texture2D* texture)
{
    const auto bytes = Read(g, texture, 8);
    uint16_t half = 0;

    if (bytes.size() >= 2)
        std::memcpy(&half, bytes.data(), 2);

    return HalfToFloat(half);
}

uint32_t ReadFirstPixel10(Gpu& g, ID3D11Texture2D* texture)
{
    const auto bytes = Read(g, texture, 4);
    uint32_t texel = 0;

    if (bytes.size() >= 4)
        std::memcpy(&texel, bytes.data(), 4);

    return texel & 1023u;
}

float Expected(float grey) { return std::pow(std::round(grey * 255.0f) / 255.0f, 2.2f) * 203.0f / 80.0f; }

SdrToHdr::PassConstants Constants()
{
    SdrToHdr::PassConstants k;
    k.gameNits = 203.0f;
    k.peakNits = 1000.0f;
    return k;
}

bool Near(float a, float b) { return std::fabs(a - b) <= 2.5e-3f; }

// Paints the proxy, runs the pass, reads the real buffer, presents: true when the real buffer holds the conversion
bool Frame(Gpu& g, SdrToHdr::ProxyChainBase& chain, IDXGISwapChain3* real, float grey)
{
    grey = std::round(grey * 255.0f) / 255.0f; // exactly a byte, so the clear and the reference agree
    ComPtr<ID3D11Texture2D> proxy, realBuffer;

    if (FAILED(chain.GetBuffer(0, IID_PPV_ARGS(&proxy))) || FAILED(real->GetBuffer(0, IID_PPV_ARGS(&realBuffer))))
        return false;

    Paint(g, proxy.Get(), grey);
    const char* why = chain.Present(g.context.Get(), real, Constants());
    const float got = ReadFirstPixel(g, realBuffer.Get());
    proxy.Reset();
    realBuffer.Reset();
    real->Present(0, 0);
    return why == nullptr && Near(got, Expected(grey));
}

void Chains(Gpu& g, bool warp)
{
    (void) warp;
    HWND window = MakeWindow();

    {
        // ---- the off path: a chain that is not proxied ----
        {
            auto plain = MakeChain(g, window, kGame, 3, 64, 32);
            Check(plain != nullptr, "an R8G8B8A8 flip chain is made");

            if (plain == nullptr)
                return;

            SdrToHdr::ProxyChainDx11 none;
            ComPtr<ID3D11Texture2D> own, viaGame, second;
            plain->GetBuffer(0, IID_PPV_ARGS(&own));
            const HRESULT hr = SdrToHdr::GameBuffer(plain.Get(), 0, IID_PPV_ARGS(&viaGame));
            Check(!none.Active() && !SdrToHdr::AnyProxied(), "off path: no proxy chain is active and none is registered");
            Check(SUCCEEDED(hr) && viaGame.Get() == own.Get(), "off path: GameBuffer hands out the swap chain's own buffer");
            Check(none.GetBuffer(0, IID_PPV_ARGS(&viaGame)) == DXGI_ERROR_INVALID_CALL, "an inactive chain hands out nothing");
            Check(none.Present(g.context.Get(), plain.Get(), Constants()) != nullptr, "an inactive chain does not present");
            Check(FAILED(plain->GetBuffer(1, IID_PPV_ARGS(&second))),
                  "fact: a Direct3D 11 flip chain shows buffer 0 only, GetBuffer(1) fails");
        }

        // ---- a proxied chain: the real one is FP16 ----
        auto real = MakeChain(g, window, DXGI_FORMAT_R16G16B16A16_FLOAT, 3, 64, 32);
        Check(real != nullptr, "an FP16 flip chain is made");

        if (real == nullptr)
            return;

        SdrToHdr::ProxyChainDx11 chain;
        const char* started = chain.Start(g.device.Get(), real.Get(), kGame, false);
        Check(started == nullptr, "Start: colour space set, proxy and pass made");

        if (started != nullptr)
        {
            std::printf("      %s\n", started);
            return;
        }

        Check(chain.Active() && SdrToHdr::AnyProxied(), "the chain is active and registered");
        Check(chain.GameFormat() == kGame, "the chain remembers the game's format");

        {
            ComPtr<ID3D11Texture2D> proxy, viaGame, realBuffer, none;
            chain.GetBuffer(0, IID_PPV_ARGS(&proxy));
            SdrToHdr::GameBuffer(real.Get(), 0, IID_PPV_ARGS(&viaGame));
            real->GetBuffer(0, IID_PPV_ARGS(&realBuffer));
            D3D11_TEXTURE2D_DESC d {}, r {};
            proxy->GetDesc(&d);
            realBuffer->GetDesc(&r);
            Check(d.Format == kGame && d.Width == 64 && d.Height == 32 && (d.BindFlags & D3D11_BIND_RENDER_TARGET) != 0 &&
                      (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0,
                  "proxy: the game's format and size, render target and shader resource binds");
            Check(viaGame.Get() == proxy.Get() && realBuffer.Get() != proxy.Get() && r.Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
                  "GameBuffer gives the proxy, the chain's own buffer stays FP16");
            Check(chain.GetBuffer(1, IID_PPV_ARGS(&none)) == DXGI_ERROR_INVALID_CALL,
                  "GetBuffer(1) is INVALID_CALL, as the real flip chain says");

            // what a game does with a UNORM chain: a null-description view and a UNORM view both work on the proxy
            ComPtr<ID3D11RenderTargetView> nullView, unormView, srgbView;
            D3D11_RENDER_TARGET_VIEW_DESC viewDesc {};
            viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            viewDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            Check(SUCCEEDED(g.device->CreateRenderTargetView(proxy.Get(), nullptr, &nullView)) &&
                      SUCCEEDED(g.device->CreateRenderTargetView(proxy.Get(), &viewDesc, &unormView)),
                  "a game's null-description and UNORM views work on the proxy");
            viewDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            const HRESULT srgb = g.device->CreateRenderTargetView(proxy.Get(), &viewDesc, &srgbView);
            std::printf("      known difference: an _SRGB view of the proxy -> %08lX (the real flip chain's buffer allows it)\n",
                        (unsigned long) srgb);
        }

        bool all = true;

        for (int i = 0; i < 6; ++i)
            all = Frame(g, chain, real.Get(), 0.25f + 0.1f * (float) i) && all;

        Check(all, "six frames over three buffers: the real buffer holds the converted grey every time");

        SdrToHdr::ResizeRequest request;
        request.rawFormat = request.resolvedFormat = kGame;
        request.wantProxy = true;
        request.gameColourSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

        // new size
        request.width = 96;
        request.height = 48;
        auto result = chain.Resize(g.device.Get(), real.Get(), request);
        Check(SUCCEEDED(result.hr) && result.proxied && !result.refused, "Resize to 96x48 stays proxied");
        {
            ComPtr<ID3D11Texture2D> proxy;
            D3D11_TEXTURE2D_DESC d {};
            chain.GetBuffer(0, IID_PPV_ARGS(&proxy));
            proxy->GetDesc(&d);
            Check(d.Width == 96 && d.Height == 48, "the proxy has the new size");
        }
        Check(Frame(g, chain, real.Get(), 0.5f), "a frame after the resize converts");

        // buffer count (the proxy stays one)
        request.count = 4;
        result = chain.Resize(g.device.Get(), real.Get(), request);
        DXGI_SWAP_CHAIN_DESC realDesc {};
        real->GetDesc(&realDesc);
        Check(SUCCEEDED(result.hr) && result.proxied && realDesc.BufferCount == 4, "Resize to four buffers stays proxied");
        bool four = true;

        for (int i = 0; i < 5; ++i)
            four = Frame(g, chain, real.Get(), 0.3f + 0.1f * (float) i) && four;

        Check(four, "five frames over four buffers convert");

        // the game still holds the proxy: a reference
        ComPtr<ID3D11Texture2D> held;
        chain.GetBuffer(0, IID_PPV_ARGS(&held));
        request.count = 0;
        request.width = 80;
        request.height = 40;
        result = chain.Resize(g.device.Get(), real.Get(), request);
        real->GetDesc(&realDesc);
        Check(result.hr == DXGI_ERROR_INVALID_CALL && result.refused && chain.Active() && realDesc.BufferDesc.Width == 96,
              "Resize while a proxy reference is held: INVALID_CALL, the real chain was not resized");
        Check(Frame(g, chain, real.Get(), 0.5f), "and the chain still converts");
        held.Reset();

        // the game left a view of the proxy bound. Direct3D 11 does not count views in a texture's reference count, and a view of
        // the proxy is no view of the swap chain's buffer, so (unlike DXGI for its own buffers) this does not stop a resize: the
        // game keeps a stale proxy alive through its view, and gets the new one from its next GetBuffer
        {
            ComPtr<ID3D11Texture2D> proxy, fresh;
            ComPtr<ID3D11RenderTargetView> rtv;
            chain.GetBuffer(0, IID_PPV_ARGS(&proxy));
            g.device->CreateRenderTargetView(proxy.Get(), nullptr, &rtv);
            ID3D11RenderTargetView* bound = rtv.Get();
            g.context->OMSetRenderTargets(1, &bound, nullptr);
            ID3D11Texture2D* old = proxy.Get();
            proxy.Reset();
            result = chain.Resize(g.device.Get(), real.Get(), request);
            chain.GetBuffer(0, IID_PPV_ARGS(&fresh));
            Check(SUCCEEDED(result.hr) && result.proxied && !result.refused && fresh.Get() != old,
                  "a bound view of the proxy does not stop a resize (known difference from DXGI): the game gets a new proxy");
            fresh.Reset();
            g.context->OMSetRenderTargets(0, nullptr, nullptr);
            rtv.Reset();
            g.context->Flush();
        }

        // to unproxied, and back
        request.wantProxy = false;
        request.rawFormat = DXGI_FORMAT_UNKNOWN; // the game said "keep": the resolved format is used
        result = chain.Resize(g.device.Get(), real.Get(), request);
        real->GetDesc(&realDesc);
        Check(SUCCEEDED(result.hr) && !result.proxied && !chain.Active() && !SdrToHdr::AnyProxied() &&
                  realDesc.BufferDesc.Format == kGame,
              "Resize to unproxied: no proxy, nothing registered, the real chain is back to the game's format");
        {
            ComPtr<ID3D11Texture2D> viaGame, own;
            real->GetBuffer(0, IID_PPV_ARGS(&own));
            SdrToHdr::GameBuffer(real.Get(), 0, IID_PPV_ARGS(&viaGame));
            Check(viaGame.Get() == own.Get(), "GameBuffer passes through again");
        }
        request.wantProxy = true;
        request.rawFormat = kGame;
        result = chain.Resize(g.device.Get(), real.Get(), request);
        real->GetDesc(&realDesc);
        Check(SUCCEEDED(result.hr) && result.proxied && realDesc.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
              "Resize to proxied again: the real chain is FP16");
        Check(Frame(g, chain, real.Get(), 0.6f), "a frame converts again");

        // a proxy that cannot be made after the real resize
        request.resolvedFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
        result = chain.Resize(g.device.Get(), real.Get(), request);
        real->GetDesc(&realDesc);
        Check(result.reverted && !result.proxied && !chain.Active() && realDesc.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM,
              "a proxy that cannot be made: the chain is put back to the game's format, unproxied");
        request.resolvedFormat = request.rawFormat = kGame;
        result = chain.Resize(g.device.Get(), real.Get(), request);
        Check(SUCCEEDED(result.hr) && result.proxied, "and the next resize makes the proxy again");

        // ---- inside the real DXGI resize: a window message the game answers ----
        {
            struct During
            {
                SdrToHdr::ProxyChainDx11* chain;
                ID3D11Device* device;
                IDXGISwapChain3* real;
                SdrToHdr::ResizeRequest request;
                bool ran = false, inResize = false, engaged = false, formatIsGame = false, getBufferRefused = false;
                bool nestedRefused = false, otherThreadDone = false;
                bool activeInside = true; // Active() inside the real call: false (the gate the wrapper routes on is Engaged())
                std::thread resizer;      // a second thread's resize, started inside, joined after the outer one
                SdrToHdr::ResizeResult secondResult;
            };
            // static: a thread of a failing run may touch it after this block
            static During during { &chain, g.device.Get(), real.Get(), request };

            SdrToHdr::ProxyChainBase::Hooks().context = &during;
            SdrToHdr::ProxyChainBase::Hooks().duringRealResize = [](void* context)
            {
                auto& d = *static_cast<During*>(context);
                d.ran = true;
                d.inResize = d.chain->InResize();
                d.engaged = d.chain->Engaged();
                d.activeInside = d.chain->Active();
                d.formatIsGame = d.chain->ReportedFormat() == kGame;
                ID3D11Texture2D* buffer = reinterpret_cast<ID3D11Texture2D*>(1);
                d.getBufferRefused = d.chain->GetBuffer(0, IID_PPV_ARGS(&buffer)) == DXGI_ERROR_INVALID_CALL && buffer == nullptr;
                const auto nested = d.chain->Resize(d.device, d.real, d.request);
                d.nestedRefused = nested.refused && nested.hr == DXGI_ERROR_INVALID_CALL;

                // Another thread taking the chain's lock must not wait for the resize: it is not held across it
                std::thread other(
                    [&d]()
                    {
                        bool relaxed = false;
                        d.chain->OwnsResource(nullptr, relaxed);
                        d.otherThreadDone = true;
                    });
                other.join();

                // A resize from another thread waits for this one (DXGI serialises them); it is not refused
                d.resizer = std::thread([&d]() { d.secondResult = d.chain->Resize(d.device, d.real, d.request); });
            };

            request.width = 72;
            request.height = 36;
            result = chain.Resize(g.device.Get(), real.Get(), request);
            SdrToHdr::ProxyChainBase::Hooks() = {};
            Check(during.ran && during.inResize && during.engaged && during.formatIsGame,
                  "inside the real resize: in-resize, engaged, the game's format is what GetDesc is told");
            Check(during.getBufferRefused, "inside the real resize: GetBuffer says INVALID_CALL (not the real HDR buffer)");
            Check(during.engaged && !during.activeInside,
                  "inside the real resize: Engaged() is true while Active() is false (the gate GetBuffer / GetDesc are routed on)");

            if (during.resizer.joinable())
                during.resizer.join();

            Check(during.secondResult.hr == S_OK && !during.secondResult.refused,
                  "inside the real resize: a resize from another thread waits for it and succeeds (not refused)");
            Check(during.nestedRefused, "inside the real resize: a nested resize is refused");
            Check(during.otherThreadDone, "inside the real resize: another thread is not blocked on the chain's lock");
            Check(SUCCEEDED(result.hr) && result.proxied && !chain.InResize(), "and the outer resize finishes, proxied, no longer in resize");
        }

        // ---- a failed real resize that asked for another format: DXGI keeps the old one ----
        {
            const auto saved = request;
            request.count = 17; // the real resize fails
            request.rawFormat = request.resolvedFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            result = chain.Resize(g.device.Get(), real.Get(), request);
            ComPtr<ID3D11Texture2D> proxy;
            D3D11_TEXTURE2D_DESC d {};
            chain.GetBuffer(0, IID_PPV_ARGS(&proxy));

            if (proxy != nullptr)
                proxy->GetDesc(&d);

            Check(FAILED(result.hr) && result.proxied && chain.Active() && chain.ReportedFormat() == kGame && d.Format == kGame,
                  "a failed resize that asked for B8G8R8A8: the proxy is rebuilt in the previous format, still reported as it was");
            proxy.Reset();
            request = saved;
            result = chain.Resize(g.device.Get(), real.Get(), request);
            Check(SUCCEEDED(result.hr) && result.proxied, "and the next resize works");
        }

        // ---- fault injection: the proxy cannot be made AND the chain cannot be put back ----
        {
            SdrToHdr::ProxyChainBase::Hooks().failBuild = true;
            request.width = 64;
            request.height = 32;
            result = chain.Resize(g.device.Get(), real.Get(), request);
            real->GetDesc(&realDesc);
            Check(result.reverted && !result.proxied && !result.stuckHdr && realDesc.BufferDesc.Format == kGame,
                  "a failed proxy build after the real resize: reverted to the game's format");

            SdrToHdr::ProxyChainBase::Hooks().failBuild = true;
            SdrToHdr::ProxyChainBase::Hooks().failRevert = true;
            result = chain.Resize(g.device.Get(), real.Get(), request);
            SdrToHdr::ProxyChainBase::Hooks() = {};
            real->GetDesc(&realDesc);
            Check(result.stuckHdr && !result.proxied && realDesc.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT,
                  "the build fails and the revert fails: reported stuck HDR, the real chain is still FP16");
            request.wantProxy = true;
            result = chain.Resize(g.device.Get(), real.Get(), request);
            Check(SUCCEEDED(result.hr) && result.proxied, "and the next resize makes the proxy");
        }

        // HDR10: the pass writes PQ because the real chain is 10 bit
        {
            request.hdr10 = true;
            result = chain.Resize(g.device.Get(), real.Get(), request);
            real->GetDesc(&realDesc);
            Check(SUCCEEDED(result.hr) && result.proxied && realDesc.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM,
                  "Resize to HDR10: the real chain is 10 bit");
            ComPtr<ID3D11Texture2D> proxy, realBuffer;
            chain.GetBuffer(0, IID_PPV_ARGS(&proxy));
            real->GetBuffer(0, IID_PPV_ARGS(&realBuffer));
            Paint(g, proxy.Get(), 128.0f / 255.0f);
            const char* why = chain.Present(g.context.Get(), real.Get(), Constants()); // Constants() says scRGB
            const uint32_t code = ReadFirstPixel10(g, realBuffer.Get());
            const double nits = std::pow(128.0 / 255.0, 2.2) * 203.0;
            const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0, c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
            const double p = std::pow(nits / 10000.0, m1);
            const double expected = std::pow((c1 + c2 * p) / (1.0 + c3 * p), m2) * 1023.0;
            char what[128];
            std::snprintf(what, sizeof(what), "HDR10: the red code is %u, PQ of the grey is %.1f", code, expected);
            Check(why == nullptr && std::fabs((double) code - expected) <= 1.0, what);
            proxy.Reset();
            realBuffer.Reset();
            request.hdr10 = false;
            result = chain.Resize(g.device.Get(), real.Get(), request);
            Check(SUCCEEDED(result.hr) && result.proxied, "and back to scRGB");
        }

        // full screen
        ShowWindow(window, SW_SHOWNOACTIVATE);
        const HRESULT full = real->SetFullscreenState(TRUE, nullptr);
        std::printf("SetFullscreenState(TRUE) -> %08lX%s\n", (unsigned long) full,
                    FAILED(full) ? " (refused by WARP or this session: the full screen cases are skipped)" : "");

        if (SUCCEEDED(full))
        {
            request.width = request.height = 0;
            result = chain.Resize(g.device.Get(), real.Get(), request);
            Check(SUCCEEDED(result.hr) && result.proxied, "full screen: Resize stays proxied");
            Check(Frame(g, chain, real.Get(), 0.5f), "full screen: a frame converts");
            Check(SUCCEEDED(real->SetFullscreenState(FALSE, nullptr)), "SetFullscreenState(FALSE)");
            result = chain.Resize(g.device.Get(), real.Get(), request);
            Check(SUCCEEDED(result.hr) && result.proxied, "windowed again: Resize stays proxied");
            Check(Frame(g, chain, real.Get(), 0.5f), "windowed again: a frame converts");
        }

        chain.Release();
        Check(!chain.Active() && !SdrToHdr::AnyProxied(), "Release: inactive, nothing registered");
        real.Reset();
        g.context->ClearState();
        g.context->Flush();
    }

    DestroyWindow(window);
}
} // namespace

// ---- an older (bitblt) request made flip model (S2b): the same desc edit PrepareCreate makes, then proxy and convert ----
void Upgraded(Gpu& g, DXGI_SWAP_EFFECT asked, const char* label)
{
    HWND window = MakeWindow();

    {
        // what the game asked for
        const UINT gameCount = 1;
        const auto edit = SdrToHdr::UpgradeSwapDesc(asked, gameCount, 0);

        DXGI_SWAP_CHAIN_DESC1 desc {};
        desc.Width = 64;
        desc.Height = 32;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // the real HDR format
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = edit.bufferCount;
        desc.SwapEffect = edit.swapEffect;
        desc.Flags = edit.flags;
        ComPtr<IDXGISwapChain1> chain1;
        ComPtr<IDXGISwapChain3> real;
        const HRESULT created = g.factory->CreateSwapChainForHwnd(g.device.Get(), window, &desc, nullptr, nullptr, &chain1);
        char what[200];
        std::snprintf(what, sizeof(what), "%s, 1 buffer: the upgraded FP16 chain is made (%08lX)", label, (unsigned long) created);
        Check(SUCCEEDED(created), what);

        if (FAILED(created))
        {
            DestroyWindow(window);
            return;
        }

        chain1.As(&real);
        DXGI_SWAP_CHAIN_DESC realDesc {};
        real->GetDesc(&realDesc);
        std::snprintf(what, sizeof(what), "%s: the real chain is flip model with two buffers (what the wrapper hides from the game)", label);
        Check(realDesc.SwapEffect == edit.swapEffect && realDesc.BufferCount == 2, what);

        SdrToHdr::ProxyChainDx11 chain;
        const char* started = chain.Start(g.device.Get(), real.Get(), kGame, false);
        std::snprintf(what, sizeof(what), "%s: Start on the upgraded chain", label);
        Check(started == nullptr, what);

        if (started == nullptr)
        {
            // one proxy texture and GetBuffer(0) whatever the buffer count: the game's bitblt GetBuffer(0) semantics hold
            ComPtr<ID3D11Texture2D> proxy, second;
            Check(SUCCEEDED(chain.GetBuffer(0, IID_PPV_ARGS(&proxy))) && chain.GetBuffer(1, IID_PPV_ARGS(&second)) == DXGI_ERROR_INVALID_CALL,
                  "the proxy is one texture, GetBuffer(0) only, with two real buffers");
            proxy.Reset();
            bool all = true;

            for (int i = 0; i < 4; ++i)
                all = Frame(g, chain, real.Get(), 0.3f + 0.1f * (float) i) && all;

            Check(all, "four frames over the two real buffers convert");

            // the game's resize with a count of 1 (the wrapper raises it to 2): works and stays proxied; the raw 1 would fail
            SdrToHdr::ResizeRequest request;
            request.rawFormat = request.resolvedFormat = kGame;
            request.wantProxy = true;
            request.width = 96;
            request.height = 48;
            request.count = SdrToHdr::UpgradedResizeCount(1);
            auto result = chain.Resize(g.device.Get(), real.Get(), request);
            Check(SUCCEEDED(result.hr) && result.proxied && request.count == 2, "resize with the game's count of 1 (raised to 2): works, stays proxied");
            Check(Frame(g, chain, real.Get(), 0.5f), "a frame after that resize converts");
            request.count = 0; // keep
            request.width = request.height = 0;
            result = chain.Resize(g.device.Get(), real.Get(), request);
            Check(SUCCEEDED(result.hr) && result.proxied, "resize with a kept count (0) works");

            // unproxied: the real chain stays flip model, still two buffers
            request.wantProxy = false;
            request.rawFormat = DXGI_FORMAT_UNKNOWN;
            result = chain.Resize(g.device.Get(), real.Get(), request);
            real->GetDesc(&realDesc);
            Check(SUCCEEDED(result.hr) && !result.proxied && realDesc.SwapEffect == edit.swapEffect && realDesc.BufferCount == 2,
                  "unproxied at a resize: the real chain is still flip model with two buffers");
            chain.Release();
        }

        // the facts the wrapper's choices rest on, read from DXGI (a flip chain, FP16)
        real.Reset();
        chain1.Reset();
    }

    DestroyWindow(window);
}

// What a flip chain does with what a bitblt game passes to Present / Present1 / ResizeBuffers, and with full screen
void FlipFacts(Gpu& g)
{
    HWND window = MakeWindow();

    for (DXGI_SWAP_EFFECT effect : { DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL })
    {
        const bool discard = effect == DXGI_SWAP_EFFECT_FLIP_DISCARD;
        auto chain = MakeChainEffect(g, window, effect);

        if (chain == nullptr)
        {
            Check(false, "a flip chain for the Present facts is made");
            continue;
        }

        Check(SUCCEEDED(chain->Present(0, 0)) && SUCCEEDED(chain->Present(1, 0)) &&
                  SUCCEEDED(chain->Present(0, DXGI_PRESENT_DO_NOT_SEQUENCE)) && SUCCEEDED(chain->Present(0, DXGI_PRESENT_RESTART)) &&
                  SUCCEEDED(chain->Present(0, DXGI_PRESENT_TEST)),
              discard ? "fact: FLIP_DISCARD takes Present with sync 0 and 1, DO_NOT_SEQUENCE, RESTART and TEST"
                      : "fact: FLIP_SEQUENTIAL takes Present with sync 0 and 1, DO_NOT_SEQUENCE, RESTART and TEST");

        RECT dirty { 1, 1, 10, 10 };
        DXGI_PRESENT_PARAMETERS withDirty {};
        withDirty.DirtyRectsCount = 1;
        withDirty.pDirtyRects = &dirty;
        const HRESULT dirtyResult = chain->Present1(0, 0, &withDirty);
        Check(discard ? dirtyResult == DXGI_ERROR_INVALID_CALL : SUCCEEDED(dirtyResult),
              discard ? "fact: FLIP_DISCARD REFUSES Present1 dirty rectangles (INVALID_CALL), a bitblt chain took them"
                      : "fact: FLIP_SEQUENTIAL takes Present1 dirty rectangles");

        RECT scroll { 0, 0, 32, 32 };
        POINT offset { 2, 2 };
        DXGI_PRESENT_PARAMETERS withScroll {};
        withScroll.pScrollRect = &scroll;
        withScroll.pScrollOffset = &offset;
        Check(chain->Present1(0, 0, &withScroll) == DXGI_ERROR_INVALID_CALL, "fact: a flip chain REFUSES a Present1 scroll rectangle");

        DXGI_PRESENT_PARAMETERS none {};
        Check(SUCCEEDED(chain->Present1(0, 0, &none)),
              "fact: Present1 with empty parameters works (a null pointer is not allowed: the wrapper then calls Present)");

        {
            const HRESULT one = chain->ResizeBuffers(1, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
            Check(one == E_INVALIDARG,
                  "fact: a flip chain REFUSES ResizeBuffers with one buffer (E_INVALIDARG; why the wrapper raises the game's count of 1)");
        }

        Check(SUCCEEDED(chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0)), "fact: ResizeBuffers with a kept count (0) works");
    }

    DestroyWindow(window);
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // (a crash must not swallow the lines before it)
    const bool warp = argc > 1 && std::strcmp(argv[1], "warp") == 0;
    Gpu g;

    if (!MakeGpu(g, warp))
    {
        std::printf("no D3D11 device\n");
        return 2;
    }

    std::printf("%s, debug layer: %s\n", warp ? "WARP" : "hardware adapter",
                g.debugLayer ? "on" : "not installed (its check is skipped)");

    const auto before = LiveObjects(g);

    {
        Facts(g);

        SdrToHdr::Dx11Pass pass;
        Check(!pass.Ready(), "not ready before Init");
        Check(pass.Init(nullptr) != nullptr, "Init without a device says why");

        Maths(g, pass, DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM proxy", warp);
        Maths(g, pass, DXGI_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM proxy", warp);
        Maths(g, pass, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "R8G8B8A8_UNORM_SRGB typed proxy (srcLinear)", warp);
        ConvertToggle(g, pass);
        StateKept(g, pass);

        pass.Release();
        Check(!pass.Ready(), "not ready after Release");
    }

    Chains(g, warp);
    Upgraded(g, DXGI_SWAP_EFFECT_DISCARD, "bitblt DISCARD");
    Upgraded(g, DXGI_SWAP_EFFECT_SEQUENTIAL, "bitblt SEQUENTIAL");
    FlipFacts(g);

    g.context->ClearState();
    g.context->Flush();
    const auto after = LiveObjects(g);

    // Everything made by the test is released: the live objects are what they were
    bool same = before.size() == after.size();
    std::string diff;

    for (size_t i = 0; same && i < before.size(); ++i)
    {
        if (before[i] != after[i])
        {
            same = false;
            diff = before[i].first + " " + std::to_string(before[i].second) + " -> " + std::to_string(after[i].second);
        }
    }

    char what[200];
    std::snprintf(what, sizeof(what), "live objects after everything is released are the same as before (%zu kinds)%s%s",
                  before.size(), diff.empty() ? "" : ": ", diff.c_str());
    Check(g.debug == nullptr || same, what);

    const int messages = DebugMessages(g);
    char debug[96];
    std::snprintf(debug, sizeof(debug), "the debug layer reported %d errors or warnings", messages);
    Check(messages == 0, debug);

    std::printf("%s (%d failed)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
