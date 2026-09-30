// Headless shader test: Windows D3D11 WARP executes dlssnr.hlsl's encode and resolve for the Colour encoding
// override ([DlssNr] ColourEncoding, DlssNr_ColourEncoding.h). No game and no NVIDIA model: the model's answer is
// the proxy itself, so a correct decode on the way in and encode on the way out must give the input back.
//
//   1. Auto is bit-identical to the reference shader (dlssnr.hlsl before the override, commit f08ee84c) for the
//      encode's proxy and kept copy and for the resolve -- tone-mapped and linear HDR, soft knee / Neutwo / hybrid,
//      at every shader-conversion value Auto can send (0; and 1, 2 must convert nothing either).
//   2. Gamma 2.2: the model is shown the sRGB encoding of the same light; the frame comes back unchanged, with the
//      model applied, with ApplyModel off, and with the game's own texture bound as the resolve's original.
//   3. PQ: the kept copy is linear BT.709 light with 1.0 = 203 nits (ITU-R BT.2408), decoded from BT.2020; the frame
//      comes back unchanged in the same three ways.
//   4. The game's own formats: 8-bit and 10-bit UNORM frames with the FP16 kept copy the D3D12 path allocates while
//      the shader converts come back within one code value (gamma 2.2 on both, PQ on 10-bit).
//
// Build: cl /nologo /std:c++20 /EHsc /W3 tests\nr_colour_encoding_shader_smoke.cpp /link d3d11.lib d3dcompiler.lib
// Reference: git show f08ee84c:OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl > dlssnr_ref.hlsl
// Run:   nr_colour_encoding_shader_smoke.exe OptiScaler\shaders\dlssnr\precompile\dlssnr.hlsl dlssnr_ref.hlsl
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include "../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
using Microsoft::WRL::ComPtr;

struct Pixel { float r, g, b, a; };
constexpr int N = 6;
using Row = std::array<Pixel, N>;

static void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D call failed"); }
static int fails = 0;
static void expect(bool ok, const std::string& label) { if (!ok) { std::printf("FAIL: %s\n", label.c_str()); ++fails; } }

static float SrgbEncode(float v) { v = std::max(v, 0.0f); return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f; }

// BT.709 -> BT.2020, the matrix dlssnr_pq.hlsli uses.
static void To2020(const float in[3], float out[3])
{
    const float m[9] = {0.6274039f, 0.3292830f, 0.0433131f, 0.0690973f, 0.9195404f, 0.0113623f, 0.0163914f, 0.0880133f, 0.8955953f};
    for (int r = 0; r < 3; ++r)
        out[r] = m[r * 3] * in[0] + m[r * 3 + 1] * in[1] + m[r * 3 + 2] * in[2];
}
static float PqEncodeNits(float nits)
{
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float y = std::pow(std::clamp(nits / 10000.0f, 0.0f, 1.0f), m1);
    return std::pow((c1 + c2 * y) / (1.0f + c3 * y), m2);
}
// An HDR10 pixel showing these BT.709 nits.
static Pixel Hdr10(float r709, float g709, float b709)
{
    const float in[3] = {r709, g709, b709};
    float n2020[3];
    To2020(in, n2020);
    return {PqEncodeNits(n2020[0]), PqEncodeNits(n2020[1]), PqEncodeNits(n2020[2]), 1};
}

// The UNORM formats a game hands NR, packed and unpacked on the host.
static uint32_t Pack(DXGI_FORMAT format, const Pixel& p)
{
    auto q = [](float v, float max) { return (uint32_t) std::lround(std::clamp(v, 0.0f, 1.0f) * max); };
    if (format == DXGI_FORMAT_R10G10B10A2_UNORM)
        return q(p.r, 1023) | q(p.g, 1023) << 10 | q(p.b, 1023) << 20 | q(p.a, 3) << 30;
    return q(p.r, 255) | q(p.g, 255) << 8 | q(p.b, 255) << 16 | q(p.a, 255) << 24;
}
static Pixel Unpack(DXGI_FORMAT format, uint32_t v)
{
    if (format == DXGI_FORMAT_R10G10B10A2_UNORM)
        return {(v & 1023) / 1023.0f, (v >> 10 & 1023) / 1023.0f, (v >> 20 & 1023) / 1023.0f, (v >> 30) / 3.0f};
    return {(v & 255) / 255.0f, (v >> 8 & 255) / 255.0f, (v >> 16 & 255) / 255.0f, (v >> 24) / 255.0f};
}

static float Half(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, mant = h & 1023;
    const float v = exp == 0 ? std::ldexp((float) mant, -24)
                  : exp == 31 ? INFINITY
                              : std::ldexp((float) (mant | 1024), (int) exp - 25);
    return sign ? -v : v;
}

