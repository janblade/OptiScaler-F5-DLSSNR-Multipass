// Headless shader test: Windows D3D11 WARP runs the statistics pass behind "Tune for this scene" and "Measure detail"
// (precompile/dlssnr_detail_stats.hlsl) on known pictures, and the grid it writes goes through the same reduction the
// game uses (DlssNr_ExposureCalibrate.h ReduceGrid).
//
//   1. Every tile of the grid matches a CPU reference of the shader's own definitions: display-encoded luma
//      (luma / white point -> Neutwo -> sRGB), the 4-neighbour Laplacian, the band as the difference of the two
//      box-binomial blurs (bilinear taps, clamped at the edges), frame-to-frame change, the shoulder and floor shares of
//      the model's picture, every second pixel each way over a 64x64 grid of tiles -- with the frame smaller than its
//      textures and the model's picture at half size.
//   2. A flat, unchanging picture measures zero detail and zero change, and counts every measured pixel.
//   3. The band ignores single-pixel grain and sees texture at the model's scale; the raw measure sees the grain.
//   4. Change is zero between identical frames and grows with the difference.
//   5. The shoulder and floor shares are exactly the share of measured pixels above / below the thresholds.
//   6. The whole chain: ReduceGrid of the grid equals the pixel-weighted means over the frame.
//
// D3D12's pass only; Vulkan's copy (mode 1, VK_MODE) is not run here.
//
// Build: cl /nologo /std:c++20 /EHsc /W3 tests\nr_detail_stats_shader_smoke.cpp /link d3d11.lib d3dcompiler.lib
// Run:   nr_detail_stats_shader_smoke.exe <dlssnr_detail_stats.hlsl>
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
#include "../OptiScaler/shaders/dlssnr/DlssNr_ExposureCalibrate.h"
using Microsoft::WRL::ComPtr;
namespace Cal = DlssNrExposureCalibrate;

struct Pixel { float r, g, b, a; };

struct Image
{
    UINT w = 0, h = 0;
    std::vector<Pixel> px;
    Pixel& at(UINT x, UINT y) { return px[y * w + x]; }
    const Pixel& at(UINT x, UINT y) const { return px[y * w + x]; }
};

static Image Make(UINT w, UINT h, float v = 0.0f) { return {w, h, std::vector<Pixel>(w * h, Pixel {v, v, v, 1})}; }

static void check(HRESULT hr, int line = __builtin_LINE())
{
    if (FAILED(hr)) throw std::runtime_error("D3D call failed, line " + std::to_string(line));
}
static int fails = 0;
static void expect(bool ok, const std::string& label) { if (!ok) { std::printf("FAIL: %s\n", label.c_str()); ++fails; } }

// The shader's cbuffer: the first fields of DlssNrConstants.
struct Params
{
    uint32_t mode = 0;
    float whitePoint = 1.0f;
    uint32_t width = 0, height = 0;
    float shoulder = 0.9f, floor = 0.02f;
    uint32_t pad[2] = {};
};

