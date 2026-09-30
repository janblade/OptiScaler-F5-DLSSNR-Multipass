// Headless shader comparison: Windows D3D11 WARP runs dlssnr.hlsl's encode, downsample and resolve to compare the
// three ways a model that ran below the frame's size comes back ([DlssNr] Transfer): 0 Classic (the answer
// enlarged bilinearly), 1 Matched residual (the RGB change enlarged and added), 2 NVIDIA residual (the change
// enlarged in OkLab: brightness as a ratio, colour as a difference).
//
// Each scene is a row with hard edges, fine stripes or saturated colour. A known edit is made to it at full size
// (brighten the highlights, deepen the shadows, local contrast, +10%, a colour shift), standing in for the model.
// The reference is the resolve with the model at full size (Transfer 0, work scale 1). Each mode then gets only
// the half-size pair -- the frame's proxy and the edited proxy, downsampled 2:1 by the shader -- and is scored
// against that reference on the resolved frame:
//
//   err    mean |Y - Y_ref| / max(Y_ref, 0.005), all pixels
//   halo   the largest brightening past the reference on dark pixels (frame Y < 0.1), relative, and `at`, the pixel
//          where it occurs
//   carried  how much of the edit arrives: sum |Y - Y_frame| / sum |Y_ref - Y_frame| (1 = all of it)
//   hue    the largest change of chromaticity (rgb / Y) past the reference, on pixels the edit touched
//
// It prints a table; it fails only when a mode's output is not finite or the harness breaks. A scoreboard of the
// modes, not a regression test: none of its scenes reaches below OkLab L 0.1, so the near-black fix
// (nr_proxy_curves_shader_smoke.cpp section 7) does not show here.
//
// Build: cl /nologo /std:c++20 /EHsc /W3 tests\nr_residual_compare_shader_smoke.cpp /link d3d11.lib d3dcompiler.lib
// Run:   nr_residual_compare_shader_smoke.exe <dlssnr.hlsl>
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include "../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
using Microsoft::WRL::ComPtr;

struct Pixel { float r, g, b, a; };
using Row = std::vector<Pixel>;

static void check(HRESULT hr, int line = __builtin_LINE())
{
    if (FAILED(hr)) throw std::runtime_error("D3D call failed, line " + std::to_string(line));
}
static int fails = 0;
static void expect(bool ok, const std::string& label) { if (!ok) { std::printf("FAIL: %s\n", label.c_str()); ++fails; } }

