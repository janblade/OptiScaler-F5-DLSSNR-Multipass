// Headless shader test: Windows D3D11 WARP runs dlssnr.hlsl's texture hint ([DlssNr] InputStyle = 4,
// DlssNrMode_TextureHint) on a small synthetic picture: a flat fill with a dark outline and a textured strip.
//
//   1. The pattern lands on the flat fill, within its amplitude; the textured strip, the outline with two pixels
//      around it and a smooth gradient are left exactly as they were.
//   2. With the picture moved 3 px to the right and motion vectors saying so, every coordinate is last frame's one
//      3 px to the left, and the hinted flat fill is last frame's moved by 3 px (the pattern sticks to the surface).
//      A pixel whose vector leaves the picture starts over at its own place.
//   3. A move of half a pixel carries the half too, so the pattern moves smoothly.
//   4. HintReset starts every coordinate over at the pixel's own place.
//   5. Strength 0 leaves the picture exactly as it was.
//
// Build: cl /nologo /std:c++20 /EHsc /W3 tests\nr_texture_hint_shader_smoke.cpp /link d3d11.lib d3dcompiler.lib
// Run:   nr_texture_hint_shader_smoke.exe OptiScaler\shaders\dlssnr\precompile\dlssnr.hlsl
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
#include <stdexcept>
#include <string>
#include <vector>
#include "../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
using Microsoft::WRL::ComPtr;

constexpr int W = 64, H = 40;
constexpr float kWrap = 4096.0f;
constexpr float kSeedX = 1013.5f, kSeedY = 2711.25f;

struct Px { float r, g, b, a; };
using Image = std::vector<Px>;

static void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D call failed"); }
static int fails = 0;
static void expect(bool ok, const std::string& label) { if (!ok) { std::printf("FAIL: %s\n", label.c_str()); ++fails; } }

// The picture with its content moved `shift` px to the right: a fill, a dark outline at x = 20 + shift, a textured strip
// from x = 44 + shift, and below y = 30 a smooth gradient (0.4% a pixel: skin, a blurred background) instead of the fill
static Image Picture(int shift)
{
    Image img((size_t) W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const int u = x - shift;
            Px p { 0.40f, 0.30f, 0.50f, 1.0f };
            if (y >= 30 && u < 44)
            {
                const float g = 0.004f * (float) u;
                p = { 0.30f + g, 0.20f + g, 0.40f + g, 1.0f };
            }
            if (u == 20)
                p = { 0.05f, 0.04f, 0.06f, 1.0f };
            else if (u >= 44)
            {
                const float t = ((u * 7 + y * 13) % 5) / 4.0f; // a texture of its own, spread well over the flat test
                p = { 0.2f + 0.4f * t, 0.15f + 0.4f * t, 0.3f + 0.4f * t, 1.0f };
            }
            img[(size_t) y * W + x] = p;
        }
    return img;
}

struct Gpu
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11ComputeShader> shader;

    explicit Gpu(const wchar_t* path)
    {
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                nullptr, &ctx));
        D3D11_BUFFER_DESC buffer {};
        buffer.ByteWidth = sizeof(DlssNrConstants);
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&buffer, nullptr, &constants));
        D3D11_SAMPLER_DESC sampling {};
        sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampling.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampling, &sampler));

        ComPtr<ID3DBlob> code, errors;
        // WARP crashes on the percentile meter's branch; this test does not use the meter.
        const D3D_SHADER_MACRO noMeter[] = { { "DLSSNR_NO_PERCENTILE_METER", "1" }, { nullptr, nullptr } };
        HRESULT hr = D3DCompileFromFile(path, noMeter, D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSMain", "cs_5_0",
                                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (errors && FAILED(hr))
            std::fprintf(stderr, "%s", (char*) errors->GetBufferPointer());
        check(hr);
        check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
    }

    ComPtr<ID3D11Texture2D> Texture(DXGI_FORMAT format, const void* data, UINT pitch)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = W;
        desc.Height = H;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        D3D11_SUBRESOURCE_DATA init { data, pitch, 0 };
        ComPtr<ID3D11Texture2D> texture;
        check(device->CreateTexture2D(&desc, data ? &init : nullptr, &texture));
        return texture;
    }

    template <class T> std::vector<T> Read(ID3D11Texture2D* texture)
    {
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&desc, nullptr, &staging));
        ctx->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        check(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<T> out((size_t) W * H);
        for (int y = 0; y < H; ++y)
            std::memcpy(&out[(size_t) y * W], (const uint8_t*) mapped.pData + (size_t) y * mapped.RowPitch, sizeof(T) * W);
        ctx->Unmap(staging.Get(), 0);
        return out;
    }

    // t0 picture, t1 last frame's coordinates, t3 motion; u0 the hinted picture, u1 this frame's coordinates
    void Run(const DlssNrConstants& settings, ID3D11Texture2D* picture, ID3D11Texture2D* before, ID3D11Texture2D* motion,
             ID3D11Texture2D* target, ID3D11Texture2D* now)
    {
        ComPtr<ID3D11ShaderResourceView> s0, s1, s3;
        check(device->CreateShaderResourceView(picture, nullptr, &s0));
        check(device->CreateShaderResourceView(before, nullptr, &s1));
        check(device->CreateShaderResourceView(motion, nullptr, &s3));
        ComPtr<ID3D11UnorderedAccessView> v0, v1;
        check(device->CreateUnorderedAccessView(target, nullptr, &v0));
        check(device->CreateUnorderedAccessView(now, nullptr, &v1));
        ID3D11ShaderResourceView* srvs[] = { s0.Get(), s1.Get(), s0.Get(), s3.Get(), s0.Get() };
        ID3D11UnorderedAccessView* uavs[] = { v0.Get(), v1.Get() };
        ctx->UpdateSubresource(constants.Get(), 0, nullptr, &settings, 0, 0);
        ctx->CSSetShader(shader.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 5, srvs);
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
        ctx->CSSetSamplers(0, 1, sampler.GetAddressOf());
        ctx->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
        ID3D11ShaderResourceView* noSrvs[5] = {};
        ID3D11UnorderedAccessView* noUavs[2] = {};
        ctx->CSSetShaderResources(0, 5, noSrvs);
        ctx->CSSetUnorderedAccessViews(0, 2, noUavs, nullptr);
    }
};