struct Gpu
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11SamplerState> linearSampler;
    ComPtr<ID3D11ComputeShader> shader;

    explicit Gpu(const wchar_t* path)
    {
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx));
        // As the game's static sampler (DlssNr_Dx12.cpp): bilinear, clamped.
        D3D11_SAMPLER_DESC sampling {};
        sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampling.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampling, &linearSampler));

        ComPtr<ID3DBlob> code, errors;
        HRESULT hr = D3DCompileFromFile(path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSMain", "cs_5_0",
                                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (errors && FAILED(hr)) std::fprintf(stderr, "%s", (char*) errors->GetBufferPointer());
        check(hr);
        check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
    }

    ComPtr<ID3D11Texture2D> Texture(const Image* data, UINT w = 0, UINT h = 0)
    {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = data ? data->w : w; desc.Height = data ? data->h : h; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        D3D11_SUBRESOURCE_DATA init {data ? data->px.data() : nullptr, (UINT) (desc.Width * sizeof(Pixel)), 0};
        ComPtr<ID3D11Texture2D> texture;
        check(device->CreateTexture2D(&desc, data ? &init : nullptr, &texture));
        return texture;
    }

    Image Read(ID3D11Texture2D* texture)
    {
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&desc, nullptr, &staging));
        ctx->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        check(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        Image image {desc.Width, desc.Height, std::vector<Pixel>(desc.Width * desc.Height)};
        for (UINT y = 0; y < desc.Height; ++y)
            std::memcpy(&image.px[y * desc.Width], (char*) mapped.pData + y * mapped.RowPitch, desc.Width * sizeof(Pixel));
        ctx->Unmap(staging.Get(), 0);
        return image;
    }

    // One stats pass, as DlssNr_Dx12::DispatchDetailStats binds it: t0 output, t1 previous output, t2 input, t3 previous
    // input, t4 the model's picture, u0 the 128x64 grid; 64x64 groups.
    Image Stats(const Params& k, const Image& out, const Image& prevOut, const Image& in, const Image& prevIn,
                const Image& proxy)
    {
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth = sizeof(Params);
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init {&k, 0, 0};
        ComPtr<ID3D11Buffer> constants;
        check(device->CreateBuffer(&desc, &init, &constants));

        ComPtr<ID3D11Texture2D> textures[5] = {Texture(&out), Texture(&prevOut), Texture(&in), Texture(&prevIn), Texture(&proxy)};
        ComPtr<ID3D11ShaderResourceView> views[5];
        ID3D11ShaderResourceView* srvs[5] = {};
        for (int i = 0; i < 5; ++i)
        {
            check(device->CreateShaderResourceView(textures[i].Get(), nullptr, &views[i]));
            srvs[i] = views[i].Get();
        }
        auto grid = Texture(nullptr, Cal::kGridTiles * 2, Cal::kGridTiles);
        ComPtr<ID3D11UnorderedAccessView> uav;
        check(device->CreateUnorderedAccessView(grid.Get(), nullptr, &uav));

        ctx->CSSetShader(shader.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 5, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
        ctx->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
        ctx->CSSetSamplers(0, 1, linearSampler.GetAddressOf());
        ctx->Dispatch(Cal::kGridTiles, Cal::kGridTiles, 1);
        ID3D11ShaderResourceView* noSrvs[5] = {};
        ID3D11UnorderedAccessView* noUav = nullptr;
        ctx->CSSetShaderResources(0, 5, noSrvs);
        ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
        return Read(grid.Get());
    }
};

// ---- The CPU reference: the shader's definitions, in double. ----

static double Encode(double r, double g, double b, double whitePoint)
{
    const double luma = std::max(0.2126 * std::max(r, 0.0) + 0.7152 * std::max(g, 0.0) + 0.0722 * std::max(b, 0.0), 0.0);
    const double x = luma / std::max(whitePoint, 1e-6);
    double v = std::clamp(x / std::sqrt(x * x + 1.0), 0.0, 1.0);
    return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(std::max(v, 1e-8), 1.0 / 2.4) - 0.055;
}

static const Pixel& Clamped(const Image& im, int x, int y)
{
    return im.at((UINT) std::clamp(x, 0, (int) im.w - 1), (UINT) std::clamp(y, 0, (int) im.h - 1));
}

static double EncodedAt(const Image& im, int x, int y, double wp)
{
    const Pixel& p = Clamped(im, x, y);
    return Encode(p.r, p.g, p.b, wp);
}

// A bilinear tap `o` pixels from pixel (x, y)'s centre along each axis, clamped: every offset used is a half, so the
// tap is the plain mean of the two pixels either side on each axis.
struct Rgb { double r = 0, g = 0, b = 0; };
static Rgb Tap(const Image& im, int x, int y, double ox, double oy)
{
    const int x0 = (int) std::floor(x + ox), y0 = (int) std::floor(y + oy);
    const double fx = (x + ox) - x0, fy = (y + oy) - y0;
    Rgb s;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i)
        {
            const double w = (i ? fx : 1.0 - fx) * (j ? fy : 1.0 - fy);
            const Pixel& p = Clamped(im, x0 + i, y0 + j);
            s.r += w * p.r;
            s.g += w * p.g;
            s.b += w * p.b;
        }
    return s;
}

static double Band(const Image& im, int x, int y, double wp)
{
    static const double kSmall[4] = {-1.5, -0.5, 0.5, 1.5};
    static const double kLarge[6] = {-4.5, -2.5, -0.5, 0.5, 2.5, 4.5};
    Rgb a, b;
    for (double oy : kSmall)
        for (double ox : kSmall)
        {
            const Rgb t = Tap(im, x, y, ox, oy);
            a.r += t.r / 16; a.g += t.g / 16; a.b += t.b / 16;
        }
    for (double oy : kLarge)
        for (double ox : kLarge)
        {
            const Rgb t = Tap(im, x, y, ox, oy);
            b.r += t.r / 36; b.g += t.g / 36; b.b += t.b / 36;
        }
    return std::abs(Encode(a.r, a.g, a.b, wp) - Encode(b.r, b.g, b.b, wp));
}

struct Frames { Image out, prevOut, in, prevIn, proxy; };

// Per tile (x, y): the eight grid values, as the shader writes them.
struct TileRef { double v[8] = {}; };

