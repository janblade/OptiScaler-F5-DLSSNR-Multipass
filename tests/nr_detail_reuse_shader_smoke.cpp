// Headless shader test: Windows D3D11 WARP executes precompile/dlssnr_detail_reuse.hlsl (reuse detail between frames).
// Checks each mode on small synthetic images: detail follows the motion (at the motion texture's own size, subrect and
// scale), dropped where depth or colour disagree, kept at still edges, the better of the pixel's own and the nearer
// surface's motion, no ringing, composed vectors across a render-size change (and with a subrect, half size and game
// scale), padded depth guides, invalid saved detail, fill (including partial trust), steadiness, NaN safety; the
// coverage grid that says how much of a frame had no detail to move; the Replace modes, which land a moved change so
// that it cannot blow up near white; and a depth-less frame (DepthWidth/DepthHeight == 0), which falls back to
// colour-only trust instead of reading the colour that stands in for depth's descriptor slot. cl /std:c++20 /EHsc /W4
// /wd4324 tests/nr_detail_reuse_shader_smoke.cpp d3d11.lib d3dcompiler.lib nr_detail_reuse_shader_smoke.exe
// OptiScaler/shaders/dlssnr/precompile/dlssnr_detail_reuse.hlsl [reference.hlsl] With a reference (the shader before
// the Replace change: git show c62aae96:<that path> > reference.hlsl), every run that is not a Replace one must give
// bit-identical output on both.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>
#include "../OptiScaler/shaders/dlssnr/DlssNr_DetailReuseConstants.h"
using Microsoft::WRL::ComPtr;

static void check(HRESULT hr)
{
    if (FAILED(hr))
        throw std::runtime_error("D3D call failed");
}

static int fails = 0;
static void expect(bool ok, const char* label)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", label);
        ++fails;
    }
}

struct Px
{
    float r, g, b, a;
};

// The working size; guides may differ.
constexpr unsigned W = 16, H = 8;

struct Img
{
    unsigned w = W, h = H;
    std::vector<Px> px;
    Px at(unsigned x, unsigned y) const { return px[y * w + x]; }
};

static Img Fill(const std::function<Px(unsigned, unsigned)>& f, unsigned w = W, unsigned h = H)
{
    Img img;
    img.w = w;
    img.h = h;
    img.px.resize(w * h);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x)
            img.px[y * w + x] = f(x, y);
    return img;
}

struct Gpu
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11ComputeShader> reference; // optional: the shader before the Replace change
    int identityRuns = 0, identityDiffs = 0;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;

    ComPtr<ID3D11Texture2D> Make(unsigned w, unsigned h, UINT bind, D3D11_USAGE usage = D3D11_USAGE_DEFAULT,
                                 const void* data = nullptr, UINT cpu = 0)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = usage;
        desc.BindFlags = bind;
        desc.CPUAccessFlags = cpu;
        D3D11_SUBRESOURCE_DATA init { data, w * (UINT) sizeof(Px), 0 };
        ComPtr<ID3D11Texture2D> tex;
        check(device->CreateTexture2D(&desc, data != nullptr ? &init : nullptr, &tex));
        return tex;
    }

    std::vector<Px> Read(ID3D11Texture2D* tex, unsigned w, unsigned h)
    {
        const auto readback = Make(w, h, 0, D3D11_USAGE_STAGING, nullptr, D3D11_CPU_ACCESS_READ);
        ctx->CopyResource(readback.Get(), tex);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        check(ctx->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<Px> result(w * h);
        for (unsigned y = 0; y < h; ++y)
            memcpy(&result[y * w], (const char*) mapped.pData + y * mapped.RowPitch, w * sizeof(Px));
        ctx->Unmap(readback.Get(), 0);
        return result;
    }

    // Runs one mode over outW x outH threads; returns u0 (and u1 when asked), both outW x outH. Outside the Replace
    // modes the reference shader, when there is one, must give the same bits.
    Img Run(const DlssNrDetailReuseConstants& c, const std::vector<Img>& inputs, Img* second = nullptr,
            unsigned outW = W, unsigned outH = H)
    {
        Img u1;
        Img u0 = RunOn(shader.Get(), c, inputs, u1, outW, outH);
        // Coverage is newer than the reference shader, which has no such mode to compare with.
        if (reference && c.ReplaceCurve == DlssNrReplaceCurve_None && c.Mode != DlssNrDetailReuse_Coverage)
        {
            Img r1;
            const Img r0 = RunOn(reference.Get(), c, inputs, r1, outW, outH);
            ++identityRuns;
            if (memcmp(u0.px.data(), r0.px.data(), u0.px.size() * sizeof(Px)) != 0 ||
                memcmp(u1.px.data(), r1.px.data(), u1.px.size() * sizeof(Px)) != 0)
                ++identityDiffs;
        }
        if (second != nullptr)
            *second = u1;
        return u0;
    }

    Img RunOn(ID3D11ComputeShader* program, const DlssNrDetailReuseConstants& c, const std::vector<Img>& inputs,
              Img& second, unsigned outW, unsigned outH)
    {
        std::vector<ComPtr<ID3D11ShaderResourceView>> srvs;
        for (const auto& in : inputs)
        {
            const auto tex = Make(in.w, in.h, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, in.px.data());
            ComPtr<ID3D11ShaderResourceView> srv;
            check(device->CreateShaderResourceView(tex.Get(), nullptr, &srv));
            srvs.push_back(srv);
        }
        while (srvs.size() < 5)
            srvs.push_back(srvs.front());

        const auto out0 = Make(outW, outH, D3D11_BIND_UNORDERED_ACCESS);
        const auto out1 = Make(outW, outH, D3D11_BIND_UNORDERED_ACCESS);
        ComPtr<ID3D11UnorderedAccessView> uav0, uav1;
        check(device->CreateUnorderedAccessView(out0.Get(), nullptr, &uav0));
        check(device->CreateUnorderedAccessView(out1.Get(), nullptr, &uav1));

        ID3D11ShaderResourceView* views[5];
        for (int i = 0; i < 5; ++i)
            views[i] = srvs[i].Get();
        ID3D11UnorderedAccessView* uavs[] = { uav0.Get(), uav1.Get() };
        ctx->CSSetShader(program, nullptr, 0);
        ctx->CSSetShaderResources(0, 5, views);
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
        ctx->CSSetSamplers(0, 1, sampler.GetAddressOf());
        ctx->UpdateSubresource(constants.Get(), 0, nullptr, &c, 0, 0);
        ctx->Dispatch((outW + 7) / 8, (outH + 7) / 8, 1);
        ID3D11UnorderedAccessView* none[] = { nullptr, nullptr };
        ctx->CSSetUnorderedAccessViews(0, 2, none, nullptr);

        second = Img { outW, outH, Read(out1.Get(), outW, outH) };
        return Img { outW, outH, Read(out0.Get(), outW, outH) };
    }
};

static Px Grey(float v) { return { v, v, v, 1 }; }
static bool Near(float a, float b, float e = 1e-4f) { return std::abs(a - b) < e; }
static bool Near3(Px a, Px b, float e = 1e-4f) { return Near(a.r, b.r, e) && Near(a.g, b.g, e) && Near(a.b, b.b, e); }
static bool Finite(Px p) { return std::isfinite(p.r) && std::isfinite(p.g) && std::isfinite(p.b) && std::isfinite(p.a); }

static DlssNrDetailReuseConstants Base(DlssNrDetailReuseMode mode)
{
    DlssNrDetailReuseConstants c {};
    c.Mode = mode;
    c.WorkWidth = c.MotionWidth = c.DepthWidth = W;
    c.WorkHeight = c.MotionHeight = c.DepthHeight = H;
    c.DepthInverted = 1;
    c.MvScaleX = c.MvScaleY = 1.0f;
    c.DepthTolerance = kDlssNrDetailReuseDepthTolerance;
    c.ClipGamma = kDlssNrDetailReuseClipGamma;
    c.ClipFalloff = kDlssNrDetailReuseClipFalloff;
    c.SigmaFloor = kDlssNrDetailReuseSigmaFloor;
    c.MotionReject = kDlssNrDetailReuseMotionReject;
    return c;
}

// A residual that varies per pixel, so moved detail is recognisable.
static Px Detail(unsigned x, unsigned y) { return { 0.01f * x, -0.02f * y, 0.005f * (x + y), 1 }; }
static Px Plus(Px in, Px d) { return { in.r + d.r, in.g + d.g, in.b + d.b, 1 }; }