struct Gpu
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;

    Gpu()
    {
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx));
        D3D11_BUFFER_DESC buffer {};
        buffer.ByteWidth = sizeof(DlssNrConstants);
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        check(device->CreateBuffer(&buffer, nullptr, &constants));
        D3D11_SAMPLER_DESC sampling {};
        sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
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

    // data is uploaded for R32G32B32A32_FLOAT and the two UNORM formats; FP16 textures start empty.
    ComPtr<ID3D11Texture2D> Texture(DXGI_FORMAT format, const Row* data, bool uav)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = N; desc.Height = 1; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = format; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
        std::array<uint32_t, N> packed {};
        D3D11_SUBRESOURCE_DATA init {};
        if (data && format == DXGI_FORMAT_R32G32B32A32_FLOAT)
            init = {data->data(), sizeof(Row), 0};
        else if (data)
        {
            if (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_R10G10B10A2_UNORM)
                throw std::runtime_error("no upload for this format in this test");
            for (int i = 0; i < N; ++i) packed[i] = Pack(format, (*data)[i]);
            init = {packed.data(), sizeof(packed), 0};
        }
        ComPtr<ID3D11Texture2D> texture;
        check(device->CreateTexture2D(&desc, data ? &init : nullptr, &texture));
        return texture;
    }

    Row Read(ID3D11Texture2D* texture)
    {
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        const DXGI_FORMAT format = desc.Format;
        desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&desc, nullptr, &staging));
        ctx->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        check(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        Row row {};
        if (format == DXGI_FORMAT_R32G32B32A32_FLOAT)
            std::memcpy(row.data(), mapped.pData, sizeof(Row));
        else if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
        {
            const uint16_t* h = (const uint16_t*) mapped.pData;
            for (int i = 0; i < N; ++i) row[i] = {Half(h[i * 4]), Half(h[i * 4 + 1]), Half(h[i * 4 + 2]), Half(h[i * 4 + 3])};
        }
        else
            for (int i = 0; i < N; ++i) row[i] = Unpack(format, ((const uint32_t*) mapped.pData)[i]);
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
        ctx->CSSetSamplers(0, 1, sampler.GetAddressOf());
        ctx->Dispatch(1, 1, 1);
        ID3D11ShaderResourceView* noSrvs[5] = {};
        ID3D11UnorderedAccessView* noUavs[2] = {};
        ctx->CSSetShaderResources(0, 5, noSrvs);
        ctx->CSSetUnorderedAccessViews(0, 2, noUavs, nullptr);
    }
};

struct Result { Row proxy, keep, frame; };

struct Case
{
    uint32_t passthrough = 0, reversible = 0, inputEncoding = 0, applyModel = 1;
    bool rawOriginal = false;                          // the game's texture as the resolve's original (no-UAV colour)
    DXGI_FORMAT game = DXGI_FORMAT_R32G32B32A32_FLOAT; // the game's frame and the resolve's target
    DXGI_FORMAT keep = DXGI_FORMAT_R32G32B32A32_FLOAT; // the kept copy
};

static DlssNrConstants Base(uint32_t mode, const Case& c)
{
    DlssNrConstants k {};
    k.Mode = mode;
    k.Width = N; k.Height = 1;
    k.WhitePoint = 1.0f;
    k.TransferStrength = 1.0f; k.ColourStrength = 1.0f;
    k.MaxRatio = 8.0f;
    k.Passthrough = c.passthrough;
    k.ApplyModel = c.applyModel;
    k.ReversibleMode = c.reversible;
    k.DebugScale = 1.0f;
    k.ModelWorkScale = 1.0f;
    k.PassFeedback = 1.0f;
    k.InputEncoding = c.inputEncoding;
    k.OriginalIsGameColour = c.rawOriginal ? 1u : 0u;
    return k;
}

// Encode, then resolve with the proxy standing in for the model's answer.
static Result EncodeResolve(Gpu& gpu, ID3D11ComputeShader* shader, const Row& input, const Case& c)
{
    auto source = gpu.Texture(c.game, &input, false);
    auto proxy = gpu.Texture(DXGI_FORMAT_R32G32B32A32_FLOAT, nullptr, true);
    auto keep = gpu.Texture(c.keep, nullptr, true);
    auto frame = gpu.Texture(c.game, nullptr, true);
    auto spare = gpu.Texture(DXGI_FORMAT_R32G32B32A32_FLOAT, nullptr, true);
    gpu.Run(shader, Base(DlssNrMode_Encode, c), source.Get(), nullptr, nullptr, proxy.Get(), keep.Get());
    gpu.Run(shader, Base(DlssNrMode_Resolve, c), proxy.Get(), proxy.Get(), c.rawOriginal ? source.Get() : keep.Get(),
            frame.Get(), spare.Get());
    return {gpu.Read(proxy.Get()), gpu.Read(keep.Get()), gpu.Read(frame.Get())};
}