static std::vector<TileRef> Reference(const Params& k, const Frames& f)
{
    std::vector<TileRef> tiles(Cal::kGridTiles * Cal::kGridTiles);
    const double wp = k.whitePoint;
    for (uint32_t gy = 0; gy < Cal::kGridTiles; ++gy)
        for (uint32_t gx = 0; gx < Cal::kGridTiles; ++gx)
        {
            const uint32_t x0 = gx * k.width / Cal::kGridTiles, x1 = std::max((gx + 1) * k.width / Cal::kGridTiles, x0 + 1);
            const uint32_t y0 = gy * k.height / Cal::kGridTiles, y1 = std::max((gy + 1) * k.height / Cal::kGridTiles, y0 + 1);
            double s[7] = {}, n = 0;
            for (uint32_t y = y0; y < y1; y += 2)
                for (uint32_t x = x0; x < x1; x += 2)
                {
                    const int px = (int) x, py = (int) y;
                    const double c = EncodedAt(f.out, px, py, wp);
                    s[0] += std::abs(4 * c - EncodedAt(f.out, px - 1, py, wp) - EncodedAt(f.out, px + 1, py, wp) -
                                     EncodedAt(f.out, px, py - 1, wp) - EncodedAt(f.out, px, py + 1, wp));
                    s[1] += Band(f.out, px, py, wp);
                    s[2] += Band(f.in, px, py, wp);
                    s[3] += std::abs(c - EncodedAt(f.prevOut, px, py, wp));
                    s[4] += std::abs(EncodedAt(f.in, px, py, wp) - EncodedAt(f.prevIn, px, py, wp));
                    // The model's picture at the frame position scaled to its size (float, as the shader).
                    const int qx = std::min((int) ((float(px) + 0.5f) * float(f.proxy.w) / float(std::max(k.width, 1u))), (int) f.proxy.w - 1);
                    const int qy = std::min((int) ((float(py) + 0.5f) * float(f.proxy.h) / float(std::max(k.height, 1u))), (int) f.proxy.h - 1);
                    const Pixel& q = f.proxy.at((UINT) qx, (UINT) qy);
                    const double peak = std::max({q.r, q.g, q.b});
                    s[5] += peak > k.shoulder ? 1 : 0;
                    s[6] += peak < k.floor ? 1 : 0;
                    ++n;
                }
            TileRef& t = tiles[gy * Cal::kGridTiles + gx];
            for (int i = 0; i < 7; ++i)
                t.v[i] = s[i] / std::max(n, 1.0);
            t.v[7] = n;
        }
    return tiles;
}

// The frame-wide means the reference implies (pixel-weighted, as ReduceGrid).
static Cal::Stats ReferenceStats(const std::vector<TileRef>& tiles)
{
    double s[7] = {}, n = 0;
    for (const TileRef& t : tiles)
    {
        for (int i = 0; i < 7; ++i)
            s[i] += t.v[i] * t.v[7];
        n += t.v[7];
    }
    Cal::Stats r;
    r.detailRaw = (float) (s[0] / n);
    r.detailBand = (float) (s[1] / n);
    r.inputBand = (float) (s[2] / n);
    r.outputChange = (float) (s[3] / n);
    r.inputChange = (float) (s[4] / n);
    r.shoulder = (float) (s[5] / n);
    r.floor = (float) (s[6] / n);
    return r;
}

// The largest difference between the grid and the reference, per value (0..7).
static std::vector<double> GridError(const Image& grid, const std::vector<TileRef>& ref)
{
    std::vector<double> worst(8, 0.0);
    for (uint32_t gy = 0; gy < Cal::kGridTiles; ++gy)
        for (uint32_t gx = 0; gx < Cal::kGridTiles; ++gx)
        {
            const Pixel& a = grid.at(gx, gy);
            const Pixel& b = grid.at(gx + Cal::kGridTiles, gy);
            const float got[8] = {a.r, a.g, a.b, a.a, b.r, b.g, b.b, b.a};
            const TileRef& t = ref[gy * Cal::kGridTiles + gx];
            for (int i = 0; i < 8; ++i)
                worst[i] = std::max(worst[i], std::isfinite(got[i]) ? std::abs(got[i] - t.v[i]) : 1e9);
        }
    return worst;
}

static Cal::Stats Reduce(const Image& grid) { return Cal::ReduceGrid(&grid.px[0].r); }

// ---- Pictures. ----

static uint32_t g_seed = 12345;
static float Noise() { g_seed = g_seed * 1664525u + 1013904223u; return ((g_seed >> 8) & 0xFFFF) / 65535.0f; }