// What the resolve does with a Replace answer, on a grey (dlssnr_replace_curve.hlsli; the peak channel is the value).
static float SrgbToLinear1(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    return v >= 0.04045f ? std::pow((v + 0.055f) / 1.055f, 2.4f) : v / 12.92f;
}
static float LinearToSrgb1(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    return v >= 0.0031308f ? 1.055f * std::pow(std::max(v, 1e-8f), 1.0f / 2.4f) - 0.055f : v * 12.92f;
}
static float NeutwoOf(float x) { return x / std::sqrt(x * x + 1.0f); }
static float NeutwoInv(float y)
{
    y = std::min(y, 0.999999f);
    return y / std::sqrt(std::max(1.0f - y * y, 1e-8f));
}
static float HybridOf(float x) { return x <= 0.75f ? x : 0.75f + 0.25f * NeutwoOf((x - 0.75f) / 0.25f); }
static float HybridInv(float y) { return y <= 0.75f ? y : 0.75f + 0.25f * NeutwoInv((y - 0.75f) / 0.25f); }
// Scene light -> the stored proxy value, and back.
static float Encode(float light, uint32_t curve)
{
    return LinearToSrgb1(curve == DlssNrReplaceCurve_Neutwo ? NeutwoOf(light) : HybridOf(light));
}
static float Decode(float proxy, uint32_t curve)
{
    const float y = SrgbToLinear1(proxy);
    return curve == DlssNrReplaceCurve_Neutwo ? NeutwoInv(y) : HybridInv(y);
}
static Img GreyImg(float v) { return Fill([=](unsigned, unsigned) { return Grey(v); }); }
// The same on a colour: one scalar from the peak channel, so the hue is kept (NeutwoEncode / HybridEncode).
static Px EncodeRgb(Px light, uint32_t curve)
{
    const float m = std::max(light.r, std::max(light.g, light.b));
    const float scale = (curve == DlssNrReplaceCurve_Neutwo ? NeutwoOf(m) : HybridOf(m)) / m;
    return { LinearToSrgb1(light.r * scale), LinearToSrgb1(light.g * scale), LinearToSrgb1(light.b * scale), 1 };
}
static Px DecodeRgb(Px proxy, uint32_t curve)
{
    const Px y { SrgbToLinear1(proxy.r), SrgbToLinear1(proxy.g), SrgbToLinear1(proxy.b), 1 };
    const float m = std::max(y.r, std::max(y.g, y.b));
    if (m <= 1e-6f)
        return y;
    const float scale = (curve == DlssNrReplaceCurve_Neutwo ? NeutwoInv(m) : HybridInv(m)) / m;
    return { y.r * scale, y.g * scale, y.b * scale, 1 };
}
// How far `to` is from `from` (both light), in stops of whichever channel moved most.
static float WorstStops(Px from, Px to)
{
    const auto stops = [](float a, float b) { return std::abs(std::log2((b + 1.0f / 512) / (a + 1.0f / 512))); };
    return std::max(stops(from.r, to.r), std::max(stops(from.g, to.g), stops(from.b, to.b)));
}

static ComPtr<ID3DBlob> Compile(const wchar_t* path)
{
    ComPtr<ID3DBlob> code, errors;
    const HRESULT compiled = D3DCompileFromFile(path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSMain", "cs_5_0",
                                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors)
        std::fprintf(stderr, "%s", (char*) errors->GetBufferPointer());
    check(compiled);
    return code;
}