struct Coord { float x, y; };

static DlssNrConstants Settings(float strength, bool reset)
{
    DlssNrConstants c {};
    c.Mode = DlssNrMode_TextureHint;
    c.Width = W;
    c.Height = H;
    c.Passthrough = 1;
    c.TransferStrength = strength;
    c.MvScaleX = c.MvScaleY = 1.0f;
    c.GuideWidth = W;
    c.GuideHeight = H;
    c.HintReset = reset ? 1u : 0u;
    return c;
}

static float WrapDiff(float d) { return d - kWrap * std::round(d / kWrap); }

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: nr_texture_hint_shader_smoke <dlssnr.hlsl>\n");
        return 2;
    }

    std::wstring path(argv[1], argv[1] + std::strlen(argv[1]));
    Gpu gpu(path.c_str());

    const Image frame0 = Picture(0);
    const Image frame1 = Picture(3);
    std::vector<Coord> still((size_t) W * H, Coord { 0.0f, 0.0f });
    std::vector<Coord> moved((size_t) W * H, Coord { -3.0f, 0.0f }); // current -> previous: from 3 px to the left
    std::vector<Coord> half((size_t) W * H, Coord { -0.5f, 0.0f });

    auto pic0 = gpu.Texture(DXGI_FORMAT_R32G32B32A32_FLOAT, frame0.data(), W * sizeof(Px));
    auto pic1 = gpu.Texture(DXGI_FORMAT_R32G32B32A32_FLOAT, frame1.data(), W * sizeof(Px));
    auto motionStill = gpu.Texture(DXGI_FORMAT_R32G32_FLOAT, still.data(), W * sizeof(Coord));
    auto motionMoved = gpu.Texture(DXGI_FORMAT_R32G32_FLOAT, moved.data(), W * sizeof(Coord));
    auto motionHalf = gpu.Texture(DXGI_FORMAT_R32G32_FLOAT, half.data(), W * sizeof(Coord));
    auto coordA = gpu.Texture(DXGI_FORMAT_R32G32_FLOAT, nullptr, 0);
    auto coordB = gpu.Texture(DXGI_FORMAT_R32G32_FLOAT, nullptr, 0);
    auto coordC = gpu.Texture(DXGI_FORMAT_R32G32_FLOAT, nullptr, 0);
    auto out0 = gpu.Texture(DXGI_FORMAT_R32G32B32A32_FLOAT, nullptr, 0);
    auto out1 = gpu.Texture(DXGI_FORMAT_R32G32B32A32_FLOAT, nullptr, 0);

    // 1. First frame (reset): coordinates are the pixel's own place; the pattern only on the flat fill
    gpu.Run(Settings(1.0f, true), pic0.Get(), coordC.Get(), motionStill.Get(), out0.Get(), coordA.Get());
    const auto hinted0 = gpu.Read<Px>(out0.Get());
    const auto coords0 = gpu.Read<Coord>(coordA.Get());

    bool resetPlaces = true, flatChanged = false, flatBounded = true, textureKept = true, outlineKept = true;
    bool gradientKept = true;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const size_t i = (size_t) y * W + x;
            resetPlaces &= coords0[i].x == x + kSeedX && coords0[i].y == y + kSeedY;
            const float ratio = hinted0[i].g / frame0[i].g;
            const bool flat = std::abs(x - 20) > 2 && x < 42 && y < 26;
            if (flat)
            {
                flatChanged |= std::abs(ratio - 1.0f) > 0.01f;
                flatBounded &= std::abs(ratio - 1.0f) <= 0.07f * (1.0f / 0.3265f) + 1e-4f;
                // one ratio on the whole colour: the hue stays
                flatBounded &= std::abs(hinted0[i].r / frame0[i].r - ratio) < 1e-4f;
            }
            if (x >= 44)
                textureKept &= hinted0[i].r == frame0[i].r && hinted0[i].g == frame0[i].g && hinted0[i].b == frame0[i].b;
            if (std::abs(x - 20) <= 2)
                outlineKept &= hinted0[i].g == frame0[i].g;
            if (y >= 34 && x < 40 && hinted0[i].g != frame0[i].g)
            {
                if (gradientKept)
                    std::printf("gradient changed at x=%d y=%d: %.6f -> %.6f\n", x, y, frame0[i].g, hinted0[i].g);
                gradientKept = false;
            }
        }
    expect(resetPlaces, "reset: every coordinate is the pixel's own place");
    expect(flatChanged, "the flat fill gets the pattern");
    expect(flatBounded, "the pattern stays within its amplitude and keeps the hue");
    expect(textureKept, "the textured strip is left as it was");
    expect(outlineKept, "the outline and two pixels around it are left as they were");
    expect(gradientKept, "a smooth gradient is left as it was");

    // 2. The picture moves 3 px right: coordinates and the hinted fill move with it
    gpu.Run(Settings(1.0f, false), pic1.Get(), coordA.Get(), motionMoved.Get(), out1.Get(), coordB.Get());
    const auto hinted1 = gpu.Read<Px>(out1.Get());
    const auto coords1 = gpu.Read<Coord>(coordB.Get());

    bool carried = true, sticks = true, edgeRestarts = true;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const size_t i = (size_t) y * W + x;
            if (x >= 3)
            {
                const size_t j = i - 3;
                carried &= std::abs(WrapDiff(coords1[i].x - coords0[j].x)) < 1e-3f &&
                           std::abs(WrapDiff(coords1[i].y - coords0[j].y)) < 1e-3f;
                const bool flat = std::abs(x - 23) > 2 && x >= 5 && x < 45;
                if (flat)
                    sticks &= std::abs(hinted1[i].g - hinted0[j].g) < 1e-5f;
            }
            else
                edgeRestarts &= coords1[i].x == x + kSeedX && coords1[i].y == y + kSeedY;
        }
    expect(carried, "each coordinate is last frame's from 3 px to the left");
    expect(sticks, "the hinted fill is last frame's moved by 3 px");
    expect(edgeRestarts, "a vector out of the picture starts over at the pixel's own place");

    // 3. Half a pixel: the half is carried too
    gpu.Run(Settings(1.0f, false), pic0.Get(), coordA.Get(), motionHalf.Get(), out1.Get(), coordC.Get());
    const auto coordsHalf = gpu.Read<Coord>(coordC.Get());
    bool halfCarried = true;
    for (int y = 0; y < H; ++y)
        for (int x = 1; x < W; ++x)
        {
            const size_t i = (size_t) y * W + x;
            // round(x - 0.5) is the pixel at x - 0.5 rounded half away from zero (x), so the coordinate is last frame's
            // at that pixel minus a half
            const int nearPx = (int) std::lround((float) x - 0.5f);
            const size_t j = (size_t) y * W + nearPx;
            halfCarried &= std::abs(WrapDiff(coordsHalf[i].x - (coords0[j].x + ((float) x - 0.5f - nearPx)))) < 1e-3f &&
                           std::abs(WrapDiff(coordsHalf[i].y - coords0[j].y)) < 1e-3f;
        }
    expect(halfCarried, "half a pixel of motion moves the coordinates by half a pixel");

    // 4. A reset starts them over, whatever came before
    gpu.Run(Settings(1.0f, true), pic1.Get(), coordB.Get(), motionMoved.Get(), out1.Get(), coordC.Get());
    const auto coordsReset = gpu.Read<Coord>(coordC.Get());
    bool resetAgain = true;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            resetAgain &= coordsReset[(size_t) y * W + x].x == x + kSeedX;
    expect(resetAgain, "HintReset starts every coordinate over");

    // 5. Strength 0: the picture as it was
    gpu.Run(Settings(0.0f, true), pic0.Get(), coordB.Get(), motionStill.Get(), out1.Get(), coordC.Get());
    const auto none = gpu.Read<Px>(out1.Get());
    bool unchanged = true;
    for (size_t i = 0; i < none.size(); ++i)
        unchanged &= none[i].r == frame0[i].r && none[i].g == frame0[i].g && none[i].b == frame0[i].b;
    expect(unchanged, "strength 0 leaves the picture as it was");

    std::printf(fails == 0 ? "nr_texture_hint_shader_smoke: PASS\n" : "nr_texture_hint_shader_smoke: %d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
