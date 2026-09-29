// Headless shader test: Windows D3D11 WARP executes DlssNrMode_ClampProxy from the shared HLSL.
// Proves the interpass clamp is real, in range, direction-preserving, and identity for valid input.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include "../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
using Microsoft::WRL::ComPtr;
struct Pixel { float r, g, b, a; };
static void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D call failed"); }
static void expect(bool ok, const char* label) { if (!ok) throw std::runtime_error(label); }
static bool near3(Pixel a, Pixel b, float e = 1e-5f) {
    return std::abs(a.r - b.r) < e && std::abs(a.g - b.g) < e && std::abs(a.b - b.b) < e;
}
static bool inRange(Pixel p) {
    auto ok = [](float v) { return std::isfinite(v) && v >= 0.0f && v <= 1.0f; };
    return ok(p.r) && ok(p.g) && ok(p.b);
}
// The same maths as CubeScaleResidual, on the CPU, as an independent reference.
static Pixel reference(Pixel P, Pixel T) {
    float p[3] = {P.r, P.g, P.b}, t[3] = {T.r, T.g, T.b}, alpha = 1.0f;
    for (int c = 0; c < 3; ++c) {
        float d = t[c] - p[c];
        if (d > 1e-6f) alpha = std::min(alpha, (1.0f - p[c]) / d);
        else if (d < -1e-6f) alpha = std::min(alpha, (0.0f - p[c]) / d);
    }
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    return {p[0] + alpha * (t[0] - p[0]), p[1] + alpha * (t[1] - p[1]), p[2] + alpha * (t[2] - p[2]), T.a};
}
int wmain(int argc, wchar_t** argv) try {
    if (argc != 2) throw std::runtime_error("Pass the dlssnr.hlsl path");
    ComPtr<ID3DBlob> code, errors;
    HRESULT compiled = D3DCompileFromFile(argv[1], nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSMain", "cs_5_0",
                                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors) std::fprintf(stderr, "%s", (char*)errors->GetBufferPointer());
    check(compiled);
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> ctx;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx));
    ComPtr<ID3D11ComputeShader> shader;
    check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));

    const float nan = std::numeric_limits<float>::quiet_NaN();
    // proxy (what the pass was fed, always in [0,1]) and raw (what the model returned, may ring).
    constexpr int N = 8;
    const std::array<Pixel, N> proxy {{
        {0.5f, 0.5f, 0.5f, 1}, {0.5f, 0.5f, 0.5f, 1}, {0.5f, 0.5f, 0.5f, 1}, {0.2f, 0.4f, 0.8f, 1},
        {0.9f, 0.1f, 0.5f, 1}, {0.5f, 0.5f, 0.5f, 1}, {0.3f, 0.3f, 0.3f, 1}, {0.6f, 0.2f, 0.4f, 0.25f}}};
    const std::array<Pixel, N> raw {{
        {0.55f, 0.45f, 0.60f, 1},   // 0: already valid -> must pass through untouched
        {1.40f, 0.60f, 0.40f, 1},   // 1: one channel above 1
        {-0.30f, 0.55f, 0.45f, 1},  // 2: one channel below 0
        {1.30f, -0.20f, 1.10f, 1},  // 3: several channels out on both sides
        {2.00f, 0.30f, 0.80f, 1},   // 4: proxy already near the wall, big overshoot
        {nan, 0.5f, 0.5f, 1},       // 5: NaN from the model
        {INFINITY, 0.3f, 0.3f, 1},  // 6: Inf from the model
        {1.50f, 0.10f, 0.90f, 0.25f}}}; // 7: overshoot with a non-1 alpha to carry through

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = N; desc.Height = 1; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> rawTex, proxyTex, output, keep, readback;
    D3D11_SUBRESOURCE_DATA data {raw.data(), sizeof(raw), 0};
    check(device->CreateTexture2D(&desc, &data, &rawTex));
    data.pSysMem = proxy.data(); check(device->CreateTexture2D(&desc, &data, &proxyTex));
    ComPtr<ID3D11ShaderResourceView> rawSrv, proxySrv;
    check(device->CreateShaderResourceView(rawTex.Get(), nullptr, &rawSrv));
    check(device->CreateShaderResourceView(proxyTex.Get(), nullptr, &proxySrv));
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    check(device->CreateTexture2D(&desc, nullptr, &output)); check(device->CreateTexture2D(&desc, nullptr, &keep));
    ComPtr<ID3D11UnorderedAccessView> outputUav, keepUav;
    check(device->CreateUnorderedAccessView(output.Get(), nullptr, &outputUav));
    check(device->CreateUnorderedAccessView(keep.Get(), nullptr, &keepUav));
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(device->CreateTexture2D(&desc, nullptr, &readback));
    D3D11_BUFFER_DESC buffer {}; buffer.ByteWidth = sizeof(DlssNrConstants); buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constants; check(device->CreateBuffer(&buffer, nullptr, &constants));
    D3D11_SAMPLER_DESC sampling {}; sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sampling.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler; check(device->CreateSamplerState(&sampling, &sampler));

    // t0 = gSource (the pass's raw answer), t1 = gModel (the pass's own proxy), as DispatchPass binds it.
    ID3D11ShaderResourceView* srvs[] = {rawSrv.Get(), proxySrv.Get(), proxySrv.Get(), proxySrv.Get(), proxySrv.Get()};
    ID3D11UnorderedAccessView* uavs[] = {outputUav.Get(), keepUav.Get()};
    ctx->CSSetShader(shader.Get(), nullptr, 0); ctx->CSSetShaderResources(0, 5, srvs);
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr); ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
    ctx->CSSetSamplers(0, 1, sampler.GetAddressOf());
    DlssNrConstants settings {}; settings.Mode = DlssNrMode_ClampProxy; settings.Width = N; settings.Height = 1;
    settings.WhitePoint = 1; settings.Passthrough = 0;
    ctx->UpdateSubresource(constants.Get(), 0, nullptr, &settings, 0, 0); ctx->Dispatch(1, 1, 1);
    ctx->CopyResource(readback.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped {};
    check(ctx->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    std::array<Pixel, N> out; memcpy(out.data(), mapped.pData, sizeof(out)); ctx->Unmap(readback.Get(), 0);

    std::puts("idx  proxy                  raw                     clamped                 naive saturate(raw)");
    for (int i = 0; i < N; ++i) {
        Pixel s {std::isfinite(raw[i].r) ? std::clamp(raw[i].r, 0.f, 1.f) : 0.f, std::isfinite(raw[i].g) ? std::clamp(raw[i].g, 0.f, 1.f) : 0.f,
                 std::isfinite(raw[i].b) ? std::clamp(raw[i].b, 0.f, 1.f) : 0.f, 1};
        std::printf("%d    %.2f %.2f %.2f      %5.2f %5.2f %5.2f      %.4f %.4f %.4f      %.2f %.2f %.2f\n", i,
                    proxy[i].r, proxy[i].g, proxy[i].b, raw[i].r, raw[i].g, raw[i].b, out[i].r, out[i].g, out[i].b, s.r, s.g, s.b);
    }

    for (int i = 0; i < N; ++i) expect(inRange(out[i]), "Output left [0,1] or is non-finite");
    expect(near3(out[0], raw[0]), "Valid raw output was modified");
    for (int i : {1, 2, 3, 4}) expect(near3(out[i], reference(proxy[i], raw[i])), "Output differs from the CPU CubeScaleResidual reference");
    expect(std::abs(out[7].a - 0.25f) < 1e-6f, "Alpha not carried through");

    // Direction preserved: (out - proxy) must be parallel to (raw - proxy) and shrunk, never flipped.
    for (int i : {1, 2, 3, 4}) {
        float d[3] = {raw[i].r - proxy[i].r, raw[i].g - proxy[i].g, raw[i].b - proxy[i].b};
        float e[3] = {out[i].r - proxy[i].r, out[i].g - proxy[i].g, out[i].b - proxy[i].b};
        float dn = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]), en = std::sqrt(e[0]*e[0] + e[1]*e[1] + e[2]*e[2]);
        float cosine = (d[0]*e[0] + d[1]*e[1] + d[2]*e[2]) / (dn * en);
        expect(cosine > 0.99999f, "Clamp bent the edit direction (hue shift)");
        expect(en <= dn + 1e-6f, "Clamp grew the edit");
    }
    // The claim that matters: a blind per-channel saturate on case 1 changes the edit's direction; this does not.
    {
        float d[3] = {raw[1].r - proxy[1].r, raw[1].g - proxy[1].g, raw[1].b - proxy[1].b};
        float s[3] = {1.0f - proxy[1].r, raw[1].g - proxy[1].g, raw[1].b - proxy[1].b};
        float cosine = (d[0]*s[0] + d[1]*s[1] + d[2]*s[2]) /
                       (std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]) * std::sqrt(s[0]*s[0] + s[1]*s[1] + s[2]*s[2]));
        std::printf("naive saturate on case 1 rotates the edit by %.2f degrees; the shipped clamp rotates it by 0\n",
                    std::acos(std::min(cosine, 1.0f)) * 57.29578f);
        expect(cosine < 0.999f, "Test case does not actually distinguish saturate from the hue-preserving clamp");
    }
    std::puts("PASS: ClampProxy is in range, identity on valid input, matches CubeScaleResidual, direction-preserving, NaN/Inf-safe (WARP HLSL)");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