int wmain(int argc, wchar_t** argv)
try
{
    if (argc != 2 && argc != 3)
        throw std::runtime_error("Pass the dlssnr_detail_reuse.hlsl path, and optionally the reference shader's");
    const auto code = Compile(argv[1]);

    Gpu gpu;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &gpu.device,
                            nullptr, &gpu.ctx));
    check(gpu.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &gpu.shader));
    if (argc == 3)
    {
        const auto referenceCode = Compile(argv[2]);
        check(gpu.device->CreateComputeShader(referenceCode->GetBufferPointer(), referenceCode->GetBufferSize(), nullptr,
                                              &gpu.reference));
    }
    D3D11_BUFFER_DESC buffer {};
    buffer.ByteWidth = sizeof(DlssNrDetailReuseConstants);
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    check(gpu.device->CreateBuffer(&buffer, nullptr, &gpu.constants));
    D3D11_SAMPLER_DESC sampling {};
    sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampling.MaxLOD = D3D11_FLOAT32_MAX;
    check(gpu.device->CreateSamplerState(&sampling, &gpu.sampler));

    const auto grey = Fill([](unsigned, unsigned) { return Grey(0.5f); });
    const auto detail = Fill(Detail);
    const auto still = Fill([](unsigned, unsigned) { return Px { 0, 0, 0, 0 }; });
    const auto depth = Fill([](unsigned, unsigned) { return Grey(0.3f); }); // one surface, reversed Z
    const auto savedColourDepth = Fill([](unsigned, unsigned) { return Px { 0.5f, 0.5f, 0.5f, 0.3f }; });
    const auto withDetail = [](const Img& in, unsigned x, unsigned y) { return Plus(in.at(x, y), Detail(x, y)); };

    // Capture: detail = answer - input, colour + far-is-zero depth at the pixel itself.
    {
        const auto answer = Fill([](unsigned x, unsigned y) { return Plus(Grey(0.5f), Detail(x, y)); });
        Img colourDepth;
        const auto saved = gpu.Run(Base(DlssNrDetailReuse_Capture), { grey, answer, depth }, &colourDepth);
        bool ok = true;
        for (unsigned y = 0; y < H; ++y)
            for (unsigned x = 0; x < W; ++x)
                ok = ok && Near3(saved.at(x, y), Detail(x, y)) && Near(saved.at(x, y).a, 1) &&
                     Near3(colourDepth.at(x, y), Grey(0.5f)) && Near(colourDepth.at(x, y).a, 0.3f);
        expect(ok, "Capture: detail is answer - input, colour and depth saved");

        DlssNrDetailReuseConstants c = Base(DlssNrDetailReuse_Capture);
        c.DepthInverted = 0;
        gpu.Run(c, { grey, answer, depth }, &colourDepth);
        expect(Near(colourDepth.at(3, 3).a, 0.7f), "Capture: standard Z saved as 1 - depth (far is zero)");
    }

    // Reproject, still scene: exactly input + detail.
    {
        const auto out = gpu.Run(Base(DlssNrDetailReuse_Reproject), { grey, detail, savedColourDepth, still, depth });
        bool ok = true;
        for (unsigned y = 0; y < H; ++y)
            for (unsigned x = 0; x < W; ++x)
                ok = ok && Near3(out.at(x, y), withDetail(grey, x, y));
        expect(ok, "Reproject: still scene is input + saved detail, exactly");
    }

    // Reproject, whole image moved 2 px: previous = current + 2, so the detail comes from x + 2.
    const auto movedBy2 = [&](const Img& out)
    {
        bool ok = true;
        for (unsigned y = 0; y < H; ++y)
            for (unsigned x = 0; x + 2 < W; ++x)
                ok = ok && Near3(out.at(x, y), Plus(Grey(0.5f), Detail(x + 2, y)));
        return ok;
    };
    {
        const auto motion = Fill([](unsigned, unsigned) { return Px { 2, 0, 0, 0 }; });
        const auto out = gpu.Run(Base(DlssNrDetailReuse_Reproject), { grey, detail, savedColourDepth, motion, depth });
        expect(movedBy2(out), "Reproject: detail follows the motion vectors");
        expect(Near3(out.at(W - 1, 2), Grey(0.5f)), "Reproject: history off-screen leaves the input alone");
    }

    // Motion at half size (render-resolution vectors): raw is in motion-texture pixels, so 1 px there is 2 here.
    {
        const auto half = Fill([](unsigned, unsigned) { return Px { 1, 0, 0, 0 }; }, W / 2, H / 2);
        DlssNrDetailReuseConstants c = Base(DlssNrDetailReuse_Reproject);
        c.MotionWidth = W / 2;
        c.MotionHeight = H / 2;
        const auto out = gpu.Run(c, { grey, detail, savedColourDepth, half, depth });
        expect(movedBy2(out), "Reproject: half-size motion vectors are scaled by the motion texture's size");
    }

    // Motion subrect inside a larger texture, with a negative scale.
    {
        const auto padded = Fill([](unsigned x, unsigned y) {
            const bool inside = x >= 3 && x < 3 + W && y >= 1 && y < 1 + H;
            return inside ? Px { -2, 0, 0, 0 } : Px { 100, 100, 0, 0 };
        }, W + 5, H + 3);
        DlssNrDetailReuseConstants c = Base(DlssNrDetailReuse_Reproject);
        c.MotionBaseX = 3;
        c.MotionBaseY = 1;
        c.MvScaleX = c.MvScaleY = -1.0f;
        const auto out = gpu.Run(c, { grey, detail, savedColourDepth, padded, depth });
        expect(movedBy2(out), "Reproject: motion subrect base and a negative scale are honoured");
    }

    // Depth disagrees (another surface was there): detail dropped. Within tolerance: kept.
    {
        const auto farther = Fill([](unsigned, unsigned) { return Px { 0.5f, 0.5f, 0.5f, 0.15f }; });
        auto out = gpu.Run(Base(DlssNrDetailReuse_Reproject), { grey, detail, farther, still, depth });
        expect(Near3(out.at(5, 3), Grey(0.5f)), "Reproject: detail dropped where depth disagrees");
        const auto close = Fill([](unsigned, unsigned) { return Px { 0.5f, 0.5f, 0.5f, 0.3f * 0.99f }; });
        out = gpu.Run(Base(DlssNrDetailReuse_Reproject), { grey, detail, close, still, depth });
        expect(Near3(out.at(5, 3), withDetail(grey, 5, 3)), "Reproject: detail kept within the depth tolerance");
    }

    // No depth this frame (DepthWidth/DepthHeight == 0, the signal DlssNr_Dx12::Dispatch's guide resolution
    // produces when the game -- or the native producer's generic depth finder -- supplied none): the depth
    // guide (t4/t2 in this mode) is not read for trust at all, so the same depth disagreement that dropped
    // detail above is now ignored and trust falls back to colour alone.
    {
        DlssNrDetailReuseConstants c = Base(DlssNrDetailReuse_Reproject);
        c.DepthWidth = 0;
        c.DepthHeight = 0;
        const auto farther = Fill([](unsigned, unsigned) { return Px { 0.5f, 0.5f, 0.5f, 0.15f }; });
        const auto out = gpu.Run(c, { grey, detail, farther, still, depth });
        expect(Near3(out.at(5, 3), withDetail(grey, 5, 3)),
               "Reproject: no depth this frame keeps detail on colour trust alone");

        DlssNrDetailReuseConstants capture = Base(DlssNrDetailReuse_Capture);
        capture.DepthWidth = 0;
        capture.DepthHeight = 0;
        const auto answer = Fill([](unsigned x, unsigned y) { return Plus(Grey(0.5f), Detail(x, y)); });
        Img colourDepth;
        gpu.Run(capture, { grey, answer, depth }, &colourDepth);
        bool savedZero = true;
        for (const Px& px : colourDepth.px)
            savedZero = savedZero && Near(px.a, 0.0f);
        expect(savedZero, "Capture: no depth this frame saves a constant placeholder, not colour read as depth");
    }

    // Standard Z: the depth guide holds 1 - (far is zero); saved values are far-is-zero.
    {
        const auto standard = Fill([](unsigned, unsigned) { return Grey(0.7f); });
        DlssNrDetailReuseConstants c = Base(DlssNrDetailReuse_Reproject);
        c.DepthInverted = 0;
        auto out = gpu.Run(c, { grey, detail, savedColourDepth, still, standard });
        expect(Near3(out.at(5, 3), withDetail(grey, 5, 3)), "Reproject: standard Z, same surface keeps detail");
        const auto farther = Fill([](unsigned, unsigned) { return Px { 0.5f, 0.5f, 0.5f, 0.15f }; });
        out = gpu.Run(c, { grey, detail, farther, still, standard });
        expect(Near3(out.at(5, 3), Grey(0.5f)), "Reproject: standard Z, another surface drops detail");
    }

    // A still depth edge keeps detail on both sides (no dropped outline around still silhouettes).
    {
        const auto edge = Fill([](unsigned x, unsigned) { return Grey(x < 8 ? 0.5f : 0.1f); });
        const auto edgeSaved = Fill([](unsigned x, unsigned) { return Px { 0.5f, 0.5f, 0.5f, x < 8 ? 0.5f : 0.1f }; });
        const auto out = gpu.Run(Base(DlssNrDetailReuse_Reproject), { grey, detail, edgeSaved, still, edge });
        bool ok = true;
        for (unsigned y = 0; y < H; ++y)
            for (unsigned x = 0; x < W; ++x)
                ok = ok && Near3(out.at(x, y), withDetail(grey, x, y));
        expect(ok, "Reproject: still silhouette keeps its detail on both sides");
    }

    // Colour outside the current neighbourhood's box: dropped. Inside a noisy neighbourhood's box: kept.
    {
        const auto brighter = Fill([](unsigned, unsigned) { return Px { 0.9f, 0.9f, 0.9f, 0.3f }; });
        auto out = gpu.Run(Base(DlssNrDetailReuse_Reproject), { grey, detail, brighter, still, depth });
        expect(Near3(out.at(5, 3), Grey(0.5f)), "Reproject: detail dropped where colour left the box");

        const auto noisy = Fill([](unsigned x, unsigned y) { return Grey((x + y) % 2 ? 0.55f : 0.45f); });
        out = gpu.Run(Base(DlssNrDetailReuse_Reproject), { noisy, detail, savedColourDepth, still, depth });
        expect(Near3(out.at(5, 3), withDetail(noisy, 5, 3)),
               "Reproject: detail kept when the saved colour is inside a noisy neighbourhood's box");
    }

    // No ringing: half-pixel motion across a detail step stays within the step.
    {
        const auto step = Fill([](unsigned x, unsigned) { return x < W / 2 ? Px { 0, 0, 0, 1 } : Px { 0.2f, 0.2f, 0.2f, 1 }; });
        const auto halfPx = Fill([](unsigned, unsigned) { return Px { 0.5f, 0, 0, 0 }; });
        const auto out = gpu.Run(Base(DlssNrDetailReuse_Reproject), { grey, step, savedColourDepth, halfPx, depth });
        bool ok = true;
        for (const Px& p : out.px)
            ok = ok && p.r >= 0.5f - 1e-5f && p.r <= 0.7f + 1e-5f;
        expect(ok, "Reproject: moved detail does not ring past the saved values");
    }

    // Moving nearer surface (x >= 8, colour 0.8, moving so previous = current + 2) over a still background (0.3).
    // Last frame the surface started at x = 10. The background pixel next to it keeps its own motion and detail; the
    // surface's own edge pixel follows the surface.
    {
        const auto input = Fill([](unsigned x, unsigned) { return Grey(x < 8 ? 0.3f : 0.8f); });
        const auto nowDepth = Fill([](unsigned x, unsigned) { return Grey(x < 8 ? 0.1f : 0.5f); });
        const auto nowMotion = Fill([](unsigned x, unsigned) { return x < 8 ? Px { 0, 0, 0, 0 } : Px { 2, 0, 0, 0 }; });
        const auto saved = Fill([](unsigned x, unsigned) {
            return x < 10 ? Px { 0.3f, 0.3f, 0.3f, 0.1f } : Px { 0.8f, 0.8f, 0.8f, 0.5f };
        });
        const auto out = gpu.Run(Base(DlssNrDetailReuse_Reproject), { input, detail, saved, nowMotion, nowDepth });
        expect(Near3(out.at(7, 2), withDetail(input, 7, 2)),
               "Reproject: background next to a moving surface keeps its own motion and detail");
        expect(Near3(out.at(8, 2), Plus(input.at(8, 2), Detail(10, 2))),
               "Reproject: the moving surface's edge pixel follows the surface");
        expect(Near3(out.at(4, 2), withDetail(input, 4, 2)), "Reproject: background away from the edge unchanged");
    }

    // Same colours on both sides: nothing tells the two motions apart, and the nearer surface's motion wins the tie.
    {
        const auto edgeDepth = Fill([](unsigned x, unsigned) { return Grey(x >= 8 ? 0.5f : 0.1f); });
        const auto edgeMotion = Fill([](unsigned x, unsigned) { return x >= 8 ? Px { 2, 0, 0, 0 } : Px { 0, 0, 0, 0 }; });
        const auto edgeSaved = Fill([](unsigned x, unsigned) { return Px { 0.5f, 0.5f, 0.5f, x >= 8 ? 0.5f : 0.1f }; });
        const auto out = gpu.Run(Base(DlssNrDetailReuse_Reproject), { grey, detail, edgeSaved, edgeMotion, edgeDepth });
        expect(Near3(out.at(7, 2), Plus(Grey(0.5f), Detail(9, 2))), "Reproject: a tie keeps the nearer surface's motion");
    }

    // A square (x 8..11, moving so previous = current + 2) over a still background, one surface and one colour, so
    // neither depth nor colour can tell anything apart. The square's trailing pixels (x 10, 11) point at x 12, 13,
    // where the current vectors are the background's: the place they came from moves differently now, so their moved
    // detail is dropped. The leading pixels (x 8, 9) point inside the square and follow it; the background is still.
    {
        const auto squareMotion =
            Fill([](unsigned x, unsigned) { return x >= 8 && x < 12 ? Px { 2, 0, 0, 0 } : Px { 0, 0, 0, 0 }; });
        const auto out =
            gpu.Run(Base(DlssNrDetailReuse_Reproject), { grey, detail, savedColourDepth, squareMotion, depth });
        bool follows = true, dropped = true, stays = true;
        for (unsigned y = 0; y < H; ++y)
        {
            for (unsigned x = 8; x < 10; ++x)
                follows = follows && Near3(out.at(x, y), Plus(Grey(0.5f), Detail(x + 2, y)));
            for (unsigned x = 10; x < 12; ++x)
                dropped = dropped && Near3(out.at(x, y), Grey(0.5f));
            for (unsigned x = 0; x < W; ++x)
                stays = stays && (x >= 8 && x < 12 || Near3(out.at(x, y), withDetail(grey, x, y)));
        }
        expect(follows, "Reproject: pixels whose source moves with them keep their moved detail");
        expect(dropped, "Reproject: moved detail dropped where the vectors at its source disagree");
        expect(stays, "Reproject: still background beside a moving square keeps its detail");

        DlssNrDetailReuseConstants off = Base(DlssNrDetailReuse_Reproject);
        off.MotionReject = 0.0f;
        const auto unchecked = gpu.Run(off, { grey, detail, savedColourDepth, squareMotion, depth });
        expect(Near3(unchecked.at(10, 3), Plus(Grey(0.5f), Detail(12, 3))),
               "Reproject: MotionReject 0 switches the check off");

        // The same square seen with a small difference (sub-pixel noise between neighbours) is not a disagreement.
        const auto jitter = Fill([](unsigned x, unsigned) { return Px { x >= 8 && x < 12 ? 2.0f : 1.8f, 0, 0, 0 }; });
        const auto soft = gpu.Run(Base(DlssNrDetailReuse_Reproject), { grey, detail, savedColourDepth, jitter, depth });
        expect(Near3(soft.at(10, 3), Plus(Grey(0.5f), Detail(12, 3)), 0.02f) &&
                   Near3(soft.at(3, 3), Plus(Grey(0.5f), Detail(5, 3)), 0.02f),
               "Reproject: a small difference between neighbouring vectors keeps the moved detail");

        // Debug view: dropped by motion is yellow, not the magenta of the other drops.
        DlssNrDetailReuseConstants debug = Base(DlssNrDetailReuse_Reproject);
        debug.DebugView = 1;
        const auto painted = gpu.Run(debug, { grey, detail, savedColourDepth, squareMotion, depth });
        expect(Near3(painted.at(10, 3), { 1, 1, 0, 1 }), "Reproject debug view: dropped by motion is yellow");
        const auto farther = Fill([](unsigned, unsigned) { return Px { 0.5f, 0.5f, 0.5f, 0.15f }; });
        const auto byDepth = gpu.Run(debug, { grey, detail, farther, still, depth });
        expect(Near3(byDepth.at(5, 3), { 1, 0, 1, 1 }), "Reproject debug view: dropped by depth stays magenta");
    }

    // NaN in the saved detail: finite output, and no trust in the estimate there.
    {
        auto broken = detail;
        broken.px[3 * W + 5].r = std::numeric_limits<float>::quiet_NaN();
        const auto out = gpu.Run(Base(DlssNrDetailReuse_Reproject), { grey, broken, savedColourDepth, still, depth });
        bool ok = true;
        for (const Px& p : out.px)
            ok = ok && Finite(p);
        expect(ok, "Reproject: NaN in the saved detail never reaches the output");
        const auto estimate = gpu.Run(Base(DlssNrDetailReuse_Estimate), { grey, broken, savedColourDepth, still, depth });
        expect(Near(estimate.at(5, 3).a, 0), "Estimate: no trust where the saved detail is NaN");
    }

    // Debug view paints dropped detail magenta.
    {
        const auto farther = Fill([](unsigned, unsigned) { return Px { 0.5f, 0.5f, 0.5f, 0.15f }; });
        DlssNrDetailReuseConstants c = Base(DlssNrDetailReuse_Reproject);
        c.DebugView = 1;
        const auto out = gpu.Run(c, { grey, detail, farther, still, depth });
        expect(Near3(out.at(5, 3), { 1, 0, 1, 1 }), "Reproject: debug view marks dropped detail");
    }

    // Estimate: the moved detail and its trust.
    {
        auto out = gpu.Run(Base(DlssNrDetailReuse_Estimate), { grey, detail, savedColourDepth, still, depth });
        expect(Near3(out.at(5, 3), Detail(5, 3)) && Near(out.at(5, 3).a, 1), "Estimate: detail with full trust");
        const auto farther = Fill([](unsigned, unsigned) { return Px { 0.5f, 0.5f, 0.5f, 0.15f }; });
        out = gpu.Run(Base(DlssNrDetailReuse_Estimate), { grey, detail, farther, still, depth });
        expect(Near(out.at(5, 3).a, 0), "Estimate: no trust where depth disagrees");
    }

    // Steady: 0 leaves the answer alone; 1 with full trust takes the estimate; halves multiply; NaN is ignored.
    {
        const auto answer = Fill([](unsigned, unsigned) { return Px { 0.6f, 0.6f, 0.6f, 1 }; }); // fresh detail +0.1
        const auto trusted = Fill([](unsigned, unsigned) { return Px { 0.2f, 0.2f, 0.2f, 1 }; });
        const auto halfTrusted = Fill([](unsigned, unsigned) { return Px { 0.2f, 0.2f, 0.2f, 0.5f }; });
        DlssNrDetailReuseConstants c = Base(DlssNrDetailReuse_Steady);
        c.Steady = 0;
        auto out = gpu.Run(c, { grey, answer, trusted });
        expect(Near3(out.at(4, 4), Grey(0.6f)), "Steady: 0 is the model's answer");
        c.Steady = 1;
        out = gpu.Run(c, { grey, answer, trusted });
        expect(Near3(out.at(4, 4), Grey(0.7f)), "Steady: 1 with full trust is input + estimate");
        c.Steady = 0.5f;
        out = gpu.Run(c, { grey, answer, halfTrusted });
        expect(Near3(out.at(4, 4), Grey(0.6f + 0.25f * 0.1f)), "Steady: strength times trust");
        auto nanEstimate = trusted;
        nanEstimate.px[4 * W + 4] = { std::numeric_limits<float>::quiet_NaN(), 0.2f, 0.2f, 1 };
        c.Steady = 1;
        out = gpu.Run(c, { grey, answer, nanEstimate });
        expect(Finite(out.at(4, 4)), "Steady: a NaN estimate never reaches the output");
    }

    // SaveMotion keeps this frame's vectors as work-image uv displacement (independent of the render size); Compose
    // adds them to the next frame's vectors read at the moved position, then returns raw units of the new size.
    {
        // Frame A: motion at half size, raw.x = its texel column (motion pixels), scale 1.
        const auto frameA = Fill([](unsigned x, unsigned) { return Px { (float) x, 0, 0, 0 }; }, W / 2, H / 2);
        DlssNrDetailReuseConstants save = Base(DlssNrDetailReuse_SaveMotion);
        save.MotionWidth = W / 2;
        save.MotionHeight = H / 2;
        const auto saved = gpu.Run(save, { frameA, frameA, frameA, frameA, frameA });
        // Work pixel 5 lies in motion texel 2: displacement 2 motion pixels of 8 = 2/8 of the image width.
        expect(Near(saved.at(5, 3).r, 2.0f / (W / 2)) && Near(saved.at(5, 3).g, 0),
               "SaveMotion: vectors kept as uv displacement");

        // Frame B: the render size changed to full size; raw 2 motion pixels = 2 work pixels.
        const auto frameB = Fill([](unsigned, unsigned) { return Px { 2, 0, 0, 0 }; });
        const auto out = gpu.Run(Base(DlssNrDetailReuse_Compose), { frameB, frameB, frameB, frameB, saved });
        // At q = 3: this frame moves to work pixel 5, where the saved displacement is 2/8 = 4 work pixels.
        expect(Near(out.at(3, 3).r, 2.0f + 4.0f), "Compose: vectors over both frames add, across a render-size change");
        expect(Near(out.at(W - 1, 3).r, 2.0f), "Compose: off-screen history keeps this frame's vector");
    }

    // Fill: pixels whose moved detail was dropped take the trusted detail of neighbours on the same surface.
    {
        const Px floorDetail { 0.05f, -0.03f, 0.02f, 1 };
        const Px bodyDetail { -0.2f, 0.2f, -0.2f, 1 };
        // Background (depth 0.1, detail D, trusted) except a revealed strip x = 6..9 with nothing trusted; a nearer
        // surface (depth 0.5, detail E, trusted) at x < 3.
        const auto fillDepth = Fill([](unsigned x, unsigned) { return Grey(x < 3 ? 0.5f : 0.1f); });
        const auto estimate = Fill([&](unsigned x, unsigned) {
            if (x >= 6 && x <= 9)
                return Px { 0, 0, 0, 0 };
            return x < 3 ? bodyDetail : floorDetail;
        });
        DlssNrDetailReuseConstants c = Base(DlssNrDetailReuse_Fill);
        c.FillStrength = 1.0f;
        c.FillRadius = 6.0f;
        auto out = gpu.Run(c, { grey, estimate, grey, grey, fillDepth });
        expect(Near3(out.at(8, 3), Plus(Grey(0.5f), floorDetail), 1e-3f),
               "Fill: a revealed strip takes the detail of the same surface around it");
        expect(Near3(out.at(6, 3), Plus(Grey(0.5f), floorDetail), 1e-3f),
               "Fill: taps on a nearer surface are ignored");
        expect(Near3(out.at(12, 3), Plus(Grey(0.5f), floorDetail)), "Fill: trusted pixels keep their own detail");
        expect(Near3(out.at(1, 3), Plus(Grey(0.5f), bodyDetail)), "Fill: the nearer surface keeps its own detail");

        c.FillStrength = 0.0f;
        out = gpu.Run(c, { grey, estimate, grey, grey, fillDepth });
        expect(Near3(out.at(8, 3), Grey(0.5f)), "Fill: strength 0 leaves dropped pixels as the input");

        const auto nothingTrusted = Fill([](unsigned, unsigned) { return Px { 0.1f, 0.1f, 0.1f, 0 }; });
        c.FillStrength = 1.0f;
        out = gpu.Run(c, { grey, nothingTrusted, grey, grey, fillDepth });
        expect(Near3(out.at(8, 3), Grey(0.5f)), "Fill: no trusted neighbour leaves the input");

        auto broken = estimate;
        broken.px[3 * W + 11] = { std::numeric_limits<float>::quiet_NaN(), 0, 0, 1 };
        out = gpu.Run(c, { grey, broken, grey, grey, fillDepth });
        bool finite = true;
        for (const Px& px : out.px)
            finite = finite && Finite(px);
        expect(finite, "Fill: NaN in a neighbour never reaches the output");

        c.DebugView = 1;
        out = gpu.Run(c, { grey, estimate, grey, grey, fillDepth });
        expect(Near3(out.at(8, 3), { 0, 1, 1, 1 }, 1e-3f), "Fill: debug view marks filled detail cyan");
        out = gpu.Run(c, { grey, nothingTrusted, grey, grey, fillDepth });
        expect(Near3(out.at(8, 3), { 1, 0, 1, 1 }, 1e-3f), "Fill: debug view marks unfilled dropped detail magenta");
    }

    // Fill with partial trust: a half-trusted pixel keeps half its own detail and takes the other half from its
    // neighbours; weakly trusted neighbours (weight sum below 2) fade the fill in.
    {
        const Px floorDetail { 0.05f, -0.03f, 0.02f, 1 };
        const Px ownDetail { -0.2f, 0.2f, -0.2f, 0.5f };
        const auto flat = Fill([](unsigned, unsigned) { return Grey(0.1f); });
        const auto halfTrusted = Fill([&](unsigned x, unsigned y) { return x == 8 && y == 3 ? ownDetail : floorDetail; });
        DlssNrDetailReuseConstants c = Base(DlssNrDetailReuse_Fill);
        c.FillStrength = 1.0f;
        c.FillRadius = 6.0f;
        auto out = gpu.Run(c, { grey, halfTrusted, grey, grey, flat });
        const Px expected { 0.5f + 0.5f * ownDetail.r + 0.5f * floorDetail.r, 0.5f + 0.5f * ownDetail.g + 0.5f * floorDetail.g,
                            0.5f + 0.5f * ownDetail.b + 0.5f * floorDetail.b, 1 };
        expect(Near3(out.at(8, 3), expected, 1e-3f), "Fill: half trust keeps half its own detail, fills the rest");

        // 16 taps at trust 0.1: weight sum 1.6, so the fill is 0.8 of the neighbours' detail.
        const auto weak = Fill([&](unsigned x, unsigned y) {
            return x == 8 && y == 3 ? Px { 0, 0, 0, 0 } : Px { floorDetail.r, floorDetail.g, floorDetail.b, 0.1f };
        });
        out = gpu.Run(c, { grey, weak, grey, grey, flat });
        const Px faded { 0.5f + 0.8f * floorDetail.r, 0.5f + 0.8f * floorDetail.g, 0.5f + 0.8f * floorDetail.b, 1 };
        expect(Near3(out.at(8, 3), faded, 1e-3f), "Fill: weakly trusted neighbours fade the fill in");
    }

    // Saved detail marked invalid (a = 0, a frame that could not be saved there): dropped, not moved.
    {
        const auto invalid = Fill([](unsigned x, unsigned y) { Px d = Detail(x, y); d.a = 0; return d; });
        const auto out = gpu.Run(Base(DlssNrDetailReuse_Reproject), { grey, invalid, savedColourDepth, still, depth });
        expect(Near3(out.at(5, 3), Grey(0.5f)), "Reproject: invalid saved detail (a = 0) is dropped");
    }

    // Depth guide at half size inside a padded texture (base 3, 1): the padding is another surface and must not be
    // read, for the reprojection or the capture.
    {
        const auto padded = Fill([](unsigned x, unsigned y) {
            const bool inside = x >= 3 && x < 3 + W / 2 && y >= 1 && y < 1 + H / 2;
            return Grey(inside ? 0.3f : 0.9f);
        }, W / 2 + 5, H / 2 + 3);
        DlssNrDetailReuseConstants c = Base(DlssNrDetailReuse_Reproject);
        c.DepthWidth = W / 2;
        c.DepthHeight = H / 2;
        c.DepthBaseX = 3;
        c.DepthBaseY = 1;
        const auto out = gpu.Run(c, { grey, detail, savedColourDepth, still, padded });
        bool ok = true;
        for (unsigned y = 0; y < H; ++y)
            for (unsigned x = 0; x < W; ++x)
                ok = ok && Near3(out.at(x, y), withDetail(grey, x, y));
        expect(ok, "Reproject: a padded, half-size depth guide is read inside its subrect");

        c.Mode = DlssNrDetailReuse_Capture;
        const auto answer = Fill([](unsigned x, unsigned y) { return Plus(Grey(0.5f), Detail(x, y)); });
        Img colourDepth;
        gpu.Run(c, { grey, answer, padded }, &colourDepth);
        bool saved = true;
        for (const Px& px : colourDepth.px)
            saved = saved && Near(px.a, 0.3f);
        expect(saved, "Capture: a padded, half-size depth guide is read inside its subrect");
    }

    // Compose with the motion at half size inside a padded texture (base 3, 1) and a game scale of 0.5: raw 2 is one
    // motion pixel (1/8 of the width). The saved displacement is 0.25 of the width. Back in raw units:
    // (1/8 + 1/4) * 8 / 0.5 = 6.
    {
        const auto padded = Fill([](unsigned x, unsigned y) {
            const bool inside = x >= 3 && x < 3 + W / 2 && y >= 1 && y < 1 + H / 2;
            return inside ? Px { 2, 0, 0, 0 } : Px { 100, 100, 0, 0 };
        }, W / 2 + 5, H / 2 + 3);
        const auto savedQuarter = Fill([](unsigned, unsigned) { return Px { 0.25f, 0, 0, 0 }; });
        DlssNrDetailReuseConstants c = Base(DlssNrDetailReuse_Compose);
        c.MotionWidth = W / 2;
        c.MotionHeight = H / 2;
        c.MotionBaseX = 3;
        c.MotionBaseY = 1;
        c.MvScaleX = c.MvScaleY = 0.5f;
        const auto out = gpu.Run(c, { padded, padded, padded, padded, savedQuarter }, nullptr, W / 2, H / 2);
        expect(Near(out.at(0, 0).r, 6.0f) && Near(out.at(0, 0).g, 0.0f),
               "Compose: motion subrect base, half size and game scale are honoured");
        // q = 7: 7.5/8 + 1/8 is past the right edge; the saved displacement is not read.
        expect(Near(out.at(W / 2 - 1, 2).r, 2.0f), "Compose: history from off-screen keeps this frame's raw vector");
    }

    // Replace modes (Neutwo + replace, balanced + replace): the answer IS the picture, decoded through the curve's
    // inverse, which runs away near white. A change is saved as a difference of proxy values (as in every mode) and
    // moved; at an edge it lands a pixel off. On a highlight that difference decodes to up to hundreds of times the pixel
    // and the resolve's guard shows it at 2x -- the white flashes. Replace modes land a moved change both as the
    // difference and as the ratio it made at its source, and keep the one that changes the pixel less.
    //
    // Each case: last frame's picture had `from` everywhere (saved colour), the model answered `from * factor`; this
    // frame the pixel at x = 5 shows `to`, with an edge at x = 5 so its colour box still trusts the moved change a little.
    // Returns what the resolve gets there, relative to `to`; `before` gets what v0.1.24 landed with the same trust.
    const auto landOnEdge = [&](uint32_t curve, float from, float factor, float to, float& before)
    {
        DlssNrDetailReuseConstants capture = Base(DlssNrDetailReuse_Capture);
        capture.ReplaceCurve = curve;
        Img savedColour;
        const auto change =
            gpu.Run(capture, { GreyImg(Encode(from, curve)), GreyImg(Encode(from * factor, curve)), depth }, &savedColour);
        const auto input = Fill([&](unsigned x, unsigned) { return Grey(Encode(x < 5 ? from : to, curve)); });
        DlssNrDetailReuseConstants reproject = Base(DlssNrDetailReuse_Reproject);
        reproject.ReplaceCurve = curve;
        const auto out = gpu.Run(reproject, { input, change, savedColour, still, depth });
        const auto estimate = gpu.Run(Base(DlssNrDetailReuse_Estimate), { input, change, savedColour, still, depth });
        const float trust = estimate.at(5, 3).a;
        expect(trust > 0.2f, "Replace: the edge's moved change is trusted, so the edge cases test the landing");
        before = Decode(input.at(5, 3).r + change.at(5, 3).r * trust, curve) / to;
        return Decode(out.at(5, 3).r, curve) / to;
    };

    for (const uint32_t curve : { (uint32_t) DlssNrReplaceCurve_Neutwo, (uint32_t) DlssNrReplaceCurve_Hybrid })
    {
        const char* name = curve == DlssNrReplaceCurve_Neutwo ? "Neutwo" : "balanced";
        const float guard = 2.0f; // the resolve's guard against the frame (MaxRatio's default)
        float before = 0.0f;

        // A mid grey's +5% landing on a highlight: about +5% there, not a flash.
        float now = landOnEdge(curve, 0.5f, 1.05f, 6.0f, before);
        std::printf("Replace %s: a mid grey's +5%% on a highlight: %.3fx (v0.1.24: %.3fx after the guard, %.1fx before)\n",
                    name, now, std::min(before, guard), before);
        expect(now >= 0.98f && now <= 1.1f, "Replace: a mid grey's change on a highlight stays about its size");

        // A dark pixel halved, landing on the lit floor (a shadow's edge): as v0.1.24 landed it -- not half the floor.
        now = landOnEdge(curve, 0.02f, 0.5f, 0.5f, before);
        std::printf("Replace %s: a dark pixel halved, on the floor: %.3fx (v0.1.24: %.3fx)\n", name, now, before);
        expect(std::abs(now / before - 1.0f) < 0.01f, "Replace: a dark pixel's change on the floor lands as in v0.1.24");

        // A highlight's +20% landing on the floor beside it (a reflection's edge): as v0.1.24 landed it -- small.
        now = landOnEdge(curve, 6.0f, 1.2f, 0.5f, before);
        std::printf("Replace %s: a highlight's +20%% on the floor: %.3fx (v0.1.24: %.3fx)\n", name, now, before);
        expect(std::abs(now / before - 1.0f) < 0.01f, "Replace: a highlight's change on the floor lands as in v0.1.24");

        // A blue jersey's change (brighter red and green, a little less blue) landing on a pale surface beside it: the
        // difference, as in v0.1.24. The ratio it made on the blue is near-black red and green tripled -- a colour
        // shift the brightest channel alone does not see.
        {
            DlssNrDetailReuseConstants capture = Base(DlssNrDetailReuse_Capture);
            capture.ReplaceCurve = curve;
            const Px blue { 0.0f, 0.0f, 0.6f, 1 }, blueAnswer { 0.05f, 0.05f, 0.59f, 1 };
            Img savedColour;
            const auto change = gpu.Run(capture, { Fill([&](unsigned, unsigned) { return blue; }),
                                                   Fill([&](unsigned, unsigned) { return blueAnswer; }), depth },
                                        &savedColour);
            const Px pale = EncodeRgb({ 0.1f, 0.2f, 0.9f, 1 }, curve);
            const auto input = Fill([&](unsigned x, unsigned) { return x < 5 ? blue : pale; });
            DlssNrDetailReuseConstants reproject = Base(DlssNrDetailReuse_Reproject);
            reproject.ReplaceCurve = curve;
            const auto out = gpu.Run(reproject, { input, change, savedColour, still, depth });
            const auto estimate =
                gpu.Run(Base(DlssNrDetailReuse_Estimate), { input, change, savedColour, still, depth });
            const float trust = estimate.at(5, 3).a;
            const Px d = change.at(5, 3);
            const Px asBefore { pale.r + d.r * trust, pale.g + d.g * trust, pale.b + d.b * trust, 1 };
            const Px light = DecodeRgb(pale, curve);
            const float nowStops = WorstStops(light, DecodeRgb(out.at(5, 3), curve));
            const float beforeStops = WorstStops(light, DecodeRgb(asBefore, curve));
            std::printf("Replace %s: a blue's change on a pale surface: %.3f stops (v0.1.24: %.3f), trust %.2f\n", name,
                        nowStops, beforeStops, trust);
            expect(trust > 0.2f, "Replace: the colour edge's moved change is trusted");
            expect(nowStops <= beforeStops + 1e-3f,
                   "Replace: a colour's change shifts no channel more than v0.1.24 did");
        }

        // The Estimate that Steady and Fill take is the same landed change Reproject adds.
        {
            DlssNrDetailReuseConstants capture = Base(DlssNrDetailReuse_Capture);
            capture.ReplaceCurve = curve;
            Img savedColour;
            const auto change =
                gpu.Run(capture, { GreyImg(Encode(0.5f, curve)), GreyImg(Encode(0.525f, curve)), depth }, &savedColour);
            const auto input = Fill([&](unsigned x, unsigned) { return Grey(Encode(x < 5 ? 0.5f : 6.0f, curve)); });
            DlssNrDetailReuseConstants reproject = Base(DlssNrDetailReuse_Reproject);
            reproject.ReplaceCurve = curve;
            DlssNrDetailReuseConstants estimateMode = reproject;
            estimateMode.Mode = DlssNrDetailReuse_Estimate;
            const auto out = gpu.Run(reproject, { input, change, savedColour, still, depth });
            const auto estimate = gpu.Run(estimateMode, { input, change, savedColour, still, depth });
            const Px e = estimate.at(5, 3);
            expect(Near(input.at(5, 3).r + e.r * e.a, out.at(5, 3).r, 1e-6f),
                   "Replace: Estimate gives Steady and Fill the landed change");
        }

        // When the nearer surface's motion wins, the change is landed against the picture it came from there, not the
        // one at the pixel's own motion. Pixel (5, 3) is far (0.1); its own motion (+4) finds a mid grey on the near
        // surface (depth 0.3, so it is dropped); the nearer surface's motion (-2) finds the highlight the change was made
        // on (+2%). Landed against the highlight it is +2%; against the grey, the ratio there would be about none.
        {
            const float highlight = 6.0f;
            const auto savedColour = Fill([&](unsigned x, unsigned) {
                const float v = Encode(x <= 4 ? highlight : 0.5f, curve);
                return Px { v, v, v, 0.3f };
            });
            const float d = Encode(highlight * 1.02f, curve) - Encode(highlight, curve);
            const auto change = Fill([&](unsigned, unsigned) { return Px { d, d, d, 1 }; });
            const auto motion = Fill([](unsigned x, unsigned) { return Px { x == 5 ? 4.0f : -2.0f, 0, 0, 0 }; });
            const auto depths = Fill([](unsigned x, unsigned y) { return Grey(x == 5 && y == 3 ? 0.1f : 0.3f); });
            DlssNrDetailReuseConstants reproject = Base(DlssNrDetailReuse_Reproject);
            reproject.ReplaceCurve = curve;
            const auto out =
                gpu.Run(reproject, { GreyImg(Encode(highlight, curve)), change, savedColour, motion, depths });
            const float landed = Decode(out.at(5, 3).r, curve) / highlight;
            std::printf("Replace %s: the nearer surface's change, landed on its own picture: %.4fx\n", name, landed);
            expect(std::abs(landed - 1.02f) < 2e-3f,
                   "Replace: the nearer surface's change lands against the picture it came from");
        }

        // On the surface it came from, the change lands whole, from near black to a highlight.
        bool sameSurface = true;
        for (const float light : { 0.01f, 0.2f, 0.5f, 2.0f, 6.0f })
        {
            DlssNrDetailReuseConstants capture = Base(DlssNrDetailReuse_Capture);
            capture.ReplaceCurve = curve;
            Img savedColour;
            const auto change = gpu.Run(
                capture, { GreyImg(Encode(light, curve)), GreyImg(Encode(light * 1.05f, curve)), depth }, &savedColour);
            DlssNrDetailReuseConstants reproject = Base(DlssNrDetailReuse_Reproject);
            reproject.ReplaceCurve = curve;
            const auto out = gpu.Run(reproject, { GreyImg(Encode(light, curve)), change, savedColour, still, depth });
            sameSurface = sameSurface && std::abs(Decode(out.at(5, 3).r, curve) / (light * 1.05f) - 1.0f) < 2e-3f;
        }
        expect(sameSurface, "Replace: on its own surface a change lands whole (0.01 to 6x white)");

        // Dropped (another surface was there): the input exactly.
        {
            DlssNrDetailReuseConstants reproject = Base(DlssNrDetailReuse_Reproject);
            reproject.ReplaceCurve = curve;
            const auto input = GreyImg(Encode(6.0f, curve));
            const auto farther = Fill([&](unsigned, unsigned) {
                const float v = Encode(0.5f, curve);
                return Px { v, v, v, 0.15f };
            });
            const auto change = Fill([&](unsigned, unsigned) { return Grey(0.01f); });
            const auto out = gpu.Run(reproject, { input, change, farther, still, depth });
            expect(out.at(5, 3).r == input.at(5, 3).r, "Replace: a dropped change leaves the input exactly");
        }

        // Fill: a dropped highlight among mid-grey neighbours whose change was +5% takes about +5%, not a flash; among
        // neighbours on its own brightness it takes their change whole.
        {
            DlssNrDetailReuseConstants fill = Base(DlssNrDetailReuse_Fill);
            fill.ReplaceCurve = curve;
            fill.FillStrength = 1.0f;
            fill.FillRadius = 6.0f;
            const auto input = Fill([&](unsigned x, unsigned y) { return Grey(Encode(x == 8 && y == 3 ? 6.0f : 0.5f, curve)); });
            const float midChange = Encode(0.5f * 1.05f, curve) - Encode(0.5f, curve);
            const auto holed = Fill([&](unsigned x, unsigned y) {
                return x == 8 && y == 3 ? Px { 0, 0, 0, 0 } : Px { midChange, midChange, midChange, 1 };
            });
            auto out = gpu.Run(fill, { input, holed, grey, grey, depth });
            now = Decode(out.at(8, 3).r, curve) / 6.0f;
            const float oldFill = Decode(input.at(8, 3).r + midChange, curve) / 6.0f;
            std::printf("Replace %s: Fill of a highlight among mid greys +5%%: %.3fx (v0.1.24: %.1fx before the guard)\n",
                        name, now, oldFill);
            expect(now >= 0.98f && now <= 1.1f, "Replace: Fill does not flash a dropped highlight");

            const float lit = 2.0f;
            const auto litInput = GreyImg(Encode(lit, curve));
            const float litChange = Encode(lit * 1.2f, curve) - Encode(lit, curve);
            const auto litHoled = Fill([&](unsigned x, unsigned y) {
                return x == 8 && y == 3 ? Px { 0, 0, 0, 0 } : Px { litChange, litChange, litChange, 1 };
            });
            out = gpu.Run(fill, { litInput, litHoled, grey, grey, depth });
            expect(std::abs(Decode(out.at(8, 3).r, curve) / lit - 1.2f) < 2e-3f,
                   "Replace: Fill among its own brightness takes the neighbours' change whole");
        }
    }

    // Coverage: the share of the frame with no detail to move, summed per tile over every second pixel. Checked
    // against a reference that walks the same pixels the shader does, on an estimate with an untrusted strip down the
    // left (what running brings in from off-screen), a half-trusted band and a NaN tile.
    {
        constexpr unsigned cw = 128, ch = 64;
        const unsigned tiles = kDlssNrDetailReuseCoverageTiles;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const auto trustAt = [&](unsigned x, unsigned y) -> float
        {
            if (x >= 40 && x < 44 && y < 4)
                return nan; // not finite: no detail at all, as Fill reads it
            if (x < 30)
                return 0.0f; // came in from off-screen
            if (y >= 32 && y < 40)
                return 0.5f; // half trusted
            return 1.0f;
        };
        const auto estimate = Fill([&](unsigned x, unsigned y) { return Px { 0.01f, 0.01f, 0.01f, trustAt(x, y) }; },
                                   cw, ch);

        DlssNrDetailReuseConstants coverage = Base(DlssNrDetailReuse_Coverage);
        coverage.WorkWidth = cw;
        coverage.WorkHeight = ch;
        const unsigned threads = tiles * 8; // one 8x8 thread group per tile
        const Img grid = gpu.Run(coverage, { estimate, estimate, estimate, estimate, estimate }, nullptr, threads,
                                 threads);

        double wantDropped = 0.0, wantPixels = 0.0;
        bool perTile = true;
        for (unsigned ty = 0; ty < tiles; ++ty)
            for (unsigned tx = 0; tx < tiles; ++tx)
            {
                const unsigned x0 = (tx * cw) / tiles, y0 = (ty * ch) / tiles;
                const unsigned x1 = ((tx + 1) * cw) / tiles, y1 = ((ty + 1) * ch) / tiles;
                double dropped = 0.0, pixels = 0.0;
                for (unsigned ly = 0; ly < 8; ++ly)
                    for (unsigned lx = 0; lx < 8; ++lx)
                        for (unsigned y = y0 + ly * 2; y < y1; y += 16)
                            for (unsigned x = x0 + lx * 2; x < x1; x += 16)
                            {
                                const float t = trustAt(x, y);
                                dropped += 1.0 - (std::isfinite(t) ? std::clamp(t, 0.0f, 1.0f) : 0.0f);
                                pixels += 1.0;
                            }
                const Px got = grid.at(tx, ty);
                if (!Near(got.r, (float) dropped, 1e-3f) || !Near(got.g, (float) pixels, 1e-3f))
                    perTile = false;
                wantDropped += dropped;
                wantPixels += pixels;
            }
        expect(perTile, "Coverage: a tile's dropped and measured sums differ from the reference");
        expect(wantPixels == (double) (cw / 2) * (ch / 2), "Coverage: not every second pixel was measured exactly once");

        double gotDropped = 0.0, gotPixels = 0.0;
        for (unsigned y = 0; y < tiles; ++y)
            for (unsigned x = 0; x < tiles; ++x)
            {
                gotDropped += grid.at(x, y).r;
                gotPixels += grid.at(x, y).g;
            }
        const double share = gotPixels > 0.0 ? gotDropped / gotPixels : -1.0;
        std::printf("Coverage: %.1f%% of the frame had no detail to move (%.0f of %.0f pixels measured)\n",
                    100.0 * share, gotDropped, gotPixels);
        expect(Near((float) gotPixels, (float) wantPixels, 1e-3f) && Near((float) gotDropped, (float) wantDropped, 1e-2f),
               "Coverage: the frame's totals differ from the reference");
        // The strip is 30 of 128 columns, the half-trusted band 8 of 64 rows of the rest, and the NaN tile 4x4 pixels.
        expect(share > 0.2 && share < 0.35, "Coverage: the measured share is not the strip plus the band");

        // A tile count in the shader that differs from the host's is caught by the per-tile and total checks above:
        // the reference walks the tiles the host's constant describes, so a wider or narrower grid in the shader gives
        // different sums. (A check for texels outside the grid cannot catch it: the dispatch is derived from the same
        // host constant, so nothing is ever written there whatever the shader believes.)
    }

    // Coverage on tiles large enough that every lane of a group works and the strided loops wrap: 512x512 gives 16x16
    // pixel tiles, so all 64 lanes contribute and the reduction's upper half carries data. Also an estimate whose rgb
    // is not finite while its alpha is: no detail at all, as Fill reads it.
    {
        constexpr unsigned cw = 512, ch = 512;
        const unsigned tiles = kDlssNrDetailReuseCoverageTiles;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const auto badRgb = [&](unsigned x, unsigned y) { return x >= 64 && x < 96 && y >= 64 && y < 96; };
        const auto trustAt = [&](unsigned x, unsigned y) -> float { return x < 100 ? 0.25f : 1.0f; };
        const auto estimate = Fill(
            [&](unsigned x, unsigned y)
            {
                const float t = trustAt(x, y);
                return badRgb(x, y) ? Px { nan, 0.01f, 0.01f, t } : Px { 0.01f, 0.01f, 0.01f, t };
            },
            cw, ch);

        DlssNrDetailReuseConstants coverage = Base(DlssNrDetailReuse_Coverage);
        coverage.WorkWidth = cw;
        coverage.WorkHeight = ch;
        const unsigned threads = tiles * 8;
        const Img grid = gpu.Run(coverage, { estimate, estimate, estimate, estimate, estimate }, nullptr, threads,
                                 threads);

        double wantDropped = 0.0, wantPixels = 0.0, gotDropped = 0.0, gotPixels = 0.0;
        bool perTile = true;
        for (unsigned ty = 0; ty < tiles; ++ty)
            for (unsigned tx = 0; tx < tiles; ++tx)
            {
                const unsigned x0 = (tx * cw) / tiles, x1 = ((tx + 1) * cw) / tiles;
                const unsigned y0 = (ty * ch) / tiles, y1 = ((ty + 1) * ch) / tiles;
                double dropped = 0.0, pixels = 0.0;
                for (unsigned ly = 0; ly < 8; ++ly)
                    for (unsigned lx = 0; lx < 8; ++lx)
                        for (unsigned y = y0 + ly * 2; y < y1; y += 16)
                            for (unsigned x = x0 + lx * 2; x < x1; x += 16)
                            {
                                dropped += badRgb(x, y) ? 1.0 : 1.0 - trustAt(x, y);
                                pixels += 1.0;
                            }
                const Px got = grid.at(tx, ty);
                if (!Near(got.r, (float) dropped, 1e-2f) || !Near(got.g, (float) pixels, 1e-3f))
                    perTile = false;
                wantDropped += dropped;
                wantPixels += pixels;
                gotDropped += got.r;
                gotPixels += got.g;
            }
        std::printf("Coverage at 512x512 (16x16 tiles): %.1f%% dropped over %.0f pixels\n",
                    100.0 * gotDropped / std::max(gotPixels, 1.0), gotPixels);
        expect(perTile, "Coverage: a 16x16 tile's sums differ from the reference (every lane, both strided loops)");
        expect(wantPixels == (double) (cw / 2) * (ch / 2) && Near((float) gotPixels, (float) wantPixels, 1e-3f),
               "Coverage: not every second pixel of a 16x16 tile was measured exactly once");
        expect(Near((float) gotDropped, (float) wantDropped, 0.5f),
               "Coverage: the totals differ from the reference at 16x16 tiles");
        // The untrusted quarter is 100 of 512 columns at trust 0.25, plus a 32x32 patch of non-finite rgb.
        expect(gotDropped > 0.0 && gotPixels > 0.0, "Coverage: nothing was measured at 512x512");
    }

    // Coverage on a frame narrower than the grid: the tiles that fall outside it are empty, and no column is counted
    // twice (an overlap would bias the share).
    {
        constexpr unsigned cw = 20, ch = 20;
        const unsigned tiles = kDlssNrDetailReuseCoverageTiles;
        const auto estimate = Fill([&](unsigned, unsigned) { return Px { 0.01f, 0.01f, 0.01f, 0.0f }; }, cw, ch);
        DlssNrDetailReuseConstants coverage = Base(DlssNrDetailReuse_Coverage);
        coverage.WorkWidth = cw;
        coverage.WorkHeight = ch;
        const Img grid = gpu.Run(coverage, { estimate, estimate, estimate, estimate, estimate }, nullptr, tiles * 8,
                                 tiles * 8);
        double pixels = 0.0;
        for (unsigned y = 0; y < tiles; ++y)
            for (unsigned x = 0; x < tiles; ++x)
                pixels += grid.at(x, y).g;
        // Tiles this small are a pixel wide or empty, so the stride measures every pixel rather than every second one.
        // What must hold is that no pixel is measured twice: count the visits the tiling makes.
        std::vector<int> visits(cw * ch, 0);
        for (unsigned ty = 0; ty < tiles; ++ty)
            for (unsigned tx = 0; tx < tiles; ++tx)
            {
                const unsigned x0 = std::min((tx * cw) / tiles, cw), x1 = std::min(((tx + 1) * cw) / tiles, cw);
                const unsigned y0 = std::min((ty * ch) / tiles, ch), y1 = std::min(((ty + 1) * ch) / tiles, ch);
                for (unsigned ly = 0; ly < 8; ++ly)
                    for (unsigned lx = 0; lx < 8; ++lx)
                        for (unsigned y = y0 + ly * 2; y < y1; y += 16)
                            for (unsigned x = x0 + lx * 2; x < x1; x += 16)
                                ++visits[y * cw + x];
            }
        const int worst = *std::max_element(visits.begin(), visits.end());
        const double counted = (double) std::count_if(visits.begin(), visits.end(), [](int v) { return v > 0; });
        std::printf("Coverage at 20x20 (tiles smaller than a pixel): %.0f measured, worst visited %dx\n", pixels,
                    worst);
        expect(worst <= 1, "Coverage: a frame narrower than the grid measures a pixel more than once");
        expect(Near((float) pixels, (float) counted, 1e-3f) && pixels > 0.0,
               "Coverage: a frame narrower than the grid measures a different set of pixels than the tiling covers");
    }

    if (gpu.reference)
    {
        std::printf("Reference: %d runs outside the Replace modes compared, %d differ\n", gpu.identityRuns,
                    gpu.identityDiffs);
        expect(gpu.identityRuns > 0 && gpu.identityDiffs == 0,
               "Outside the Replace modes the output is bit-identical to the reference shader");
    }
    else
        std::puts("Reference: SKIPPED (no reference shader given; outside the Replace modes nothing was compared)");

    if (fails == 0)
        std::puts("PASS: nr_detail_reuse_shader_smoke (WARP HLSL)");
    return fails == 0 ? 0 : 1;
}
catch (const std::exception& e)
{
    std::printf("ERROR: %s\n", e.what());
    return 2;
}
