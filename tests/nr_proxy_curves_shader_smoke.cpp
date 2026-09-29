// Headless shader test: Windows D3D11 WARP executes dlssnr.hlsl's encode, downsample and resolve for the proxy
// curves ([DlssNr] ReversibleMode). No game and no NVIDIA model: the model's answer is the proxy itself.
//
//   1. Modes 0-4 (soft knee, Neutwo and hybrid, composed and replace) are bit-identical to the reference shader
//      (dlssnr.hlsl before the ProxyCurve/IsReplace refactor, commit 7d518d74) for the encode's proxy and kept
//      copy, the downsample, and the resolve -- linear HDR and tone-mapped.
//   2. HLG (5) and PQ (6): the peak channel's proxy value is the standard's signal, with the 203-nit reference
//      white (ITU-R BT.2408) at 0.75 and 0.58; hue is kept (one scalar on the triple); the proxy rises with the
//      light from 0 to 64x white; the downsample's decode and re-encode give the proxy back; and the resolve gives
//      the frame back. Linear (7) the same, with white at 1.0.
//   3. The signal curves force opaque alpha, as the other reversible curves do.
//   4. Composed carries the model's edit whole on the signal curves: a model that answers 10% brighter lifts the
//      frame 10% (they compose in scene light). Modes 0-4 stay bit-identical to the reference with that model.
//   5. Matched residual (the model below the frame's size, transfer 1 and 2): modes 0-4 bit-identical to the
//      reference (transfer 2 to rounding, see 7); the signal curves rebuild the frame's own proxy exactly, so an unchanged answer gives the frame back.
//   6. SGSR1 (sgsr1.hlsl): modes 0-4 bit-identical to its reference; the signal curves are enlarged on the stored
//      value as it is, exactly like the soft knee.
//   7. The OkLab residual (transfer 2) keeps the shadows: an unchanged answer gives near-black back on every curve.
//
// Build: cl /nologo /std:c++20 /EHsc /W3 tests\nr_proxy_curves_shader_smoke.cpp /link d3d11.lib d3dcompiler.lib
// Reference: git show 7d518d74:OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl > precompile\dlssnr_ref.hlsl
//            (next to dlssnr_pq.hlsli, which it includes)
//            git show 7d518d74:OptiScaler/shaders/sgsr1/precompile/sgsr1.hlsl > sgsr1_ref.hlsl
// Run:   nr_proxy_curves_shader_smoke.exe <dlssnr.hlsl> <dlssnr reference> <sgsr1.hlsl> <sgsr1 reference>
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

struct Pixel { float r, g, b, a; };
using Row = std::vector<Pixel>;

static void check(HRESULT hr, int line = __builtin_LINE())
{
    if (FAILED(hr)) throw std::runtime_error("D3D call failed, line " + std::to_string(line));
}
static int fails = 0;
static void expect(bool ok, const std::string& label) { if (!ok) { std::printf("FAIL: %s\n", label.c_str()); ++fails; } }

// The standards, on the host.
static double HlgSignal(double e)
{
    const double a = 0.17883277, b = 0.28466892, c = 0.55991073;
    return e <= 1.0 / 12.0 ? std::sqrt(3.0 * e) : a * std::log(12.0 * e - b) + c;
}
static double PqSignal(double nits)
{
    const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0, c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    const double y = std::pow(std::clamp(nits / 10000.0, 0.0, 1.0), m1);
    return std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}
