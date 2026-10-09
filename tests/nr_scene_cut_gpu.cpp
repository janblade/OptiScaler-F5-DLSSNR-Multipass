// GPU test for motion/SceneCut_Dx12.cpp, the scene-cut detector, no game: synthetic pictures (sky and ground, or a
// textured scene, value noise) played as short sequences through the detector, the way DLSS-NR's game input feeds it
// (DetectColor: its own small luma from the colour first) and the way the optical flow does (Detect on a luma):
//   - a hard cut is found on the frame it happens, and on no other;
//   - fades, exposure steps, a fast pan, a one-frame flash of the whole picture and a bright disc over the middle (an
//     explosion) are not cuts; a frame that turns white is reported (it is one to the detector);
//   - the 1x1 distrust is 1 exactly where the flag is;
//   - scRGB colour (RGBA16F) behaves like SDR; Reset() and a change of the luma's shape start over without a cut,
//     and the same scene at another resolution is not a cut;
//   - the time of a frame at 3840x2160, and with the debug layer installed, the runtime reports no error or warning.
//
//   vcvars64, then from the repo root:
//   cl /std:c++20 /EHsc /O2 tests\nr_scene_cut_gpu.cpp OptiScaler\motion\SceneCut_Dx12.cpp d3d12.lib dxgi.lib

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include "../OptiScaler/motion/SceneCut_Dx12.h"

using Microsoft::WRL::ComPtr;

namespace
{
int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

constexpr float kThreshold = 0.45f; // the flow's default (OpticalFlowDx12::Settings::sceneCutThreshold)

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;
    HANDLE event = nullptr;
    ComPtr<ID3D12InfoQueue> info;
    std::vector<ComPtr<ID3D12Resource>> keep; // uploads and pictures until the list has run

    void Submit()
    {
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++value);
        fence->SetEventOnCompletion(value, event);
        WaitForSingleObject(event, 10000);
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
        keep.clear();
    }
};

bool MakeGpu(Gpu& g)
{
    ComPtr<ID3D12Debug> debug;

    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device))))
        return false;

    g.device.As(&g.info);

    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g.queue));
    g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator));
    g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator.Get(), nullptr, IID_PPV_ARGS(&g.list));
    g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
    g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return g.queue != nullptr && g.list != nullptr && g.fence != nullptr;
}

void Barrier(Gpu& g, ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    g.list->ResourceBarrier(1, &b);
}

ComPtr<ID3D12Resource> Buffer(Gpu& g, D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer));
    return buffer;
}

uint16_t Half(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int exponent = (int) ((x >> 23) & 0xff) - 127 + 15;
    const uint32_t mantissa = x & 0x7fffffu;

    if (exponent <= 0)
        return (uint16_t) sign;
    if (exponent >= 31)
        return (uint16_t) (sign | 0x7c00u);
    return (uint16_t) (sign | ((uint32_t) exponent << 10) | (mantissa >> 13));
}

// A texture with `pixels` (RGB per pixel, linear 0..) uploaded, left in NON_PIXEL_SHADER_RESOURCE. RGBA8_UNORM,
// RGBA16_FLOAT, or R32_FLOAT (then `pixels` holds one value per pixel).
ComPtr<ID3D12Resource> Upload(Gpu& g, UINT width, UINT height, DXGI_FORMAT format, const std::vector<float>& pixels)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> tex;
    g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                      IID_PPV_ARGS(&tex));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 total = 0;
    g.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    auto upload = Buffer(g, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);

    uint8_t* data = nullptr;
    upload->Map(0, nullptr, (void**) &data);

    for (UINT y = 0; y < height; ++y)
    {
        uint8_t* row = data + footprint.Offset + (size_t) y * footprint.Footprint.RowPitch;

        for (UINT x = 0; x < width; ++x)
        {
            const size_t i = (size_t) y * width + x;

            if (format == DXGI_FORMAT_R32_FLOAT)
                std::memcpy(row + 4 * x, &pixels[i], 4);
            else if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
            {
                const uint16_t h[4] = { Half(pixels[3 * i]), Half(pixels[3 * i + 1]), Half(pixels[3 * i + 2]),
                                        Half(1.0f) };
                std::memcpy(row + 8 * x, h, 8);
            }
            else
                for (int c = 0; c < 4; ++c)
                    row[4 * x + c] =
                        (uint8_t) std::lround(std::clamp(c < 3 ? pixels[3 * i + c] : 1.0f, 0.0f, 1.0f) * 255.0f);
        }
    }

    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = tex.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(g, tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    g.keep.push_back(upload);
    g.keep.push_back(tex);
    return tex;
}