struct Gpu
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> linearSampler;

    Gpu()
    {
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx));
        D3D11_BUFFER_DESC buffer {};
        buffer.ByteWidth = sizeof(DlssNrConstants);
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&buffer, nullptr, &constants));
        // gLinear, as the game binds it (DlssNr_Dx12.cpp: MIN_MAG_MIP_LINEAR, clamped).
        D3D11_SAMPLER_DESC sampling {};
        sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampling.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampling, &linearSampler));
    }

    ComPtr<ID3D11ComputeShader> Compile(const wchar_t* path)
    {
        ComPtr<ID3DBlob> code, errors;
        HRESULT hr = D3DCompileFromFile(path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSMain", "cs_5_0",
                                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (errors && FAILED(hr)) std::fprintf(stderr, "%s", (char*) errors->GetBufferPointer());
        check(hr);
        ComPtr<ID3D11ComputeShader> shader;
        check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
        return shader;
    }

    ComPtr<ID3D11Texture2D> Texture(UINT width, const Row* data)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = width; desc.Height = 1; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        D3D11_SUBRESOURCE_DATA init {data ? data->data() : nullptr, (UINT) (width * sizeof(Pixel)), 0};
        ComPtr<ID3D11Texture2D> texture;
        check(device->CreateTexture2D(&desc, data ? &init : nullptr, &texture));
        return texture;
    }

    Row Read(ID3D11Texture2D* texture)
    {
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&desc, nullptr, &staging));
        ctx->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        check(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        Row row(desc.Width);
        std::memcpy(row.data(), mapped.pData, desc.Width * sizeof(Pixel));
        ctx->Unmap(staging.Get(), 0);
        return row;
    }

    // One dispatch: t0..t2 as given (t3/t4 get t0, unread on these paths), u0 target, u1 keep.
    void Run(ID3D11ComputeShader* shader, const DlssNrConstants& settings, ID3D11Texture2D* t0, ID3D11Texture2D* t1,
             ID3D11Texture2D* t2, ID3D11Texture2D* u0, ID3D11Texture2D* u1)
    {
        ComPtr<ID3D11ShaderResourceView> s0, s1, s2;
        check(device->CreateShaderResourceView(t0, nullptr, &s0));
        check(device->CreateShaderResourceView(t1 ? t1 : t0, nullptr, &s1));
        check(device->CreateShaderResourceView(t2 ? t2 : t0, nullptr, &s2));
        ComPtr<ID3D11UnorderedAccessView> v0, v1;
        check(device->CreateUnorderedAccessView(u0, nullptr, &v0));
        check(device->CreateUnorderedAccessView(u1, nullptr, &v1));
        ID3D11ShaderResourceView* srvs[] = {s0.Get(), s1.Get(), s2.Get(), s0.Get(), s0.Get()};
        ID3D11UnorderedAccessView* uavs[] = {v0.Get(), v1.Get()};
        ctx->UpdateSubresource(constants.Get(), 0, nullptr, &settings, 0, 0);
        ctx->CSSetShader(shader, nullptr, 0);
        ctx->CSSetShaderResources(0, 5, srvs);
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
        ctx->CSSetSamplers(0, 1, linearSampler.GetAddressOf());
        ctx->Dispatch((settings.Width + 7) / 8, 1, 1); // 8x8 groups
        ID3D11ShaderResourceView* noSrvs[5] = {};
        ID3D11UnorderedAccessView* noUavs[2] = {};
        ctx->CSSetShaderResources(0, 5, noSrvs);
        ctx->CSSetUnorderedAccessViews(0, 2, noUavs, nullptr);
    }
};

static DlssNrConstants Base(uint32_t mode, UINT width, uint32_t reversible)
{
    DlssNrConstants k {};
    k.Mode = mode;
    k.Width = width; k.Height = 1;
    k.WhitePoint = 1.0f;
    k.TransferStrength = 1.0f; k.ColourStrength = 1.0f;
    k.MaxRatio = 8.0f;
    k.ApplyModel = 1;
    k.ReversibleMode = reversible;
    k.DebugScale = 1.0f;
    k.ModelWorkScale = 1.0f;
    k.PassFeedback = 1.0f;
    return k;
}

// The encode's proxy and kept copy, and the proxy's 2:1 downsample, left on the GPU.
struct Encoded { ComPtr<ID3D11Texture2D> proxy, keep, half; };

static Encoded Encode(Gpu& gpu, ID3D11ComputeShader* shader, const Row& input, uint32_t reversible)
{
    const UINT n = (UINT) input.size();
    auto source = gpu.Texture(n, &input);
    Encoded e {gpu.Texture(n, nullptr), gpu.Texture(n, nullptr), gpu.Texture(n / 2, nullptr)};
    auto spare = gpu.Texture(n, nullptr);
    gpu.Run(shader, Base(DlssNrMode_Encode, n, reversible), source.Get(), nullptr, nullptr, e.proxy.Get(), e.keep.Get());
    gpu.Run(shader, Base(DlssNrMode_Downsample, n / 2, reversible), e.proxy.Get(), nullptr, nullptr, e.half.Get(), spare.Get());
    return e;
}

// Resolve only: t0 the proxy the model saw, t1 its answer, t2 the kept frame.
static Row Resolve(Gpu& gpu, ID3D11ComputeShader* shader, ID3D11Texture2D* proxy, ID3D11Texture2D* model, ID3D11Texture2D* keep,
                   UINT n, uint32_t reversible, uint32_t transfer, float workScale)
{
    auto frame = gpu.Texture(n, nullptr);
    auto spare = gpu.Texture(n, nullptr);
    DlssNrConstants k = Base(DlssNrMode_Resolve, n, reversible);
    k.Transfer = transfer;
    k.ModelWorkScale = workScale;
    gpu.Run(shader, k, proxy, model, keep, frame.Get(), spare.Get());
    return gpu.Read(frame.Get());
}