static Case With(Case c, DXGI_FORMAT game, DXGI_FORMAT keep)
{
    c.game = game;
    c.keep = keep;
    return c;
}

static bool SameBits(const Row& a, const Row& b) { return std::memcmp(a.data(), b.data(), sizeof(Row)) == 0; }

static float WorstRelative(const Row& got, const Row& want)
{
    float worst = 0.0f;
    for (int i = 0; i < N; ++i)
    {
        const float g[3] = {got[i].r, got[i].g, got[i].b}, w[3] = {want[i].r, want[i].g, want[i].b};
        for (int c = 0; c < 3; ++c)
            worst = std::max(worst, std::abs(g[c] - w[c]) / std::max(std::abs(w[c]), 1e-3f));
    }
    return worst;
}

static float WorstCodes(const Row& got, const Row& want, float codes)
{
    float worst = 0.0f;
    for (int i = 0; i < N; ++i)
        worst = std::max({worst, std::abs(got[i].r - want[i].r) * codes, std::abs(got[i].g - want[i].g) * codes,
                          std::abs(got[i].b - want[i].b) * codes});
    return worst;
}

int wmain(int argc, wchar_t** argv) try
{
    if (argc != 3) throw std::runtime_error("Pass <dlssnr.hlsl> <reference dlssnr.hlsl from f08ee84c>");
    Gpu gpu;
    auto shader = gpu.Compile(argv[1]);
    auto reference = gpu.Compile(argv[2]);

    // 1. Auto is bit-identical to the reference shader, at every conversion value Auto can send.
    const Row toneMapped {{ {0.02f, 0.02f, 0.02f, 1}, {0.2f, 0.2f, 0.2f, 1}, {0.5f, 0.5f, 0.5f, 1},
                            {0.9f, 0.9f, 0.9f, 1}, {0.8f, 0.3f, 0.1f, 1}, {0.1f, 0.4f, 0.7f, 1} }};
    const Row linear {{ {0.01f, 0.01f, 0.01f, 1}, {0.18f, 0.18f, 0.18f, 1}, {1.0f, 1.0f, 1.0f, 1},
                        {4.0f, 4.0f, 4.0f, 1}, {2.0f, 0.5f, 0.1f, 1}, {0.05f, 0.3f, 1.5f, 1} }};
    for (uint32_t reversible : {0u, 1u, 3u})
        for (uint32_t passthrough : {0u, 1u})
            for (uint32_t autoValue : {0u, 1u, 2u})
            {
                const Row& input = passthrough ? toneMapped : linear;
                const auto a = EncodeResolve(gpu, shader.Get(), input, {passthrough, reversible, autoValue});
                const auto b = EncodeResolve(gpu, reference.Get(), input, {passthrough, reversible, 0});
                const std::string what = " (Auto sends " + std::to_string(autoValue) + ", passthrough " +
                                         std::to_string(passthrough) + ", curve " + std::to_string(reversible) + ")";
                expect(SameBits(a.proxy, b.proxy), "proxy differs from the reference shader" + what);
                expect(SameBits(a.keep, b.keep), "kept copy differs from the reference shader" + what);
                expect(SameBits(a.frame, b.frame), "resolved frame differs from the reference shader" + what);
            }

    // 2. Gamma 2.2: the model sees sRGB-encoded light, and the frame comes back.
    {
        const Row& input = toneMapped;
        Row wantProxy = input;
        for (auto& p : wantProxy)
        {
            p.r = SrgbEncode(std::pow(p.r, 2.2f));
            p.g = SrgbEncode(std::pow(p.g, 2.2f));
            p.b = SrgbEncode(std::pow(p.b, 2.2f));
        }
        const auto got = EncodeResolve(gpu, shader.Get(), input, {1, 0, 3});
        const float proxyError = WorstRelative(got.proxy, wantProxy);
        const float frameError = WorstRelative(got.frame, input);
        const float offError = WorstRelative(EncodeResolve(gpu, shader.Get(), input, {1, 0, 3, 0}).frame, input);
        const float rawError = WorstRelative(EncodeResolve(gpu, shader.Get(), input, {1, 0, 3, 1, true}).frame, input);
        std::printf("gamma 2.2: proxy %.2e, round trip %.2e, model off %.2e, game texture as original %.2e\n",
                    proxyError, frameError, offError, rawError);
        expect(proxyError < 1e-4f, "gamma 2.2: the model is not shown the sRGB encoding of the same light");
        expect(frameError < 1e-4f, "gamma 2.2: the frame does not come back unchanged");
        expect(offError < 1e-4f, "gamma 2.2: with the model off the frame does not come back unchanged");
        expect(rawError < 1e-4f, "gamma 2.2: wrong when the resolve reads the game's own texture as original");
    }

    // 3. PQ: linear BT.709 light with 1.0 = 203 nits, decoded from BT.2020, and the frame comes back.
    {
        const float nits709[N][3] = {{1, 1, 1}, {20, 20, 20}, {100, 100, 100}, {203, 203, 203}, {1000, 1000, 1000}, {600, 150, 40}};
        Row input {}, wantKeep {};
        for (int i = 0; i < N; ++i)
        {
            input[i] = Hdr10(nits709[i][0], nits709[i][1], nits709[i][2]);
            wantKeep[i] = {nits709[i][0] / 203.0f, nits709[i][1] / 203.0f, nits709[i][2] / 203.0f, 1};
        }
        const auto got = EncodeResolve(gpu, shader.Get(), input, {0, 0, 4});
        const float keepError = WorstRelative(got.keep, wantKeep);
        const float frameError = WorstRelative(got.frame, input);
        const float offError = WorstRelative(EncodeResolve(gpu, shader.Get(), input, {0, 0, 4, 0}).frame, input);
        const float rawError = WorstRelative(EncodeResolve(gpu, shader.Get(), input, {0, 0, 4, 1, true}).frame, input);
        std::printf("PQ: decode %.2e, round trip %.2e, model off %.2e, game texture as original %.2e\n", keepError,
                    frameError, offError, rawError);
        expect(keepError < 1e-3f, "PQ: the kept copy is not linear BT.709 light at 203 nits = 1.0");
        expect(frameError < 1e-4f, "PQ: the frame does not come back unchanged");
        expect(offError < 1e-4f, "PQ: with the model off the frame does not come back unchanged");
        expect(rawError < 1e-4f, "PQ: wrong when the resolve reads the game's own texture as original");
    }

    // 4. The game's own formats, with the FP16 kept copy the D3D12 path allocates while the shader converts.
    {
        const float codes[N] = {2, 5, 16, 100, 500, 1000};
        struct Format { DXGI_FORMAT format; float codes; const char* name; };
        for (const Format& f : {Format {DXGI_FORMAT_R8G8B8A8_UNORM, 255.0f, "8-bit"},
                                Format {DXGI_FORMAT_R10G10B10A2_UNORM, 1023.0f, "10-bit"}})
        {
            Row input {};
            for (int i = 0; i < N; ++i)
            {
                const float v = std::min(codes[i], f.codes) / f.codes;
                input[i] = Unpack(f.format, Pack(f.format, {v, v * 0.5f, v * 0.25f, 1})); // what the buffer holds
            }
            const Case gamma = With({1, 0, 3}, f.format, DXGI_FORMAT_R16G16B16A16_FLOAT);
            const float gammaCodes = WorstCodes(EncodeResolve(gpu, shader.Get(), input, gamma).frame, input, f.codes);
            std::printf("gamma 2.2 on %s: worst %.2f code values\n", f.name, gammaCodes);
            expect(gammaCodes <= 1.0f, std::string("gamma 2.2 on ") + f.name + ": more than one code value off");
        }
        Row hdr10 {};
        const float nits[N] = {0.05f, 1, 50, 203, 1000, 4000};
        for (int i = 0; i < N; ++i)
            hdr10[i] = Unpack(DXGI_FORMAT_R10G10B10A2_UNORM, Pack(DXGI_FORMAT_R10G10B10A2_UNORM, Hdr10(nits[i], nits[i], nits[i])));
        const Case pq = With({0, 0, 4}, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT);
        const float pqCodes = WorstCodes(EncodeResolve(gpu, shader.Get(), hdr10, pq).frame, hdr10, 1023.0f);
        std::printf("PQ on 10-bit: worst %.2f code values (1000 and 4000 nits included)\n", pqCodes);
        expect(pqCodes <= 1.0f, "PQ on 10-bit: more than one code value off (highlights clipped?)");

        // Sensitivity: a kept copy in the game's own 10-bit format -- what the D3D12 path used to allocate -- clips
        // the highlights, and this check must see it, or it could not have caught that.
        const Case oldKeep = With({0, 0, 4}, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM);
        const float oldCodes = WorstCodes(EncodeResolve(gpu, shader.Get(), hdr10, oldKeep).frame, hdr10, 1023.0f);
        std::printf("PQ on 10-bit with a 10-bit kept copy: worst %.2f code values (must be large)\n", oldCodes);
        expect(oldCodes > 10.0f, "PQ with a 10-bit kept copy should clip, and this test no longer notices");
    }

    if (fails)
    {
        std::printf("%d check(s) failed\n", fails);
        return 1;
    }
    std::puts("PASS: Auto unchanged; gamma 2.2 and PQ decode and round-trip, float and 8/10-bit (WARP HLSL)");
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
