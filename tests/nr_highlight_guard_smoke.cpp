// Headless shader test: Windows D3D11 WARP executes DlssNrMode_Resolve (the real composition path,
// not a synthetic stand-in) from the shared HLSL, driving the Composed Highlight guard through
// grey (zero-chroma) test pixels. This is the plan's step-2 falsifiability gate, run in the order
// the plan calls for: written and executed against the UNMODIFIED shader first, where the
// darkening case below must FAIL (proving the current symmetric clamp is still live), then
// re-executed at step 8 against the edited shader, where it must PASS.
//
// Grey pixels let this test sidestep implementing OkLab in C++: HueOkLab(incorrect, correct) takes
// its hue/chroma direction from `correct` and its chroma magnitude from `incorrect`
// (dlssnr.hlsl:224-256), and for a zero-chroma `correct` (an achromatic model answer) that
// direction is forced to (0,0), so the call is a numerical identity -- ToOkLab/FromOkLab round-trip
// a grey value back to itself. Case A below (the guard inactive, amplified == 1) empirically checks
// that assumption against the real shader before the darkening/brightening cases lean on it: if it
// is wrong, Case A fails first and loudly, rather than silently invalidating the others.
//
// Build: cl /nologo /std:c++20 /EHsc /W3 nr_highlight_guard_smoke.cpp /Fe:nr_highlight_guard_smoke.exe
//        /link d3d11.lib d3dcompiler.lib
// Run:   nr_highlight_guard_smoke.exe <path to dlssnr.hlsl>
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include "../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
using Microsoft::WRL::ComPtr;
struct Pixel { float r, g, b, a; };
static void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D call failed"); }
static void expect(bool ok, const char* label) { if (!ok) throw std::runtime_error(label); }
static bool nearv(float a, float b, float e) { return std::abs(a - b) < e; }

// Mirrors dlssnr.hlsl's own dual-floor constant (kRatioFloor, file scope) -- power-of-two, exact
// in float.
constexpr float kRatioFloor = 1.0f / 512.0f;

// Mirrors the resolve's ratio branch exactly (dlssnr.hlsl:1411-1424). Every case below sets
// proxyLuma == originalLuma (P == O), which takes the `else` branch and collapses it to 1.0
// regardless of O -- deliberately, so this test does not also have to carry the `originalLuma <
// proxyLuma` branch's arithmetic to make its point about the guard.
static float Ratio(float O, float P, float M) {
    return O < P ? O / std::max(P, 1e-6f) : (M + std::max(0.0f, O - P)) / M;
}

// Mirrors lumaRatio at TransferStrength == 1.0, where amplified == lumaRatio exactly
// (pow(x, 1.0) == x, dlssnr.hlsl:1454/1467), for grey inputs where upgraded-before-the-guard is
// exactly `model * ratio` (see the file header comment on HueOkLab as identity).
static float LumaRatio(float O, float P, float M) {
    const float upgradedLuma = M * Ratio(O, P, M);
    return (upgradedLuma + kRatioFloor) / (O + kRatioFloor);
}

static float BoundedRatioOld(float amplified, float guard) { return std::clamp(amplified, 1.0f / guard, guard); }
static float BoundedRatioNew(float amplified, float guard) { return std::min(amplified, guard); }

// Predicts the resolve's grey output at ColourStrength == 1.0 (result == upgraded exactly, the
// lerp's far endpoint, dlssnr.hlsl:1502) and Passthrough == 1 (normScale == 1, no WhitePoint()/
// exposure texture involved, dlssnr.hlsl:1275-1278).
static float Predicted(float O, float P, float M, float guard, bool newBehaviour) {
    const float ratio = Ratio(O, P, M);
    const float lumaRatio = LumaRatio(O, P, M);
    const float bounded = newBehaviour ? BoundedRatioNew(lumaRatio, guard) : BoundedRatioOld(lumaRatio, guard);
    return (M * ratio) * (bounded / std::max(lumaRatio, 1e-6f));
}