static float Luma(const Pixel& p) { return 0.2126f * p.r + 0.7152f * p.g + 0.0722f * p.b; }

static bool Finite(const Row& row)
{
    for (const Pixel& p : row)
        if (!std::isfinite(p.r) || !std::isfinite(p.g) || !std::isfinite(p.b))
            return false;
    return true;
}

// Scenes, 64 pixels, edges on odd pixels so a half-size texel straddles them.
static Row EdgeScene()
{
    Row row;
    for (int i = 0; i < 64; ++i)
    {
        const bool bright = (i >= 13 && i < 29) || (i >= 45 && i < 55);
        const float v = bright ? 0.8f : 0.02f;
        row.push_back({v, v, v, 1});
    }
    return row;
}
static Row StripeScene()
{
    Row row;
    for (int i = 0; i < 64; ++i)
    {
        const float v = (i / 3) % 2 ? 0.5f : 0.04f; // stripes three pixels wide: not a multiple of the 2:1 grid
        row.push_back({v, v, v, 1});
    }
    return row;
}
static Row ColourScene()
{
    Row row;
    for (int i = 0; i < 64; ++i)
    {
        const int band = ((i + 3) / 8) % 4;
        const Pixel colours[4] = {{0.7f, 0.06f, 0.03f, 1}, {0.02f, 0.03f, 0.12f, 1}, {0.1f, 0.5f, 0.08f, 1}, {0.9f, 0.8f, 0.6f, 1}};
        row.push_back(colours[band]);
    }
    return row;
}

using Edit = std::function<Row(const Row&)>;

static Row EachPixel(const Row& row, const std::function<Pixel(const Pixel&)>& f)
{
    Row out;
    for (const Pixel& p : row)
        out.push_back(f(p));
    return out;
}

