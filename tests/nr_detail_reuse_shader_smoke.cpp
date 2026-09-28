// Headless shader test: Windows D3D11 WARP executes precompile/dlssnr_detail_reuse.hlsl (reuse detail between frames).
// Checks each mode on small synthetic images: detail follows the motion (at the motion texture's own size, subrect and
// scale), dropped where depth or colour disagree, kept at still edges, the better of the pixel's own and the nearer
// surface's motion, no ringing, composed vectors across a render-size change (and with a subrect, half size and game
// scale), padded depth guides, invalid saved detail, fill (including partial trust), steadiness, NaN safety.
// cl /std:c++20 /EHsc /W4 /wd4324 tests/nr_detail_reuse_shader_smoke.cpp d3d11.lib d3dcompiler.lib
// nr_detail_reuse_shader_smoke.exe OptiScaler/shaders/dlssnr/precompile/dlssnr_detail_reuse.hlsl
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

    // Runs one mode over outW x outH threads; returns u0 (and u1 when asked), both outW x outH.
    Img Run(const DlssNrDetailReuseConstants& c, const std::vector<Img>& inputs, Img* second = nullptr,
            unsigned outW = W, unsigned outH = H)
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
        ctx->CSSetShader(shader.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 5, views);
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
        ctx->CSSetSamplers(0, 1, sampler.GetAddressOf());
        ctx->UpdateSubresource(constants.Get(), 0, nullptr, &c, 0, 0);
        ctx->Dispatch((outW + 7) / 8, (outH + 7) / 8, 1);
        ID3D11UnorderedAccessView* none[] = { nullptr, nullptr };
        ctx->CSSetUnorderedAccessViews(0, 2, none, nullptr);

        if (second != nullptr)
            *second = Img { outW, outH, Read(out1.Get(), outW, outH) };
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
    return c;
}

// A residual that varies per pixel, so moved detail is recognisable.
static Px Detail(unsigned x, unsigned y) { return { 0.01f * x, -0.02f * y, 0.005f * (x + y), 1 }; }
static Px Plus(Px in, Px d) { return { in.r + d.r, in.g + d.g, in.b + d.b, 1 }; }

int wmain(int argc, wchar_t** argv)
try
{
    if (argc != 2)
        throw std::runtime_error("Pass the dlssnr_detail_reuse.hlsl path");
    ComPtr<ID3DBlob> code, errors;
    const HRESULT compiled = D3DCompileFromFile(argv[1], nullptr, nullptr, "CSMain", "cs_5_0",
                                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors)
        std::fprintf(stderr, "%s", (char*) errors->GetBufferPointer());
    check(compiled);

    Gpu gpu;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &gpu.device,
                            nullptr, &gpu.ctx));
    check(gpu.device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &gpu.shader));
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

    if (fails == 0)
        std::puts("PASS: nr_detail_reuse_shader_smoke (WARP HLSL)");
    return fails == 0 ? 0 : 1;
}
catch (const std::exception& e)
{
    std::printf("ERROR: %s\n", e.what());
    return 2;
}