int wmain(int argc, wchar_t** argv) try {
    if (argc != 2) throw std::runtime_error("Pass the dlssnr.hlsl path");
    ComPtr<ID3DBlob> code, errors;
    HRESULT compiled = D3DCompileFromFile(argv[1], nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSMain", "cs_5_0",
                                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors) std::fprintf(stderr, "%s", (char*) errors->GetBufferPointer());
    check(compiled);
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> ctx;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx));
    ComPtr<ID3D11ComputeShader> shader;
    check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));

    constexpr float guard = 2.0f; // matches DlssNrMaxRatio's default (Config.h)
    // Case A: identity -- O == P == M, ratio == 1, amplified == 1, guard never engages. Empirically
    // validates the HueOkLab-is-identity-for-grey assumption the other two cases lean on.
    // Case B: brightening past the ceiling -- P == O (ratio == 1), M >> O. Old (clamp) and new (min)
    // agree: both cap at `guard`. Confirms the port does not change the brightening side at all.
    // Case C: THE discriminator. P == O (ratio == 1), M << O -- a genuine darkening past 1/guard.
    // Old floors it at `1/guard`; new leaves it at the model's own (unbounded) answer.
    constexpr int N = 3;
    const std::array<Pixel, N> proxy {{ {0.5f, 0.5f, 0.5f, 1}, {0.1f, 0.1f, 0.1f, 1}, {0.5f, 0.5f, 0.5f, 1} }};
    const std::array<Pixel, N> original {{ {0.5f, 0.5f, 0.5f, 1}, {0.1f, 0.1f, 0.1f, 1}, {0.5f, 0.5f, 0.5f, 1} }};
    const std::array<Pixel, N> model {{ {0.5f, 0.5f, 0.5f, 1}, {0.5f, 0.5f, 0.5f, 1}, {0.05f, 0.05f, 0.05f, 1} }};
    const char* names[N] = {"A identity", "B brightening (capped both ways)", "C darkening (the discriminator)"};

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = N; desc.Height = 1; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> proxyTex, modelTex, originalTex, target, keep, readback;
    D3D11_SUBRESOURCE_DATA data {proxy.data(), sizeof(proxy), 0};
    check(device->CreateTexture2D(&desc, &data, &proxyTex));
    data.pSysMem = model.data(); check(device->CreateTexture2D(&desc, &data, &modelTex));
    data.pSysMem = original.data(); check(device->CreateTexture2D(&desc, &data, &originalTex));
    ComPtr<ID3D11ShaderResourceView> proxySrv, modelSrv, originalSrv;
    check(device->CreateShaderResourceView(proxyTex.Get(), nullptr, &proxySrv));
    check(device->CreateShaderResourceView(modelTex.Get(), nullptr, &modelSrv));
    check(device->CreateShaderResourceView(originalTex.Get(), nullptr, &originalSrv));
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    check(device->CreateTexture2D(&desc, nullptr, &target)); check(device->CreateTexture2D(&desc, nullptr, &keep));
    ComPtr<ID3D11UnorderedAccessView> targetUav, keepUav;
    check(device->CreateUnorderedAccessView(target.Get(), nullptr, &targetUav));
    check(device->CreateUnorderedAccessView(keep.Get(), nullptr, &keepUav));
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(device->CreateTexture2D(&desc, nullptr, &readback));
    D3D11_BUFFER_DESC buffer {}; buffer.ByteWidth = sizeof(DlssNrConstants); buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constants; check(device->CreateBuffer(&buffer, nullptr, &constants));
    D3D11_SAMPLER_DESC sampling {}; sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sampling.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler; check(device->CreateSamplerState(&sampling, &sampler));

    // t0 proxy (gSource), t1 model (gModel), t2 original (gOriginal); t3/t4 (gMotion/gExposure) are
    // not read on this path (Mode 1, Passthrough 1, no onDivider) -- filled defensively rather than
    // left null, matching the sibling smoke tests' own convention.
    ID3D11ShaderResourceView* srvs[] = {proxySrv.Get(), modelSrv.Get(), originalSrv.Get(), originalSrv.Get(), originalSrv.Get()};
    ID3D11UnorderedAccessView* uavs[] = {targetUav.Get(), keepUav.Get()};
    ctx->CSSetShader(shader.Get(), nullptr, 0); ctx->CSSetShaderResources(0, 5, srvs);
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr); ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
    ctx->CSSetSamplers(0, 1, sampler.GetAddressOf());

    DlssNrConstants settings {};
    settings.Mode = DlssNrMode_Resolve;
    settings.Width = N; settings.Height = 1;
    settings.TransferStrength = 1.0f; settings.ColourStrength = 1.0f;
    settings.MaxRatio = guard;
    settings.Passthrough = 1;    // skips WhitePoint()/gExposure entirely -- normScale == 1
    settings.ApplyModel = 1;     // else the resolve returns the clean frame and nothing here runs
    settings.DebugView = 0;
    settings.CompareMode = 0;    // showOriginal/outsideFrame/onDivider all stay false
    settings.Transfer = 0;       // classic, skips the matched-residual rebuild entirely
    settings.ReversibleMode = 0; // Composed, not Replace -- ApplyReplaceGuard is untouched by this test
    settings.SkinProtection = 0;
    ctx->UpdateSubresource(constants.Get(), 0, nullptr, &settings, 0, 0);
    ctx->Dispatch(1, 1, 1);
    ctx->CopyResource(readback.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE mapped {};
    check(ctx->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    std::array<Pixel, N> out; memcpy(out.data(), mapped.pData, sizeof(out)); ctx->Unmap(readback.Get(), 0);

    std::puts("case                                O     P     M    -> actual (r g b)          predicted (new)");
    for (int i = 0; i < N; ++i) {
        const float pred = Predicted(original[i].r, proxy[i].r, model[i].r, guard, /*newBehaviour=*/true);
        std::printf("%-34s %.2f  %.2f  %.2f    %.4f %.4f %.4f      %.4f\n", names[i],
                    original[i].r, proxy[i].r, model[i].r, out[i].r, out[i].g, out[i].b, pred);
    }

    // AC (Case A): the guard is inactive here (amplified == 1), so this also validates the
    // HueOkLab-identity-for-grey assumption Cases B/C lean on, against the real shader, before
    // trusting it further.
    expect(nearv(out[0].r, 0.5f, 1e-3f) && nearv(out[0].g, 0.5f, 1e-3f) && nearv(out[0].b, 0.5f, 1e-3f),
           "Case A (guard inactive) did not reproduce the grey input -- HueOkLab-identity assumption "
           "does not hold, or the resolve path setup is wrong; the other two cases cannot be trusted "
           "until this one passes");
    expect(nearv(out[0].r, out[0].g, 1e-5f) && nearv(out[0].g, out[0].b, 1e-5f), "Case A output is not grey");

    // AC: brightening past the guard is unchanged by this port -- both formulas cap at `guard`.
    const float predictedB = Predicted(original[1].r, proxy[1].r, model[1].r, guard, true);
    expect(nearv(out[1].r, predictedB, 5e-3f) && nearv(out[1].g, predictedB, 5e-3f) && nearv(out[1].b, predictedB, 5e-3f),
           "Case B (brightening) does not match the predicted guard-capped value");

    // AC: the discriminator. Predicted-new (unbounded darkening, == the model's own answer since
    // ratio == 1 here) is far from predicted-old (floored at 1/guard) -- 0.05 vs ~0.24 at these
    // inputs, a 5x gap no float-precision slop can bridge. Run against the UNMODIFIED shader this
    // must FAIL (the real output will land near predicted-old instead); run again at step 8 against
    // the edited shader it must PASS.
    const float predictedC = Predicted(original[2].r, proxy[2].r, model[2].r, guard, true);
    expect(nearv(out[2].r, predictedC, 5e-3f) && nearv(out[2].g, predictedC, 5e-3f) && nearv(out[2].b, predictedC, 5e-3f),
           "Case C (darkening) does not match the unbounded prediction -- still floored at 1/guard "
           "(expected before the port; the falsifiability gate)");

    std::puts("PASS: Composed's guard bounds brightening only; darkening runs model-driven (WARP HLSL)");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