// A scene in linear HDR around the white point: a gradient, an edge, grain and a weave, a little colour.
static Image Scene(UINT w, UINT h, float wp)
{
    Image im = Make(w, h);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x)
        {
            float v = 0.2f + 0.6f * x / w;
            if (x > w / 2 && y < h / 3) v = 2.5f;                                     // a bright block with an edge
            v *= 1.0f + 0.3f * std::sin(0.785f * x) * std::sin(0.785f * y + 0.4f);  // a weave, period 8
            v *= 1.0f + 0.2f * (Noise() - 0.5f);                                     // grain
            im.at(x, y) = {v * wp * 1.1f, v * wp, v * wp * 0.8f, 1};
        }
    return im;
}

int wmain(int argc, wchar_t** argv) try
{
    if (argc != 2)
        throw std::runtime_error("Pass the dlssnr_detail_stats.hlsl path");
    Gpu gpu(argv[1]);

    // 1. Every tile against the reference, twice: a 250x130 frame in 256x136 textures (the frame smaller than its
    //    resources, not a multiple of 64) with the model's picture at half size; and a 1210x1090 frame, whose tiles
    //    (18-19 x 17 pixels) are wider than a thread group's 16-pixel stride as a game's are, with the model's picture
    //    at 0.59x. A white point that is not 1.
    struct Size { UINT W, H, width, height, proxyW, proxyH; };
    for (const Size& z : {Size {256, 136, 250, 130, 128, 68}, Size {1216, 1096, 1210, 1090, 714, 643}})
    {
        const UINT W = z.W, H = z.H;
        Params k;
        k.whitePoint = 1.7f;
        k.width = z.width;
        k.height = z.height;
        std::printf("%ux%u frame, model picture %ux%u\n", k.width, k.height, z.proxyW, z.proxyH);
        Frames f;
        f.out = Scene(W, H, k.whitePoint);
        f.prevOut = f.out;
        for (Pixel& p : f.prevOut.px) { const float m = 1.0f + 0.05f * (Noise() - 0.5f); p.r *= m; p.g *= m; p.b *= m; }
        f.in = Scene(W, H, k.whitePoint);
        f.prevIn = f.in;
        for (Pixel& p : f.prevIn.px) { const float m = 1.0f + 0.01f * (Noise() - 0.5f); p.r *= m; p.g *= m; p.b *= m; }
        f.proxy = Make(z.proxyW, z.proxyH, 0.5f);
        for (UINT y = 0; y < f.proxy.h; ++y)
            for (UINT x = 0; x < f.proxy.w; ++x)
                f.proxy.at(x, y) = x < f.proxy.w / 4 ? Pixel {0.3f, 0.95f, 0.2f, 1}                 // green peak above
                                   : y < f.proxy.h / 10 ? Pixel {0.01f, 0.005f, 0.015f, 1}          // every channel below
                                                        : Pixel {0.5f, 0.4f, 0.3f, 1};

        const Image grid = gpu.Stats(k, f.out, f.prevOut, f.in, f.prevIn, f.proxy);
        const std::vector<TileRef> ref = Reference(k, f);
        const std::vector<double> err = GridError(grid, ref);
        static const char* names[8] = {"raw", "band out", "band in", "output change", "input change", "shoulder", "floor", "pixels"};
        std::printf("per-tile error vs the reference:");
        for (int i = 0; i < 8; ++i)
            std::printf(" %s %.1e%s", names[i], err[i], i < 7 ? "," : "\n");
        for (int i = 0; i < 5; ++i)
            expect(err[i] < 1e-4, std::string("tile ") + names[i] + " differs from the reference");
        expect(err[5] < 1e-6 && err[6] < 1e-6, "tile shoulder/floor shares differ from the reference");
        expect(err[7] == 0.0, "tile pixel counts differ from the reference");

        // 6. The chain: the reduction of the grid is the reference's frame-wide means.
        const Cal::Stats got = Reduce(grid), want = ReferenceStats(ref);
        std::printf("frame: raw %.5f band out %.5f in %.5f change out %.5f in %.5f shoulder %.4f floor %.4f\n", got.detailRaw,
                    got.detailBand, got.inputBand, got.outputChange, got.inputChange, got.shoulder, got.floor);
        const auto close = [](float a, float b) { return std::abs(a - b) <= 1e-5f + 1e-4f * std::abs(b); };
        expect(close(got.detailRaw, want.detailRaw) && close(got.detailBand, want.detailBand) &&
                   close(got.inputBand, want.inputBand) && close(got.outputChange, want.outputChange) &&
                   close(got.inputChange, want.inputChange) && close(got.shoulder, want.shoulder) && close(got.floor, want.floor),
               "ReduceGrid of the grid differs from the frame-wide reference");
        expect(got.detailRaw > 0 && got.detailBand > 0 && got.outputChange > got.inputChange,
               "the textured scene measures no detail, or the bigger change is not the bigger number");
        // 5. The shares are the share of measured pixels whose model picture is above / below the thresholds.
        expect(want.shoulder > 0.2 && want.shoulder < 0.3 && want.floor > 0.05 && want.floor < 0.1,
               "the reference shares are not near the quarter / tenth of the model's picture drawn");
    }

    // 2. Flat and unchanging: zero detail, zero change, every measured pixel counted, no shares.
    {
        const UINT W = 128, H = 128;
        Params k;
        k.width = W;
        k.height = H;
        const Image flat = Make(W, H, 0.5f);
        const Image grid = gpu.Stats(k, flat, flat, flat, flat, flat);
        const Cal::Stats s = Reduce(grid);
        double pixels = 0;
        for (UINT y = 0; y < Cal::kGridTiles; ++y)
            for (UINT x = 0; x < Cal::kGridTiles; ++x)
                pixels += grid.at(x + Cal::kGridTiles, y).a;
        std::printf("flat: raw %.1e band %.1e change %.1e pixels %.0f\n", s.detailRaw, s.detailBand, s.outputChange, pixels);
        expect(s.detailRaw < 1e-6f && s.detailBand < 1e-6f && s.inputBand < 1e-6f, "a flat picture measures detail");
        expect(s.outputChange == 0.0f && s.inputChange == 0.0f, "identical frames measure change");
        expect(s.shoulder == 0.0f && s.floor == 0.0f, "a mid-grey model picture counts as shoulder or floor");
        expect(pixels == (W / 2) * (H / 2), "not every second pixel each way was measured");
    }

    // 3. Single-pixel grain vs texture at the model's scale, the same amplitude.
    {
        const UINT W = 128, H = 128;
        Params k;
        k.width = W;
        k.height = H;
        Image checker = Make(W, H), weave = Make(W, H);
        for (UINT y = 0; y < H; ++y)
            for (UINT x = 0; x < W; ++x)
            {
                const float c = ((x + y) & 1) ? 0.6f : 0.4f;
                const float s = 0.5f + 0.1f * std::sin(0.785398f * x) * std::sin(0.785398f * y); // period 8
                checker.at(x, y) = {c, c, c, 1};
                weave.at(x, y) = {s, s, s, 1};
            }
        const Cal::Stats g = Reduce(gpu.Stats(k, checker, checker, checker, checker, checker));
        const Cal::Stats t = Reduce(gpu.Stats(k, weave, weave, weave, weave, weave));
        std::printf("grain: raw %.5f band %.6f | weave (period 8): raw %.5f band %.6f\n", g.detailRaw, g.detailBand,
                    t.detailRaw, t.detailBand);
        expect(g.detailBand < 0.1f * t.detailBand, "the band sees single-pixel grain");
        expect(g.detailRaw > 2.0f * t.detailRaw, "the raw measure does not favour single-pixel grain");
        expect(std::abs(g.detailBand - g.inputBand) < 1e-7f && std::abs(t.detailBand - t.inputBand) < 1e-7f,
               "the same picture as output and input measures a different band");
    }

    // 4. Change grows with the difference between frames.
    {
        const UINT W = 128, H = 128;
        Params k;
        k.width = W;
        k.height = H;
        const Image base = Scene(W, H, 1.0f);
        float last = 0.0f;
        for (float step : {0.0f, 0.02f, 0.05f, 0.1f})
        {
            Image moved = base;
            for (Pixel& p : moved.px) { p.r *= 1 + step; p.g *= 1 + step; p.b *= 1 + step; }
            const Cal::Stats s = Reduce(gpu.Stats(k, base, moved, base, base, base));
            expect(step == 0.0f ? s.outputChange == 0.0f : s.outputChange > last, "change does not grow with the difference");
            expect(s.inputChange == 0.0f, "an unchanged input measures change");
            last = s.outputChange;
        }
        std::printf("change at +10%% brightness: %.5f\n", last);
    }

    if (fails)
    {
        std::printf("%d check(s) failed\n", fails);
        return 1;
    }
    std::puts("PASS: the stats pass matches its reference tile by tile (frame smaller than its textures, tiles\n"
              "      wider than a group stride, model picture at 0.5x and 0.59x); flat is zero; the band ignores pixel grain and sees texture; change grows with the\n"
              "      difference; shares are exact; ReduceGrid gives the frame-wide means (WARP HLSL)");
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
