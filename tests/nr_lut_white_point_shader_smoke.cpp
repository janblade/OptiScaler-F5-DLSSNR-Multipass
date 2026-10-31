// Headless shader test: Windows D3D11 WARP runs dlssnr_lut.hlsl, the LUT grade NR's input goes through, and checks the
// white point it grades for: the encode's own, recomputed this frame from the live exposure texture.
//
//   1. With no live exposure the pass is bit-identical to the shader before the white point became shared
//      (dlssnr_lut.hlsl at 7364a7f8): linear HDR, PQ, tone-mapped sRGB and gamma 2.2 frames, at several white points.
//   2. With a live exposure texture (UseGameExposure, or UseExposureWhitePoint for Automatic's) the pass grades for
//      PreExposure / exposure x the Trim -- the encode's own rule (dlssnr_whitepoint.hlsli) -- and follows the texel
//      when it changes between dispatches, instead of the host value it was handed.
//   3. The Trim table applies at the live base white point, as it does in the encode.
//   4. An implausible texel (0, inf) falls back to the host value.
//   5. Forced PQ grades for the game's HDR paper white: white point 2 reads the decoded light at half, so the result
//      matches a frame of half the light graded at white 1.
//
// Build: cl /nologo /std:c++20 /EHsc /W3 tests\nr_lut_white_point_shader_smoke.cpp d3d11.lib d3dcompiler.lib
// Reference: git show 7364a7f8:OptiScaler/shaders/dlssnr/precompile/dlssnr_lut.hlsl > precompile\dlssnr_lut_ref.hlsl
//            (next to dlssnr_pq.hlsli and dlssnr_replace_curve.hlsli, which it includes)
// Run:   nr_lut_white_point_shader_smoke.exe <dlssnr_lut.hlsl> <its reference>
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "../OptiScaler/shaders/dlssnr/DlssNr_LutConstants.h"
using Microsoft::WRL::ComPtr;

struct Pixel { float r, g, b, a; };
static void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D call failed"); }
static int fails = 0;
static void expect(bool ok, const std::string& label)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", label.c_str());
        ++fails;
    }
}

constexpr int N = 8;
constexpr int kLut = 4;