// Smooth value noise in 0..1, a few octaves.
float Hash(int x, int y, uint32_t seed)
{
    uint32_t h = (uint32_t) x * 374761393u + (uint32_t) y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float) ((h ^ (h >> 16)) & 0xffffff) / 16777215.0f;
}

float Noise(float x, float y, uint32_t seed)
{
    float sum = 0.0f, weight = 0.0f, amplitude = 1.0f, scale = 1.0f / 64.0f;

    for (int octave = 0; octave < 4; ++octave, amplitude *= 0.5f, scale *= 2.0f)
    {
        const float fx = x * scale, fy = y * scale;
        const int ix = (int) std::floor(fx), iy = (int) std::floor(fy);
        const float tx = fx - ix, ty = fy - iy;
        const float sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
        const uint32_t s = seed + (uint32_t) octave * 101u;
        const float a = Hash(ix, iy, s), b = Hash(ix + 1, iy, s), c = Hash(ix, iy + 1, s), d = Hash(ix + 1, iy + 1, s);
        sum += amplitude * ((a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy);
        weight += amplitude;
    }

    return sum / weight;
}

// One frame of a scene: what is where, where the camera is, how bright it is, and what is laid over it.
struct Frame
{
    int layout = 0;           // 0 sky above ground, 1 sky below (upside down), 2 a textured scene
    float panX = 0, panY = 0; // in pixels of a 1280-wide picture
    float gain = 1.0f;        // the whole picture's brightness
    float disc = 0.0f;        // a bright disc over the middle, this bright (0 = none), radius an eighth of the height
    bool white = false;       // the whole frame white
};

std::vector<float> Picture(UINT width, UINT height, const Frame& f)
{
    std::vector<float> out((size_t) width * height * 3);
    const float scale = 1280.0f / (float) width; // the same scene at any size

    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x)
        {
            const float px = x * scale + f.panX, py = y * scale + f.panY;
            const float v = (float) y / (float) height;
            const float n = Noise(px, py, f.layout == 2 ? 7u : 3u);
            float r, gr, b;

            if (f.layout == 2)
            {
                r = 0.05f + 0.8f * n;
                gr = 0.05f + 0.7f * Noise(px + 500.0f, py, 11u);
                b = 0.05f + 0.6f * Noise(px, py + 500.0f, 13u);
            }
            else
            {
                const bool sky = f.layout == 0 ? v < 0.5f : v >= 0.5f;
                const float t = f.layout == 0 ? v : 1.0f - v;
                r = sky ? 0.55f + 0.2f * t + 0.05f * n : 0.08f + 0.3f * n;
                gr = sky ? 0.65f + 0.2f * t + 0.05f * n : 0.1f + 0.25f * n;
                b = sky ? 0.85f + 0.1f * t + 0.05f * n : 0.06f + 0.15f * n;
            }

            float add = 0.0f;
            if (f.disc > 0.0f)
            {
                const float dx = (float) x - width * 0.5f, dy = (float) y - height * 0.5f;
                if (dx * dx + dy * dy < (height / 8.0f) * (height / 8.0f))
                    add = f.disc;
            }

            const size_t i = ((size_t) y * width + x) * 3;
            out[i] = f.white ? 1.0f : r * f.gain + add;
            out[i + 1] = f.white ? 1.0f : gr * f.gain + add;
            out[i + 2] = f.white ? 1.0f : b * f.gain + add;
        }

    return out;
}

struct Answer
{
    bool flag = false;
    float divergence = 0.0f;
    float distrust = 0.0f;
};