// HLG's scene light at the reference white: the E whose signal is 0.75.
static const double kHlgWhite = (std::exp((0.75 - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;

static double WantSignal(uint32_t mode, double n)
{
    if (mode == 5) return std::min(HlgSignal(n * kHlgWhite), 1.0);
    if (mode == 6) return PqSignal(n * 203.0);
    return std::clamp(n, 0.0, 1.0);
}

struct Gpu
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11SamplerState> linearSampler;

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
        sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
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
        ctx->CSSetSamplers(0, 1, sampler.GetAddressOf());
        ctx->Dispatch((settings.Width + 7) / 8, 1, 1); // 8x8 groups
        ID3D11ShaderResourceView* noSrvs[5] = {};
        ID3D11UnorderedAccessView* noUavs[2] = {};
        ctx->CSSetShaderResources(0, 5, noSrvs);
        ctx->CSSetUnorderedAccessViews(0, 2, noUavs, nullptr);
    }
};

static DlssNrConstants Base(uint32_t mode, UINT width, uint32_t passthrough, uint32_t reversible)
{
    DlssNrConstants k {};
    k.Mode = mode;
    k.Width = width; k.Height = 1;
    k.WhitePoint = 1.0f;
    k.TransferStrength = 1.0f; k.ColourStrength = 1.0f;
    k.MaxRatio = 8.0f;
    k.Passthrough = passthrough;
    k.ApplyModel = 1;
    k.ReversibleMode = reversible;
    k.DebugScale = 1.0f;
    k.ModelWorkScale = 1.0f;
    k.PassFeedback = 1.0f;
    return k;
}

struct Result { Row proxy, keep, half, frame; };

// Encode; downsample the proxy 2:1; resolve with the proxy standing in for the model's answer.
static Result Run(Gpu& gpu, ID3D11ComputeShader* shader, const Row& input, uint32_t passthrough, uint32_t reversible)
{
    const UINT n = (UINT) input.size();
    auto source = gpu.Texture(n, &input);
    auto proxy = gpu.Texture(n, nullptr);
    auto keep = gpu.Texture(n, nullptr);
    auto half = gpu.Texture(n / 2, nullptr);
    auto frame = gpu.Texture(n, nullptr);
    auto spare = gpu.Texture(n, nullptr);
    gpu.Run(shader, Base(DlssNrMode_Encode, n, passthrough, reversible), source.Get(), nullptr, nullptr, proxy.Get(), keep.Get());
    gpu.Run(shader, Base(DlssNrMode_Downsample, n / 2, passthrough, reversible), proxy.Get(), nullptr, nullptr, half.Get(), spare.Get());
    gpu.Run(shader, Base(DlssNrMode_Resolve, n, passthrough, reversible), proxy.Get(), proxy.Get(), keep.Get(), frame.Get(), spare.Get());
    return {gpu.Read(proxy.Get()), gpu.Read(keep.Get()), gpu.Read(half.Get()), gpu.Read(frame.Get())};
}

struct Settings { uint32_t passthrough = 0, reversible = 0, transfer = 0; float workScale = 1.0f; };

// The encode's proxy and kept copy, and the proxy's 2:1 downsample, left on the GPU.
struct Encoded { ComPtr<ID3D11Texture2D> proxy, keep, half; };

static Encoded Encode(Gpu& gpu, ID3D11ComputeShader* shader, const Row& input, uint32_t passthrough, uint32_t reversible)
{
    const UINT n = (UINT) input.size();
    auto source = gpu.Texture(n, &input);
    Encoded e {gpu.Texture(n, nullptr), gpu.Texture(n, nullptr), gpu.Texture(n / 2, nullptr)};
    auto spare = gpu.Texture(n, nullptr);
    gpu.Run(shader, Base(DlssNrMode_Encode, n, passthrough, reversible), source.Get(), nullptr, nullptr, e.proxy.Get(), e.keep.Get());
    gpu.Run(shader, Base(DlssNrMode_Downsample, n / 2, passthrough, reversible), e.proxy.Get(), nullptr, nullptr, e.half.Get(), spare.Get());
    return e;
}

// Resolve only: t0 the proxy the model saw, t1 its answer, t2 the kept frame.
static Row Resolve(Gpu& gpu, ID3D11ComputeShader* shader, ID3D11Texture2D* proxy, ID3D11Texture2D* model, ID3D11Texture2D* keep,
                   UINT n, const Settings& s)
{
    auto frame = gpu.Texture(n, nullptr);
    auto spare = gpu.Texture(n, nullptr);
    DlssNrConstants k = Base(DlssNrMode_Resolve, n, s.passthrough, s.reversible);
    k.Transfer = s.transfer;
    k.ModelWorkScale = s.workScale;
    gpu.Run(shader, k, proxy, model, keep, frame.Get(), spare.Get());
    return gpu.Read(frame.Get());
}

// sgsr1.hlsl's cbuffer (SGSR1_Dx12.cpp's Sgsr1Constants), padded to 16 bytes.
struct Sgsr1Constants
{
    float viewport[4];
    int32_t dstWidth, dstHeight;
    uint32_t reversibleMode, passthrough;
    float edgeThreshold, edgeSharpness;
    float pad[2];
};

// One SGSR1 enlarge of a row to twice its width, with the constants the NR pass uses (0.3 / 2.0).
static Row Sgsr1(Gpu& gpu, ID3D11ComputeShader* shader, const Row& input, uint32_t reversible)
{
    const UINT n = (UINT) input.size();
    auto source = gpu.Texture(n, &input);
    auto target = gpu.Texture(n * 2, nullptr);
    Sgsr1Constants k {{1.0f / n, 1.0f, (float) n, 1.0f}, (int32_t) n * 2, 1, reversible, 0, 0.3f, 2.0f, {}};
    D3D11_BUFFER_DESC desc {};
    desc.ByteWidth = sizeof(k);
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init {&k, 0, 0};
    ComPtr<ID3D11Buffer> buffer;
    check(gpu.device->CreateBuffer(&desc, &init, &buffer));
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    check(gpu.device->CreateShaderResourceView(source.Get(), nullptr, &srv));
    check(gpu.device->CreateUnorderedAccessView(target.Get(), nullptr, &uav));
    gpu.ctx->CSSetShader(shader, nullptr, 0);
    gpu.ctx->CSSetShaderResources(0, 1, srv.GetAddressOf());
    gpu.ctx->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
    gpu.ctx->CSSetConstantBuffers(0, 1, buffer.GetAddressOf());
    gpu.ctx->CSSetSamplers(0, 1, gpu.linearSampler.GetAddressOf());
    gpu.ctx->Dispatch((n * 2 + 7) / 8, 1, 1);
    ID3D11ShaderResourceView* noSrv = nullptr;
    ID3D11UnorderedAccessView* noUav = nullptr;
    gpu.ctx->CSSetShaderResources(0, 1, &noSrv);
    gpu.ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    return gpu.Read(target.Get());
}

static bool Finite(const Row& row)
{
    for (const Pixel& p : row)
        if (!std::isfinite(p.r) || !std::isfinite(p.g) || !std::isfinite(p.b) || !std::isfinite(p.a))
            return false;
    return true;
}

static bool SameBits(const Row& a, const Row& b)
{
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(Pixel)) == 0;
}