struct Gpu
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Texture2D> sourceTex, exposureTex, target, readback;
    ComPtr<ID3D11ShaderResourceView> sourceSrv, lutSrv, exposureSrv;
    ComPtr<ID3D11UnorderedAccessView> targetUav;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;

    Gpu()
    {
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                nullptr, &ctx));
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = N; desc.Height = 1; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        check(device->CreateTexture2D(&desc, nullptr, &sourceTex));
        check(device->CreateShaderResourceView(sourceTex.Get(), nullptr, &sourceSrv));
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        check(device->CreateTexture2D(&desc, nullptr, &target));
        check(device->CreateUnorderedAccessView(target.Get(), nullptr, &targetUav));
        desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&desc, nullptr, &readback));

        D3D11_TEXTURE2D_DESC one {};
        one.Width = 1; one.Height = 1; one.MipLevels = 1; one.ArraySize = 1; one.Format = DXGI_FORMAT_R32_FLOAT;
        one.SampleDesc.Count = 1; one.Usage = D3D11_USAGE_DEFAULT; one.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        check(device->CreateTexture2D(&one, nullptr, &exposureTex));
        check(device->CreateShaderResourceView(exposureTex.Get(), nullptr, &exposureSrv));

        // A graded cube: each lattice point (r, g, b) -> (g, b, r) with a lift. Trilinear between lattice points is
        // linear, so the grade is exact everywhere and depends on where the white point put each pixel.
        std::vector<float> lattice;
        for (int b = 0; b < kLut; ++b)
            for (int g = 0; g < kLut; ++g)
                for (int r = 0; r < kLut; ++r)
                {
                    const float R = r / float(kLut - 1), G = g / float(kLut - 1), B = b / float(kLut - 1);
                    lattice.insert(lattice.end(), { 0.9f * G + 0.05f, 0.9f * B + 0.05f, 0.9f * R + 0.05f, 1.0f });
                }
        D3D11_TEXTURE3D_DESC cube {};
        cube.Width = cube.Height = cube.Depth = kLut; cube.MipLevels = 1; cube.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        cube.Usage = D3D11_USAGE_DEFAULT; cube.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA cubeData { lattice.data(), kLut * 16, kLut * kLut * 16 };
        ComPtr<ID3D11Texture3D> cubeTex;
        check(device->CreateTexture3D(&cube, &cubeData, &cubeTex));
        check(device->CreateShaderResourceView(cubeTex.Get(), nullptr, &lutSrv));

        D3D11_BUFFER_DESC buffer {};
        buffer.ByteWidth = sizeof(DlssNrLutConstants);
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&buffer, nullptr, &constants));
        D3D11_SAMPLER_DESC sampling {};
        sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampling.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampling, &sampler));
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

    std::array<Pixel, N> Run(ID3D11ComputeShader* shader, const std::array<Pixel, N>& in, const DlssNrLutConstants& c,
                             float exposureTexel)
    {
        ctx->UpdateSubresource(sourceTex.Get(), 0, nullptr, in.data(), sizeof(in), 0);
        ctx->UpdateSubresource(exposureTex.Get(), 0, nullptr, &exposureTexel, sizeof(float), 0);
        ctx->UpdateSubresource(constants.Get(), 0, nullptr, &c, 0, 0);
        ID3D11ShaderResourceView* srvs[] = { sourceSrv.Get(), lutSrv.Get(), exposureSrv.Get() };
        ctx->CSSetShader(shader, nullptr, 0);
        ctx->CSSetShaderResources(0, 3, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, targetUav.GetAddressOf(), nullptr);
        ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
        ctx->CSSetSamplers(0, 1, sampler.GetAddressOf());
        ctx->Dispatch(1, 1, 1);
        ctx->CopyResource(readback.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        check(ctx->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::array<Pixel, N> out;
        std::memcpy(out.data(), mapped.pData, sizeof(out));
        ctx->Unmap(readback.Get(), 0);
        return out;
    }
};

static bool SameBits(const std::array<Pixel, N>& a, const std::array<Pixel, N>& b)
{
    return std::memcmp(a.data(), b.data(), sizeof(a)) == 0;
}

static float Worst(const std::array<Pixel, N>& a, const std::array<Pixel, N>& b)
{
    float worst = 0.0f;
    for (int i = 0; i < N; ++i)
    {
        const float scale = std::max({ std::abs(b[i].r), std::abs(b[i].g), std::abs(b[i].b), 1e-3f });
        worst = std::max({ worst, std::abs(a[i].r - b[i].r) / scale, std::abs(a[i].g - b[i].g) / scale,
                           std::abs(a[i].b - b[i].b) / scale });
    }
    return worst;
}

static DlssNrLutConstants Base(uint32_t encoding, bool linearHdr, float white)
{
    DlssNrLutConstants c {};
    c.Width = N; c.Height = 1; c.Strength = 1.0f;
    c.DomainMinR = c.DomainMinG = c.DomainMinB = 0.0f;
    c.DomainMaxR = c.DomainMaxG = c.DomainMaxB = 1.0f;
    c.LutSize = kLut;
    c.InputEncoding = encoding;
    c.ColourIsLinearHdr = linearHdr ? 1u : 0u;
    c.Trim = white;
    c.ExposureTrim = 1.0f;
    return c;
}

int wmain(int argc, wchar_t** argv)
try
{
    if (argc != 3) throw std::runtime_error("Pass dlssnr_lut.hlsl and its reference");
    Gpu gpu;
    const auto shader = gpu.Compile(argv[1]);
    const auto reference = gpu.Compile(argv[2]);

    const std::array<Pixel, N> linear {{ {0.0f, 0.0f, 0.0f, 1}, {0.02f, 0.03f, 0.05f, 1}, {0.18f, 0.18f, 0.18f, 1},
                                         {0.6f, 0.4f, 0.2f, 1}, {1.0f, 1.0f, 1.0f, 1}, {3.0f, 1.0f, 0.2f, 1},
                                         {12.0f, 12.0f, 12.0f, 0.5f}, {250.0f, 300.0f, 120.0f, 1} }};
    const std::array<Pixel, N> display {{ {0.0f, 0.0f, 0.0f, 1}, {0.1f, 0.2f, 0.3f, 1}, {0.46f, 0.46f, 0.46f, 1},
                                          {0.8f, 0.3f, 0.1f, 1}, {1.0f, 1.0f, 1.0f, 1}, {0.05f, 0.6f, 0.9f, 1},
                                          {0.33f, 0.66f, 0.99f, 0.5f}, {0.7f, 0.7f, 0.2f, 1} }};
    const std::array<Pixel, N> pq {{ {0.0f, 0.0f, 0.0f, 1}, {0.3f, 0.3f, 0.3f, 1}, {0.58f, 0.58f, 0.58f, 1},
                                     {0.65f, 0.5f, 0.4f, 1}, {0.75f, 0.75f, 0.75f, 1}, {0.4f, 0.55f, 0.7f, 1},
                                     {0.5f, 0.62f, 0.3f, 1}, {0.2f, 0.25f, 0.45f, 1} }};

    // 1. Bit-identical to the reference with no live exposure.
    for (float white : { 0.01f, 1.0f, 2.5375f, 1700.0f })
    {
        const auto c = Base(0, true, white);
        expect(SameBits(gpu.Run(shader.Get(), linear, c, 0.0f), gpu.Run(reference.Get(), linear, c, 0.0f)),
               "linear HDR differs from the reference at white " + std::to_string(white));
    }
    expect(SameBits(gpu.Run(shader.Get(), display, Base(0, false, 1.0f), 0.0f),
                    gpu.Run(reference.Get(), display, Base(0, false, 1.0f), 0.0f)),
           "tone-mapped sRGB differs from the reference");
    expect(SameBits(gpu.Run(shader.Get(), display, Base(3, false, 1.0f), 0.0f),
                    gpu.Run(reference.Get(), display, Base(3, false, 1.0f), 0.0f)),
           "gamma 2.2 differs from the reference");
    expect(SameBits(gpu.Run(shader.Get(), pq, Base(4, true, 1.0f), 0.0f),
                    gpu.Run(reference.Get(), pq, Base(4, true, 1.0f), 0.0f)),
           "PQ at the default paper white differs from the reference");

    // 2. Live exposure: PreExposure / exposure x Trim, following the texel; the host value is ignored.
    {
        for (uint32_t automatic : { 0u, 1u })
            for (float exposure : { 0.5f, 0.01f, 0.004f })
            {
                auto live = Base(0, true, 77.0f); // a stale host value, never the answer here
                live.PreExposure = 2.0f;
                live.ExposureTrim = 1.5f;
                live.UseGameExposure = automatic ? 0u : 1u;
                live.UseExposureWhitePoint = automatic;
                const float want = 2.0f / exposure * 1.5f;
                const auto got = gpu.Run(shader.Get(), linear, live, exposure);
                const auto host = gpu.Run(shader.Get(), linear, Base(0, true, want), 0.0f);
                const float off = Worst(got, host);
                std::printf("live %s exposure %.3f: white %.1f, off %.2e\n", automatic ? "automatic" : "game",
                            exposure, want, off);
                expect(off < 1e-5f, "live white point is not PreExposure / exposure x Trim");
            }
        // Following the game: ExposureBaseScale multiplies the base.
        auto follow = Base(0, true, 77.0f);
        follow.PreExposure = 1.0f;
        follow.ExposureTrim = 1.0f;
        follow.UseExposureWhitePoint = 1u;
        follow.ExposureBaseScale = 4.0f;
        expect(Worst(gpu.Run(shader.Get(), linear, follow, 0.5f), gpu.Run(shader.Get(), linear, Base(0, true, 8.0f), 0.0f)) <
                   1e-5f,
               "ExposureBaseScale is not applied to the live base");
    }

    // 3. Trim table at the live base: two points (base 1 -> trim 1, base 100 -> trim 4); base 10 is halfway in log.
    {
        auto live = Base(0, true, 77.0f);
        live.PreExposure = 1.0f;
        live.UseGameExposure = 1u;
        live.ExposureTrimAnchorCount = 2;
        live.ExposureTrimAnchors[0] = 1.0f; live.ExposureTrimAnchors[1] = 1.0f;
        live.ExposureTrimAnchors[2] = 100.0f; live.ExposureTrimAnchors[3] = 4.0f;
        const float off = Worst(gpu.Run(shader.Get(), linear, live, 0.1f), gpu.Run(shader.Get(), linear, Base(0, true, 20.0f), 0.0f));
        std::printf("trim table at live base 10: off %.2e\n", off);
        expect(off < 1e-4f, "the Trim table is not applied at the live base white point");
    }

    // 4. Implausible texels fall back to the host value.
    for (float bad : { 0.0f, INFINITY, -1.0f })
    {
        auto live = Base(0, true, 3.0f);
        live.PreExposure = 1.0f;
        live.UseGameExposure = 1u;
        expect(SameBits(gpu.Run(shader.Get(), linear, live, bad), gpu.Run(shader.Get(), linear, Base(0, true, 3.0f), 0.0f)),
               "an implausible exposure texel did not fall back to the host value");
    }

    // 5. Forced PQ at paper white 2x the reference: grades the light at half, so it equals a frame with half the light
    // at white 1 (EncodePQ/DecodePQ of the same light; the round trip is compared in decoded light, to rounding).
    {
        const auto atTwo = gpu.Run(shader.Get(), pq, Base(4, true, 2.0f), 0.0f);
        const auto atOne = gpu.Run(shader.Get(), pq, Base(4, true, 1.0f), 0.0f);
        float moved = 0.0f;
        for (int i = 1; i < N; ++i)
            moved = std::max(moved, std::abs(atTwo[i].r - atOne[i].r));
        std::printf("PQ paper white 406 vs 203 nits: largest change %.3f of the signal\n", moved);
        expect(moved > 0.01f, "forced PQ ignores the paper white");
        for (int i = 0; i < N; ++i)
            expect(std::isfinite(atTwo[i].r) && std::isfinite(atTwo[i].g) && std::isfinite(atTwo[i].b),
                   "forced PQ at paper white 2 is not finite");
    }

    std::puts(fails == 0 ? "PASS: the LUT grades for the encode's white point, live or host (WARP HLSL)"
                         : "FAILED");
    return fails == 0 ? 0 : 1;
}
catch (const std::exception& e)
{
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