// Copies the flag and the distrust out once the list ran.
Answer Read(Gpu& g, SceneCutDx12& sc)
{
    auto readback = Buffer(g, D3D12_HEAP_TYPE_READBACK, 1024, D3D12_RESOURCE_STATE_COPY_DEST);

    struct Item
    {
        ID3D12Resource* resource;
        DXGI_FORMAT format;
        UINT width;
        UINT64 offset;
    };

    for (const Item& item : { Item { sc.Flag(), DXGI_FORMAT_R32_UINT, 2, 0 },
                              Item { sc.Distrust(), DXGI_FORMAT_R8_UNORM, 1, 512 } })
    {
        Barrier(g, item.resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src {};
        src.pResource = item.resource;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst {};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = item.offset;
        dst.PlacedFootprint.Footprint = { item.format, item.width, 1, 1, 256 };
        g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(g, item.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    g.Submit();

    Answer a;
    const uint8_t* data = nullptr;
    readback->Map(0, nullptr, (void**) &data);
    uint32_t words[2];
    std::memcpy(words, data, 8);
    a.flag = words[0] != 0;
    std::memcpy(&a.divergence, &words[1], 4);
    a.distrust = data[512] / 255.0f;
    readback->Unmap(0, nullptr);
    return a;
}

struct Source
{
    UINT width = 1280, height = 720;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    SceneCutDx12::Encoding encoding = SceneCutDx12::Encoding::Srgb;
    float hdrScale = 1.0f; // scRGB: the picture's values times this (1.0 = 80 nits)
};

// One frame through DetectColor; the answer read back.
Answer Feed(Gpu& g, SceneCutDx12& sc, const Source& s, const Frame& f)
{
    std::vector<float> pixels = Picture(s.width, s.height, f);

    for (float& v : pixels)
        v *= s.hdrScale;

    auto color = Upload(g, s.width, s.height, s.format, pixels);
    sc.DetectColor(g.list.Get(), color.Get(), s.format, s.encoding, 203.0f, kThreshold);
    return Read(g, sc);
}

struct Sequence
{
    const char* what;
    int frames;
    std::function<Frame(int)> frame;
    int cutAt; // the frame that must be a cut, -1 for none
    bool reportOnly = false;
};

bool distrustMatches = true;
double worstQuiet = 0.0, weakestCut = 1.0;

void Play(Gpu& g, SceneCutDx12& sc, const Source& s, const Sequence& seq)
{
    sc.Reset();
    bool ok = true;
    float largestQuiet = 0.0f, onCut = 0.0f;
    bool flaggedOnCut = false;

    for (int k = 0; k < seq.frames; ++k)
    {
        const Answer a = Feed(g, sc, s, seq.frame(k));
        distrustMatches = distrustMatches && (a.distrust == (a.flag ? 1.0f : 0.0f));

        if (k == seq.cutAt)
        {
            onCut = a.divergence;
            flaggedOnCut = a.flag;
        }
        else
        {
            largestQuiet = std::max(largestQuiet, a.divergence);
            ok = ok && !a.flag;
        }
    }

    ok = ok && (seq.cutAt < 0 || flaggedOnCut);
    char line[256];

    if (seq.cutAt >= 0)
    {
        std::snprintf(line, sizeof(line), "%-48s divergence on the cut %.3f, elsewhere at most %.3f%s", seq.what, onCut,
                      largestQuiet, seq.reportOnly ? (flaggedOnCut ? "  (reported: flagged)" : "  (reported: not flagged)") : "");
        if (!seq.reportOnly)
            weakestCut = std::min(weakestCut, (double) onCut);
    }
    else
    {
        std::snprintf(line, sizeof(line), "%-48s largest divergence %.3f", seq.what, largestQuiet);
        worstQuiet = std::max(worstQuiet, (double) largestQuiet);
    }

    if (seq.reportOnly)
        std::printf("      %s\n", line);
    else
        Check(ok, line);
}

} // namespace

int main()
{
    Gpu g;

    if (!MakeGpu(g))
    {
        std::printf("no D3D12 device\n");
        return 1;
    }

    SceneCutDx12 sc;

    if (!sc.Init(g.device.Get()))
    {
        std::printf("FAIL  Init: %s\n", sc.Error().c_str());
        return 1;
    }

    const Sequence sequences[] = {
        { "hard cut, sky above -> sky below", 6,
          [](int k) { return k < 4 ? Frame { 0, 3.0f * k } : Frame { 1, 3.0f * k }; }, 4 },
        { "hard cut, dark textured interior -> sky and ground", 6,
          [](int k) { return k < 4 ? Frame { 2, 3.0f * k, -2.0f * k, 0.3f } : Frame { 0, 3.0f * k }; }, 4 },
        // Two pictures whose brightness spreads overlap: near the threshold, as the flow test's "same make" cut.
        { "cut, textured scene -> sky and ground", 6,
          [](int k) { return k < 4 ? Frame { 2, 3.0f * k, -2.0f * k } : Frame { 0, 3.0f * k }; }, 4, true },
        { "fade, 5% a frame", 9, [](int k) { return Frame { 0, 3.0f * k, 0, std::pow(0.95f, (float) k) }; }, -1 },
        { "fade, 10% a frame (textured)", 9,
          [](int k) { return Frame { 2, 3.0f * k, 0, std::pow(0.9f, (float) k) }; }, -1 },
        { "exposure step to twice the brightness", 8,
          [](int k) { return Frame { 0, 3.0f * k, 0, k < 4 ? 0.4f : 0.8f }; }, -1 },
        { "exposure step to half the brightness", 8,
          [](int k) { return Frame { 2, 3.0f * k, 0, k < 4 ? 0.8f : 0.4f }; }, -1 },
        { "fast pan, 160 px a frame (textured)", 6, [](int k) { return Frame { 2, 160.0f * k, -40.0f * k }; }, -1 },
        { "fast pan, 160 px a frame (sky and ground)", 6, [](int k) { return Frame { 0, 160.0f * k }; }, -1 },
        { "flash: the whole picture 2.5x for one frame", 7,
          [](int k) { return Frame { 0, 3.0f * k, 0, k == 3 ? 0.75f : 0.3f }; }, -1 },
        { "explosion: a bright disc over the middle, 2 frames", 8,
          [](int k) { return Frame { 2, 3.0f * k, 0, 1.0f, k == 3 || k == 4 ? 0.9f : 0.0f }; }, -1 },
        { "a frame that turns white", 6,
          [](int k)
          {
              Frame f { 0, 3.0f * k };
              f.white = k == 4;
              return f;
          },
          4, true },
    };

    std::printf("scene-cut detector, DetectColor, 1280x720 RGBA8 (SDR), threshold %.2f\n", kThreshold);
    for (const Sequence& seq : sequences)
        Play(g, sc, Source {}, seq);

    // scRGB: the same scenes as linear HDR (white at 203 nits = 2.54), shrunk 3x from 1920x1080.
    std::printf("DetectColor, 1920x1080 RGBA16F (scRGB)\n");
    const Source hdr { 1920, 1080, DXGI_FORMAT_R16G16B16A16_FLOAT, SceneCutDx12::Encoding::ScRgb, 2.54f };
    for (const int i : { 0, 1, 3, 5, 7 }) // the two cuts, a fade, an exposure step, a fast pan
        Play(g, sc, hdr, sequences[i]);

    // Reset: two different scenes with a Reset between them are not a cut; nor is the first frame after a size change.
    {
        sc.Reset();
        Feed(g, sc, Source {}, Frame { 0 });
        sc.Reset();
        const Answer afterReset = Feed(g, sc, Source {}, Frame { 1 });
        Check(!afterReset.flag && afterReset.distrust == 0.0f, "after Reset(): another scene is not a cut");

        // 1280x720 and 1920x1080 both shrink to a 640x360 luma, which is compared as usual: the same scene is no cut.
        Feed(g, sc, Source {}, Frame { 0 });
        const Answer sameScene = Feed(g, sc, Source { 1920, 1080 }, Frame { 0 });
        Check(!sameScene.flag, "the same scene at another resolution is not a cut");

        // A luma of another shape (1280x1024 -> 640x512) is not comparable: the first frame only stores.
        const Answer reshaped = Feed(g, sc, Source { 1280, 1024 }, Frame { 1 });
        Check(!reshaped.flag, "the first frame at a new luma shape is not a cut");
        const Answer next = Feed(g, sc, Source { 1280, 1024 }, Frame { 0 });
        Check(next.flag, "... and the detector works at that shape (sky below -> sky above is a cut)");
    }

    // Detect on a luma the caller made (as the flow does): a cut is found, a still frame is not.
    {
        auto luma = [&](const Frame& f)
        {
            const UINT w = 640, h = 360;
            const std::vector<float> rgb = Picture(w, h, f);
            std::vector<float> l((size_t) w * h);
            for (size_t i = 0; i < l.size(); ++i)
                l[i] = 0.299f * rgb[3 * i] + 0.587f * rgb[3 * i + 1] + 0.114f * rgb[3 * i + 2];
            return Upload(g, w, h, DXGI_FORMAT_R32_FLOAT, l);
        };

        sc.Reset();
        auto a = luma(Frame { 0 });
        sc.Detect(g.list.Get(), a.Get(), kThreshold);
        Read(g, sc);
        auto b = luma(Frame { 0, 3.0f });
        sc.Detect(g.list.Get(), b.Get(), kThreshold);
        const Answer still = Read(g, sc);
        auto c = luma(Frame { 1, 6.0f });
        sc.Detect(g.list.Get(), c.Get(), kThreshold);
        const Answer cut = Read(g, sc);
        Check(!still.flag && cut.flag, "Detect on a luma: a small pan is not a cut, sky above -> sky below is");
    }

    Check(distrustMatches, "the 1x1 distrust is 1 on exactly the frames flagged");
    std::printf("      weakest cut %.3f, strongest non-cut %.3f (threshold %.2f)\n", weakestCut, worstQuiet, kThreshold);

    // GPU time of a 3840x2160 RGBA16F frame (luma + histograms + divergence), timestamps around DetectColor.
    {
        D3D12_QUERY_HEAP_DESC qd {};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = 2;
        ComPtr<ID3D12QueryHeap> heap;
        g.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&heap));
        UINT64 frequency = 0;
        g.queue->GetTimestampFrequency(&frequency);
        const UINT w = 3840, h = 2160;
        std::vector<float> pixels = Picture(w, h, Frame { 2 });
        auto color = Upload(g, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, pixels);
        g.Submit();
        double total = 0.0;
        const int runs = 20;

        for (int i = 0; i < runs + 2; ++i)
        {
            auto readback = Buffer(g, D3D12_HEAP_TYPE_READBACK, 16, D3D12_RESOURCE_STATE_COPY_DEST);
            g.list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            sc.DetectColor(g.list.Get(), color.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, SceneCutDx12::Encoding::ScRgb,
                           203.0f, kThreshold);
            g.list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            g.list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback.Get(), 0);
            g.keep.push_back(color);
            g.Submit();
            UINT64* ticks = nullptr;
            readback->Map(0, nullptr, (void**) &ticks);
            if (i >= 2)
                total += 1000.0 * (double) (ticks[1] - ticks[0]) / (double) frequency;
            readback->Unmap(0, nullptr);
        }

        std::printf("      3840x2160 RGBA16F: %.4f ms a frame (GPU timestamps, %d frames)\n", total / runs, runs);
    }

    // The debug layer's verdict on everything above.
    if (g.info != nullptr)
    {
        UINT64 bad = 0;
        const UINT64 count = g.info->GetNumStoredMessages();

        for (UINT64 i = 0; i < count; ++i)
        {
            SIZE_T size = 0;
            g.info->GetMessage(i, nullptr, &size);
            std::vector<char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            g.info->GetMessage(i, message, &size);

            if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            {
                ++bad;
                std::printf("      debug layer: %s\n", message->pDescription);
            }
        }

        Check(bad == 0, "the debug layer reports no error or warning");
    }
    else
        std::printf("      (no debug layer installed: not checked)\n");

    std::printf(g_failures == 0 ? "all passed\n" : "%d FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