static float Peak(const Pixel& p) { return std::max({p.r, p.g, p.b}); }

int wmain(int argc, wchar_t** argv) try
{
    if (argc != 5)
        throw std::runtime_error("Pass <dlssnr.hlsl> <its reference from 7d518d74> <sgsr1.hlsl> <its reference from 7d518d74>");
    Gpu gpu;
    auto shader = gpu.Compile(argv[1]);
    auto reference = gpu.Compile(argv[2]);
    auto sgsr1 = gpu.Compile(argv[3]);
    auto sgsr1Reference = gpu.Compile(argv[4]);

    // Pairs, so the 2:1 downsample averages two equal taps.
    const Row toneMapped {{0.02f, 0.02f, 0.02f, 0.5f}, {0.02f, 0.02f, 0.02f, 0.5f}, {0.5f, 0.5f, 0.5f, 1}, {0.5f, 0.5f, 0.5f, 1},
                          {0.8f, 0.3f, 0.1f, 1},      {0.1f, 0.4f, 0.7f, 1},      {0.9f, 0.9f, 0.9f, 1}, {0.2f, 0.2f, 0.2f, 1}};
    const Row linear {{0.01f, 0.01f, 0.01f, 0.5f}, {0.01f, 0.01f, 0.01f, 0.5f}, {1.0f, 1.0f, 1.0f, 1}, {1.0f, 1.0f, 1.0f, 1},
                      {2.0f, 0.5f, 0.1f, 1},       {2.0f, 0.5f, 0.1f, 1},       {0.05f, 0.3f, 1.5f, 1}, {12.0f, 12.0f, 12.0f, 1}};

    // 1. Modes 0-4 bit-identical to the reference shader.
    for (uint32_t reversible : {0u, 1u, 2u, 3u, 4u})
        for (uint32_t passthrough : {0u, 1u})
        {
            const Row& input = passthrough ? toneMapped : linear;
            const Result a = Run(gpu, shader.Get(), input, passthrough, reversible);
            const Result b = Run(gpu, reference.Get(), input, passthrough, reversible);
            const std::string what = " (curve " + std::to_string(reversible) + ", passthrough " + std::to_string(passthrough) + ")";
            expect(SameBits(a.proxy, b.proxy), "proxy differs from the reference shader" + what);
            expect(SameBits(a.keep, b.keep), "kept copy differs from the reference shader" + what);
            expect(SameBits(a.half, b.half), "downsample differs from the reference shader" + what);
            expect(SameBits(a.frame, b.frame), "resolved frame differs from the reference shader" + what);
        }

    // 2. The signal curves.
    struct Curve { uint32_t mode; const char* name; float white; float headroom; };
    for (const Curve& c : {Curve {5, "HLG", 0.75f, 3.77f}, Curve {6, "PQ", 0.58f, 49.2f}, Curve {7, "linear", 1.0f, 1.0f}})
    {
        const std::string name = c.name;

        // A sweep of grey from 0 to 64x white, in pairs, plus the coloured pixels above.
        Row sweep;
        std::vector<double> levels;
        for (double n = 0.0; n <= 64.0; n = n < 1e-3 ? 1e-3 : n * 1.5)
            levels.push_back(n);
        levels.push_back(1.0);
        levels.push_back(64.0);
        std::sort(levels.begin(), levels.end());
        for (double n : levels)
            for (int k = 0; k < 2; ++k)
                sweep.push_back({(float) n, (float) n, (float) n, 0.5f});
        for (const Pixel& p : linear)
            sweep.push_back(p);

        const Result got = Run(gpu, shader.Get(), sweep, 0, c.mode);

        float worstSignal = 0.0f, worstDown = 0.0f, worstFrame = 0.0f, worstHue = 0.0f, whiteAt = -1.0f;
        bool rises = true, opaque = true;
        for (size_t i = 0; i < levels.size() * 2; ++i)
        {
            const double n = sweep[i].r;
            worstSignal = std::max(worstSignal, (float) std::abs(got.proxy[i].r - WantSignal(c.mode, n)));
            if (n == 1.0) whiteAt = got.proxy[i].r;
            if (i >= 2 && got.proxy[i].r + 1e-6f < got.proxy[i - 2].r) rises = false;
            // Strictly rising while below the curve's ceiling.
            if (i >= 2 && n > sweep[i - 2].r && n < c.headroom * 0.99 && got.proxy[i].r <= got.proxy[i - 2].r) rises = false;
        }
        for (size_t i = 0; i < sweep.size(); ++i)
        {
            opaque = opaque && got.proxy[i].a == 1.0f;
            const Pixel& p = got.proxy[i];
            const Pixel& d = got.half[i / 2];
            const Pixel& twin = sweep[i ^ 1];
            if (twin.r == sweep[i].r && twin.g == sweep[i].g && twin.b == sweep[i].b)
                worstDown = std::max({worstDown, std::abs(d.r - p.r), std::abs(d.g - p.g), std::abs(d.b - p.b)});
            const Pixel& f = got.frame[i];
            const Pixel& s = sweep[i];
            const float scale = std::max(Peak(s), 1e-3f);
            worstFrame = std::max({worstFrame, std::abs(f.r - s.r) / scale, std::abs(f.g - s.g) / scale, std::abs(f.b - s.b) / scale});
        }
        // Hue: the coloured pixels keep their channel ratios, read in the light the model sees (sRGB-decoded).
        auto decode = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
        for (size_t i = levels.size() * 2; i < sweep.size(); ++i)
        {
            const Pixel& s = sweep[i];
            const Pixel& p = got.proxy[i];
            const float peakIn = Peak(s), peakOut = decode(Peak(p));
            const float in[3] = {s.r / peakIn, s.g / peakIn, s.b / peakIn};
            const float out[3] = {decode(p.r) / peakOut, decode(p.g) / peakOut, decode(p.b) / peakOut};
            for (int k = 0; k < 3; ++k)
                worstHue = std::max(worstHue, std::abs(in[k] - out[k]));
        }

        std::printf("%s: signal %.2e, white at %.4f, downsample round trip %.2e, frame round trip %.2e, hue %.2e\n",
                    c.name, worstSignal, whiteAt, worstDown, worstFrame, worstHue);
        expect(worstSignal < 1e-5f, name + ": the proxy is not the standard's signal");
        expect(std::abs(whiteAt - c.white) <= 0.005f, name + ": white is not where BT.2408 puts it");
        expect(rises, name + ": the proxy does not rise with the light");
        expect(worstDown < 1e-5f, name + ": decode and re-encode do not give the proxy back");
        expect(worstFrame < 1e-4f, name + ": the resolve does not give the frame back");
        expect(worstHue < 1e-4f, name + ": the hue bends");
        expect(opaque, name + ": alpha is not opaque");
    }

    // 4. The model's edit arrives whole on the signal curves. Grey levels inside every curve's range even at +10%.
    {
        Row grey, brighter;
        for (float v : {0.02f, 0.1f, 0.3f, 0.6f, 0.85f})
            for (int k = 0; k < 2; ++k)
            {
                grey.push_back({v, v, v, 1});
                brighter.push_back({v * 1.1f, v * 1.1f, v * 1.1f, 1});
            }
        const UINT n = (UINT) grey.size();
        for (uint32_t mode = 0; mode <= 7; ++mode)
        {
            const Encoded frame = Encode(gpu, shader.Get(), grey, 0, mode);
            const Encoded answer = Encode(gpu, shader.Get(), brighter, 0, mode);
            const Row got = Resolve(gpu, shader.Get(), frame.proxy.Get(), answer.proxy.Get(), frame.keep.Get(), n, {0, mode});
            std::printf("curve %u: a 10%% brighter answer lifts white-ish grey (0.85) by %.1f%%\n", mode,
                        (got[n - 1].r / grey[n - 1].r - 1.0f) * 100.0f);
            if (mode <= 4)
            {
                const Encoded rf = Encode(gpu, reference.Get(), grey, 0, mode);
                const Encoded ra = Encode(gpu, reference.Get(), brighter, 0, mode);
                const Row want = Resolve(gpu, reference.Get(), rf.proxy.Get(), ra.proxy.Get(), rf.keep.Get(), n, {0, mode});
                expect(SameBits(got, want), "curve " + std::to_string(mode) + ": resolve with a changed answer differs from the reference");
                continue;
            }
            float worst = 0.0f;
            for (UINT i = 0; i < n; ++i)
                worst = std::max(worst, std::abs(got[i].r / grey[i].r - 1.1f));
            expect(worst < 2e-3f, "curve " + std::to_string(mode) + ": the model's edit does not arrive whole (off by " +
                                      std::to_string(worst) + ")");
        }
    }

    // 5. Matched residual, the model at half the frame's size and answering exactly what it was shown.
    for (uint32_t transfer : {1u, 2u})
        for (uint32_t mode = 0; mode <= 7; ++mode)
            for (uint32_t passthrough : {0u, 1u})
            {
                if (passthrough && mode > 4)
                    continue;
                const Row& input = passthrough ? toneMapped : linear;
                const UINT n = (UINT) input.size();
                const Settings settings {passthrough, mode, transfer, 0.5f};
                const Encoded e = Encode(gpu, shader.Get(), input, passthrough, mode);
                const Row got = Resolve(gpu, shader.Get(), e.half.Get(), e.half.Get(), e.keep.Get(), n, settings);
                const std::string what = " (matched residual " + std::to_string(transfer) + ", curve " + std::to_string(mode) +
                                         ", passthrough " + std::to_string(passthrough) + ")";
                if (mode <= 4)
                {
                    const Encoded r = Encode(gpu, reference.Get(), input, passthrough, mode);
                    const Row want = Resolve(gpu, reference.Get(), r.half.Get(), r.half.Get(), r.keep.Get(), n, settings);
                    if (transfer == 1)
                    {
                        expect(SameBits(got, want), "resolve differs from the reference shader" + what);
                        continue;
                    }
                    // The OkLab residual applies its ratio as L * exp(r) now, not exp(log(max(L, 0.1)) + r) (section 7),
                    // which rounds differently; none of these pixels sits below L 0.1, so the two agree to rounding.
                    float drift = 0.0f;
                    for (UINT i = 0; i < n; ++i)
                        drift = std::max({drift, std::abs(got[i].r - want[i].r) / std::max(Peak(want[i]), 1e-3f),
                                          std::abs(got[i].g - want[i].g) / std::max(Peak(want[i]), 1e-3f),
                                          std::abs(got[i].b - want[i].b) / std::max(Peak(want[i]), 1e-3f)});
                    std::printf("matched residual 2, curve %u, passthrough %u: drift from the reference %.2e\n", mode, passthrough, drift);
                    expect(drift < 1e-5f, "resolve differs from the reference shader beyond rounding" + what);
                    continue;
                }
                float worst = 0.0f;
                for (UINT i = 0; i < n; ++i)
                {
                    const float scale = std::max(Peak(input[i]), 1e-3f);
                    worst = std::max({worst, std::abs(got[i].r - input[i].r) / scale, std::abs(got[i].g - input[i].g) / scale,
                                      std::abs(got[i].b - input[i].b) / scale});
                }
                std::printf("matched residual %u, curve %u: frame round trip %.2e\n", transfer, mode, worst);
                expect(Finite(got), "not finite" + what);
                expect(worst < 1e-3f, "an unchanged answer does not give the frame back" + what);
            }

    // 6. SGSR1: modes 0-4 as before; the signal curves are enlarged on the stored value, like the soft knee.
    {
        const Row stored {{0.05f, 0.05f, 0.05f, 1}, {0.9f, 0.8f, 0.7f, 1}, {0.2f, 0.3f, 0.1f, 1}, {0.7f, 0.7f, 0.7f, 1},
                          {0.1f, 0.1f, 0.1f, 1},    {0.95f, 0.6f, 0.2f, 1}, {0.3f, 0.3f, 0.3f, 1}, {0.6f, 0.65f, 0.7f, 1}};
        const Row knee = Sgsr1(gpu, sgsr1.Get(), stored, 0);
        for (uint32_t mode = 0; mode <= 7; ++mode)
        {
            const Row got = Sgsr1(gpu, sgsr1.Get(), stored, mode);
            const std::string what = " (SGSR1, curve " + std::to_string(mode) + ")";
            if (mode <= 4)
                expect(SameBits(got, Sgsr1(gpu, sgsr1Reference.Get(), stored, mode)), "differs from the reference" + what);
            else
                expect(SameBits(got, knee), "not enlarged on the stored value like the soft knee" + what);
        }
        // Sensitivity: Neutwo strips the outer sRGB, so its enlarge must differ from the knee's on this row.
        expect(!SameBits(Sgsr1(gpu, sgsr1.Get(), stored, 1), knee), "SGSR1 test cannot tell the domains apart");
    }

    // 7. Shadows under the OkLab residual (transfer 2, the model at half size): an unchanged answer gives the dark
    //    pixels back on every curve, and a 10% brighter one moves them by about that -- the residual's 0.1 floor is
    //    for the ratio, not for the pixel it is applied to. Before the fix: 1.25 (knee), 0.5 (HLG), 12.9 (linear).
    {
        Row dark, darkBrighter;
        for (float v : {0.0f, 1e-5f, 1e-4f, 3e-4f, 1e-3f, 3e-3f, 0.01f, 0.03f})
            for (int k = 0; k < 2; ++k)
            {
                dark.push_back({v, v, v, 1});
                darkBrighter.push_back({v * 1.1f, v * 1.1f, v * 1.1f, 1});
            }
        for (const Pixel& p : {Pixel {2e-3f, 5e-4f, 1e-4f, 1}, Pixel {1e-4f, 3e-4f, 8e-4f, 1}})
            for (int k = 0; k < 2; ++k)
            {
                dark.push_back(p);
                darkBrighter.push_back({p.r * 1.1f, p.g * 1.1f, p.b * 1.1f, 1});
            }
        const UINT n = (UINT) dark.size();
        for (uint32_t mode = 0; mode <= 7; ++mode)
            for (uint32_t passthrough : {0u, 1u})
            {
                if (passthrough && mode > 4)
                    continue;
                const Settings settings {passthrough, mode, 2, 0.5f};
                const std::string what = " (OkLab residual, curve " + std::to_string(mode) + ", passthrough " +
                                         std::to_string(passthrough) + ")";
                const Encoded frame = Encode(gpu, shader.Get(), dark, passthrough, mode);
                const Encoded answer = Encode(gpu, shader.Get(), darkBrighter, passthrough, mode);
                const Row same = Resolve(gpu, shader.Get(), frame.half.Get(), frame.half.Get(), frame.keep.Get(), n, settings);
                const Row lifted = Resolve(gpu, shader.Get(), frame.half.Get(), answer.half.Get(), frame.keep.Get(), n, settings);
                // Errors against 1e-3 of white or the pixel, whichever is larger: what shows on screen.
                float worstSame = 0.0f, worstLift = 0.0f;
                for (UINT i = 0; i < n; ++i)
                {
                    const float scale = std::max(Peak(dark[i]), 1e-3f);
                    for (int c = 0; c < 3; ++c)
                    {
                        const float in = (&dark[i].r)[c];
                        worstSame = std::max(worstSame, std::abs((&same[i].r)[c] - in) / scale);
                        // Moved by more than the answer's 10%, either way. The residual carries an edit within about
                        // a point of the full-size model's on HLG and PQ, so 2% of the pixel is allowed.
                        worstLift = std::max(worstLift, std::abs((&lifted[i].r)[c] - in) / scale - 0.1f * in / scale);
                    }
                }
                std::printf("OkLab residual shadows, curve %u, passthrough %u: unchanged answer %.2e, brighter answer overshoot %.2e\n",
                            mode, passthrough, worstSame, worstLift);
                expect(Finite(same) && Finite(lifted), "not finite" + what);
                expect(worstSame < 1e-3f, "an unchanged answer does not give the shadows back" + what);
                expect(worstLift < 0.02f, "a 10% brighter answer moves the shadows by more than 12%" + what);
            }
    }

    if (fails)
    {
        std::printf("%d check(s) failed\n", fails);
        return 1;
    }
    std::puts("PASS: modes 0-4 unchanged (encode, downsample, resolve, matched residual, SGSR1); HLG, PQ and linear\n"
              "      proxies correct, reversible, composed in scene light; the OkLab residual keeps the shadows (WARP HLSL)");
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