int wmain(int argc, wchar_t** argv) try
{
    if (argc != 2)
        throw std::runtime_error("Pass the dlssnr.hlsl path");
    Gpu gpu;
    auto shader = gpu.Compile(argv[1]);

    struct Scene { const char* name; Row row; };
    const Scene scenes[] = {{"edges", EdgeScene()}, {"stripes", StripeScene()}, {"colour", ColourScene()}};

    struct NamedEdit { const char* name; Edit edit; };
    const NamedEdit edits[] = {
        {"brighten highlights x1.2",
         [](const Row& r) { return EachPixel(r, [](const Pixel& p) { const float s = Luma(p) > 0.3f ? 1.2f : 1.0f; return Pixel {p.r * s, p.g * s, p.b * s, 1}; }); }},
        {"deepen shadows x0.7",
         [](const Row& r) { return EachPixel(r, [](const Pixel& p) { const float s = Luma(p) < 0.1f ? 0.7f : 1.0f; return Pixel {p.r * s, p.g * s, p.b * s, 1}; }); }},
        {"local contrast",
         [](const Row& r)
         {
             Row out;
             for (size_t i = 0; i < r.size(); ++i)
             {
                 const Pixel& a = r[i ? i - 1 : 0];
                 const Pixel& b = r[i];
                 const Pixel& c = r[std::min(i + 1, r.size() - 1)];
                 auto boost = [](float x, float y, float z) { return std::max(y + 0.5f * (y - (x + y + z) / 3.0f), 0.0f); };
                 out.push_back({boost(a.r, b.r, c.r), boost(a.g, b.g, c.g), boost(a.b, b.b, c.b), 1});
             }
             return out;
         }},
        {"everything +10%", [](const Row& r) { return EachPixel(r, [](const Pixel& p) { return Pixel {p.r * 1.1f, p.g * 1.1f, p.b * 1.1f, 1}; }); }},
        {"colour shift (green x0.8)",
         [](const Row& r) { return EachPixel(r, [](const Pixel& p) { return Pixel {p.r, p.g * 0.8f, p.b, 1}; }); }},
    };

    struct Curve { uint32_t mode; const char* name; };
    const Curve curves[] = {{0, "soft knee"}, {5, "HLG"}};
    const char* modeNames[3] = {"Classic", "Matched", "NVIDIA"};

    // Totals per curve and mode, over every scene and edit.
    double totalErr[2][3] = {}, worstHalo[2][3] = {}, totalCarried[2][3] = {};
    int cases = 0;

    for (int ci = 0; ci < 2; ++ci)
    {
        const Curve& curve = curves[ci];
        std::printf("\n== %s (ReversibleMode %u), model at half size\n", curve.name, curve.mode);
        std::printf("%-8s %-27s %-8s %8s %8s %4s %8s %8s\n", "scene", "edit", "mode", "err", "halo", "at", "carried", "hue");
        for (const Scene& scene : scenes)
            for (const NamedEdit& edit : edits)
            {
                const Row& frame = scene.row;
                const Row answer = edit.edit(frame);
                const UINT n = (UINT) frame.size();
                const Encoded f = Encode(gpu, shader.Get(), frame, curve.mode);
                const Encoded a = Encode(gpu, shader.Get(), answer, curve.mode);
                const Row reference = Resolve(gpu, shader.Get(), f.proxy.Get(), a.proxy.Get(), f.keep.Get(), n, curve.mode, 0, 1.0f);
                expect(Finite(reference), std::string("reference not finite: ") + scene.name + ", " + edit.name);

                for (uint32_t transfer = 0; transfer < 3; ++transfer)
                {
                    const Row got = Resolve(gpu, shader.Get(), f.half.Get(), a.half.Get(), f.keep.Get(), n, curve.mode, transfer, 0.5f);
                    expect(Finite(got), std::string("not finite: ") + scene.name + ", " + edit.name + ", " + modeNames[transfer]);

                    double err = 0.0, moved = 0.0, wanted = 0.0;
                    float halo = 0.0f, hue = 0.0f;
                    int haloAt = -1;
                    for (UINT i = 0; i < n; ++i)
                    {
                        const float y = Luma(got[i]), yRef = Luma(reference[i]), yFrame = Luma(frame[i]);
                        const float scale = std::max(yRef, 0.005f);
                        err += std::abs(y - yRef) / scale;
                        moved += std::abs(y - yFrame);
                        wanted += std::abs(yRef - yFrame);
                        if (yFrame < 0.1f && (y - yRef) / scale > halo)
                        {
                            halo = (y - yRef) / scale;
                            haloAt = (int) i;
                        }
                        if (std::abs(yRef - yFrame) > 1e-4f || std::abs(Luma(answer[i]) - yFrame) > 1e-4f ||
                            std::abs(answer[i].g - frame[i].g) > 1e-4f)
                        {
                            const float yy = std::max(y, 1e-4f), yr = std::max(yRef, 1e-4f);
                            hue = std::max({hue, std::abs(got[i].r / yy - reference[i].r / yr), std::abs(got[i].g / yy - reference[i].g / yr),
                                            std::abs(got[i].b / yy - reference[i].b / yr)});
                        }
                    }
                    err /= n;
                    const double carried = wanted > 1e-6 ? moved / wanted : 1.0;
                    std::printf("%-8s %-27s %-8s %8.4f %8.4f %4d %8.3f %8.4f\n", scene.name, edit.name, modeNames[transfer], err, halo,
                                haloAt, carried, hue);
                    totalErr[ci][transfer] += err;
                    worstHalo[ci][transfer] = std::max(worstHalo[ci][transfer], (double) halo);
                    totalCarried[ci][transfer] += std::abs(carried - 1.0);
                }
                if (ci == 0)
                    ++cases;
            }
    }

    std::printf("\n== Summary over %d scene x edit cases (lower is better)\n", cases);
    std::printf("%-10s %-8s %10s %10s %14s\n", "curve", "mode", "mean err", "worst halo", "mean |carried-1|");
    for (int ci = 0; ci < 2; ++ci)
        for (int t = 0; t < 3; ++t)
            std::printf("%-10s %-8s %10.4f %10.4f %14.4f\n", curves[ci].name, modeNames[t], totalErr[ci][t] / cases, worstHalo[ci][t],
                        totalCarried[ci][t] / cases);

    if (fails)
    {
        std::printf("%d check(s) failed\n", fails);
        return 1;
    }
    std::puts("PASS: every mode finite on every scene and edit (comparison above, WARP HLSL)");
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
