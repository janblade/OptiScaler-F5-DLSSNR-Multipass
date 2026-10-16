// GPU test for Optical F5Low's motion on kinds of game, no game: four synthetic scenes, each built like a kind of game
// the motion profiles are for, run through the real OpticalFlowDx12, TrustMaskDx12 and TrustMaskDx12::BuildGuides as
// the native producer calls them, and scored on the picture-size motion guide (what the model, Reuse detail and, on the
// native path, frame generation get) against each pixel's true motion, plus the trust mask against the true
// disocclusions.
//   cel        flat colour regions with dark outlines, flat-filled characters with outlines, no grain (anime, cel shaded)
//   realistic  dark, low-contrast texture with grain that changes every frame, textured characters (Resident Evil)
//   sportsWide striped grass, thin white pitch lines, a slow pan, about twenty small players each going its own way, a
//              ball, a still scoreboard (FIFA's broadcast camera)
//   sportsPan  a repetitive crowd over a wood floor, a fast pan left to right, tall players, a still score bar (NBA)
// Tallies per scene (share of pixels within 1 pixel of the truth / mean error in pixels):
//   objects  inside the characters, more than 3 pixels from an outline
//   edges    within 3 pixels of an outline between two layers (either side)
//   back     the rest of the background
//   hud      the still overlay (scenes that have one)
//   score    the mean of the shares above that the scene has (the number a profile is judged by)
// and the mask: "reveal" the share of just-uncovered pixels it distrusts (>= 0.5), "false" the share of the others
// (more than 3 pixels from an outline) it distrusts.
//
//   vcvars64, then from the repo root:
//   cl /std:c++20 /EHsc /O2 tests\nr_flow_genres_gpu.cpp OptiScaler\motion\OpticalFlow_Dx12.cpp
//   OptiScaler\motion\SceneCut_Dx12.cpp OptiScaler\motion\TrustMask_Dx12.cpp d3d12.lib dxgi.lib d3dcompiler.lib
//
// Arguments: key=value settings of the flow and the trust mask (see main), scene=<name> to run one scene, depth=0/1 to
// run only without / with depth, dump=<dir> to write each run's last frame there (picture, guide, truth, layer, mask). No
// arguments: every scene with and without depth under the default settings.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "../OptiScaler/motion/OpticalFlow_Dx12.h"
#include "../OptiScaler/motion/TrustMask_Dx12.h"

using Microsoft::WRL::ComPtr;

namespace
{

uint32_t kWidth = 1280;
uint32_t kHeight = 720;
std::string dumpDir; // dump=<dir>: the last frame of each run written there

float Hash(int x, int y, int seed)
{
    uint32_t h = (uint32_t) x * 374761393u + (uint32_t) y * 668265263u + (uint32_t) seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xFFFFFF) / 16777215.0f;
}

float ValueNoise(float x, float y, float scale, int seed)
{
    const float fx = x / scale, fy = y / scale;
    const int ix = (int) std::floor(fx), iy = (int) std::floor(fy);
    const float tx = fx - ix, ty = fy - iy;
    const float sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
    const float a = Hash(ix, iy, seed), b = Hash(ix + 1, iy, seed), c = Hash(ix, iy + 1, seed),
                d = Hash(ix + 1, iy + 1, seed);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

float Texture(float x, float y, int channel, int seed)
{
    return 0.45f * ValueNoise(x, y, 64, seed + channel) + 0.25f * ValueNoise(x, y, 24, seed + 20 + channel) +
           0.18f * ValueNoise(x, y, 9, seed + 40 + channel) + 0.12f * ValueNoise(x, y, 4, seed + 60 + channel);
}

float Half(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, mant = h & 1023;
    float v;
    if (exp == 0)
        v = std::ldexp((float) mant, -24);
    else if (exp == 31)
        v = INFINITY;
    else
        v = std::ldexp((float) (mant + 1024), (int) exp - 25);
    return sign ? -v : v;
}

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 fenceValue = 0;

    bool Init()
    {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;

        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
            {
                wprintf(L"adapter: %s\n", desc.Description);
                break;
            }
        }

        if (!device)
            return false;

        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return false;

        event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        return true;
    }

    void Submit()
    {
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++fenceValue);
        fence->SetEventOnCompletion(fenceValue, event);
        WaitForSingleObject(event, INFINITE);
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
    }

    ComPtr<ID3D12Resource> Buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r));
        return r;
    }

    // A texture filled by fill(x, y, pixel), recorded onto the list (no submit), left in the non-pixel shader resource
    // state. The upload buffer is kept in `keep` until the list has run.
    ComPtr<ID3D12Resource> Upload(DXGI_FORMAT format, uint32_t bytesPerPixel,
                                  const std::function<void(uint32_t, uint32_t, uint8_t*)>& fill,
                                  std::vector<ComPtr<ID3D12Resource>>& keep)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> tex;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&tex));

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto upload = Buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

        uint8_t* data = nullptr;
        upload->Map(0, nullptr, (void**) &data);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
                fill(x, y, data + (size_t) y * fp.Footprint.RowPitch + (size_t) x * bytesPerPixel);
        upload->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.pResource = tex.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &b);
        keep.push_back(upload);
        return tex;
    }

    // An RGBA16F texture in the given resting state as x, y pairs, row by row.
    std::vector<float> ReadMotion(ID3D12Resource* tex, D3D12_RESOURCE_STATES rest)
    {
        const auto desc = tex->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto readback = Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = rest;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);

        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = tex;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        Submit();

        std::vector<float> out((size_t) desc.Width * desc.Height * 2);
        uint8_t* data = nullptr;
        readback->Map(0, nullptr, (void**) &data);
        for (uint32_t y = 0; y < desc.Height; ++y)
            for (uint32_t x = 0; x < desc.Width; ++x)
            {
                const uint16_t* px = (const uint16_t*) (data + (size_t) y * fp.Footprint.RowPitch + x * 8);
                out[((size_t) y * desc.Width + x) * 2] = Half(px[0]);
                out[((size_t) y * desc.Width + x) * 2 + 1] = Half(px[1]);
            }
        readback->Unmap(0, nullptr);
        return out;
    }
};

// An R8_UNORM texture in the given resting state as values 0..1, row by row.
std::vector<float> ReadMask(Gpu& gpu, ID3D12Resource* tex, D3D12_RESOURCE_STATES rest)
{
    const auto desc = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 total = 0;
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto readback = gpu.Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = rest;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    gpu.list->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    gpu.list->ResourceBarrier(1, &b);
    gpu.Submit();

    std::vector<float> out((size_t) desc.Width * desc.Height);
    uint8_t* data = nullptr;
    readback->Map(0, nullptr, (void**) &data);
    for (uint32_t y = 0; y < desc.Height; ++y)
        for (uint32_t x = 0; x < desc.Width; ++x)
            out[(size_t) y * desc.Width + x] = data[(size_t) y * fp.Footprint.RowPitch + x] / 255.0f;
    readback->Unmap(0, nullptr);
    return out;
}

constexpr int kFrames = 10; // frames run; the last is scored (the flow's history and still age have settled)
constexpr int kHud = 100;   // the overlay's layer

enum Genre
{
    kCel,
    kRealistic,
    kSportsWide,
    kSportsPan,
    kGenres
};
const char* kGenreNames[kGenres] = { "cel", "realistic", "sportsWide", "sportsPan" };

// A character or a player: a disc (h = 0, w = radius) or an upright rectangle (w x h, x y its top left), where it is on
// the last frame and how far it moves each frame (picture pixels), and how nearness it is (depth's reciprocal scale).
struct Object
{
    float x, y, w, h;
    float mx, my;
    float nearness;
    int style;
};

struct Scene
{
    Genre genre;
    float camX, camY; // the background's motion per frame
    std::vector<Object> objects;
    bool hud = false;
    float hudX0 = 0, hudY0 = 0, hudX1 = 0, hudY1 = 0;
};

Scene MakeScene(Genre g)
{
    Scene s { g, 0, 0, {} };
    const float W = (float) kWidth, H = (float) kHeight;
    switch (g)
    {
    case kCel:
        s.camX = 5;
        s.camY = 1;
        s.objects = { { 400, 360, 70, 0, -4, 0, 6, 0 }, { 800, 300, 45, 0, 2, 3, 8, 1 }, { 950, 520, 30, 0, -6, -2, 10, 2 } };
        break;
    case kRealistic:
        s.camX = 3;
        s.camY = 0.5f;
        s.objects = { { 500, 380, 90, 0, -5, 1, 6, 0 }, { 900, 250, 40, 0, 8, 0, 9, 1 } };
        break;
    case kSportsWide:
        s.camX = 3;
        for (int i = 0; i < 20; ++i)
            s.objects.push_back({ 100 + 1060 * Hash(i, 1, 77), 150 + 480 * Hash(i, 2, 77), 8, 18,
                                  std::round((Hash(i, 3, 77) * 8 - 4) * 2) / 2, std::round((Hash(i, 4, 77) * 6 - 3) * 2) / 2,
                                  17, i & 1 });
        s.objects.push_back({ 700, 420, 3, 0, 6, -2, 16, 9 }); // the ball
        s.hud = true;
        s.hudX0 = 40, s.hudY0 = 30, s.hudX1 = 300, s.hudY1 = 74;
        break;
    case kSportsPan:
        s.camX = 16;
        for (int i = 0; i < 6; ++i)
            s.objects.push_back({ 120 + 180 * (float) i + 60 * Hash(i, 1, 91), 0.45f * H + 40 + 120 * Hash(i, 2, 91), 26,
                                  80, std::round(Hash(i, 3, 91) * 16 - 6), std::round(Hash(i, 4, 91) * 4 - 2), 8, i & 1 });
        s.hud = true;
        s.hudX0 = 200, s.hudY0 = H - 70, s.hudX1 = W - 200, s.hudY1 = H - 26;
        break;
    default:
        break;
    }
    return s;
}

// What one picture pixel shows on a frame: colour, nearness (reversed-Z depth), which layer (0 background, 1.. the
// objects in order, kHud the overlay) and that layer's motion per frame.
struct Px
{
    float rgb[3];
    float nearness;
    int layer;
    float mx, my;
};

void Mix(float* rgb, float r, float g, float b)
{
    rgb[0] = r;
    rgb[1] = g;
    rgb[2] = b;
}

int CelLevel(float wx, float wy)
{
    const float v = 0.6f * ValueNoise(wx, wy, 90, 11) + 0.4f * ValueNoise(wx, wy, 40, 12);
    return std::clamp((int) (v * 5.0f), 0, 4);
}

void Background(const Scene& s, int frame, float x, float y, Px& p)
{
    const float wx = x - s.camX * frame, wy = y - s.camY * frame;
    const float H = (float) kHeight;
    p.layer = 0;
    p.mx = s.camX;
    p.my = s.camY;
    p.nearness = 0.1f / 20.0f;

    switch (s.genre)
    {
    case kCel:
    {
        static const float pal[5][3] = { { 0.35f, 0.55f, 0.85f }, { 0.55f, 0.75f, 0.45f }, { 0.85f, 0.80f, 0.55f },
                                         { 0.75f, 0.45f, 0.40f }, { 0.92f, 0.92f, 0.88f } };
        const int l = CelLevel(wx, wy);
        const bool line = CelLevel(wx - 1.5f, wy) != l || CelLevel(wx + 1.5f, wy) != l || CelLevel(wx, wy - 1.5f) != l ||
                          CelLevel(wx, wy + 1.5f) != l;
        if (line)
            Mix(p.rgb, 0.06f, 0.05f, 0.08f);
        else
            Mix(p.rgb, pal[l][0], pal[l][1], pal[l][2]);
        break;
    }
    case kRealistic:
        for (int ch = 0; ch < 3; ++ch)
            p.rgb[ch] = 0.18f + 0.35f * (Texture(wx, wy, ch, 7) - 0.5f);
        break;
    case kSportsWide:
    {
        const float stripe = ((int) std::floor(wx / 60.0f) & 1) ? 0.035f : -0.035f;
        const float fine = 0.05f * (Texture(wx, wy, 0, 31) - 0.5f);
        Mix(p.rgb, 0.18f + stripe + fine, 0.42f + stripe + fine, 0.16f + stripe + fine);
        if (std::fabs(wx - 600) < 1.2f || std::fabs(wy - 150) < 1.2f || std::fabs(std::hypot(wx - 600, wy - 380) - 150) < 1.2f)
            Mix(p.rgb, 0.92f, 0.92f, 0.92f);
        break;
    }
    case kSportsPan:
        if (y < 0.45f * H)
        {
            // The crowd: a person every 14 x 18 pixels, shirts of many colours, dark gaps.
            const int cx = (int) std::floor(wx / 14.0f), cy = (int) std::floor(y / 18.0f);
            const float lx = wx - cx * 14.0f, ly = y - cy * 18.0f;
            p.nearness = 0.1f / 40.0f;
            Mix(p.rgb, 0.10f, 0.09f, 0.11f);
            if (std::hypot(lx - 7, ly - 5) < 3.0f)
                Mix(p.rgb, 0.70f, 0.52f, 0.40f);
            else if (lx >= 2 && lx < 12 && ly >= 9)
                Mix(p.rgb, 0.2f + 0.7f * Hash(cx, cy, 5), 0.2f + 0.7f * Hash(cx, cy, 6), 0.2f + 0.7f * Hash(cx, cy, 8));
        }
        else
        {
            // The floor: planks 22 pixels tall with seams, wood grain, a painted line; nearer towards the bottom.
            const int row = (int) std::floor(y / 22.0f);
            const float grain = 0.08f * (ValueNoise(wx, y * 4.0f, 40, 41 + row) - 0.5f);
            float shade = grain;
            if (std::fmod(y, 22.0f) < 1.0f || std::fmod(wx + row * 67.0f + 100000.0f, 180.0f) < 1.5f)
                shade -= 0.12f;
            Mix(p.rgb, 0.62f + shade, 0.42f + shade, 0.22f + shade);
            if (std::fabs(wx - 640) < 3.0f || (y >= 0.8f * H && y < 0.8f * H + 5))
                Mix(p.rgb, 0.93f, 0.93f, 0.90f);
            p.nearness = 0.1f / (30.0f - 20.0f * (y - 0.45f * H) / (0.55f * H));
        }
        break;
    default:
        break;
    }
}

// The object's own picture at (lx, ly) from its centre (discs) or top left (rectangles); false outside it.
bool ObjectAt(const Scene& s, const Object& o, float lx, float ly, float* rgb)
{
    if (o.h == 0)
    {
        const float r = std::hypot(lx, ly);
        if (r >= o.w)
            return false;
        switch (s.genre)
        {
        case kCel:
        {
            static const float fill[3][3] = { { 0.95f, 0.75f, 0.6f }, { 0.4f, 0.3f, 0.7f }, { 0.9f, 0.4f, 0.5f } };
            const float fx = lx - 0.3f * o.w, fy = ly + 0.25f * o.w, fr = std::hypot(fx, fy), face = 0.35f * o.w;
            if (o.w - r < 3.0f || std::fabs(fr - face) < 1.0f)
                Mix(rgb, 0.06f, 0.05f, 0.08f);
            else if (fr < face)
                Mix(rgb, 0.98f, 0.95f, 0.9f);
            else
                Mix(rgb, fill[o.style][0], fill[o.style][1], fill[o.style][2]);
            return true;
        }
        case kRealistic:
            for (int ch = 0; ch < 3; ++ch)
                rgb[ch] = 0.26f + 0.4f * (Texture(lx + 3000.0f, ly + 3000.0f, ch, 507 + o.style) - 0.5f);
            return true;
        default: // the ball
            Mix(rgb, 0.95f, 0.95f, 0.95f);
            return true;
        }
    }
    if (lx < 0 || ly < 0 || lx >= o.w || ly >= o.h)
        return false;
    const float headH = s.genre == kSportsPan ? 14.0f : 5.0f;
    if (ly < headH)
        Mix(rgb, 0.55f, 0.40f, 0.30f);
    else if (ly >= o.h - (s.genre == kSportsPan ? 28.0f : 5.0f))
        Mix(rgb, 0.12f, 0.12f, 0.14f);
    else if (o.style == 0)
        Mix(rgb, 0.80f, 0.12f, 0.10f);
    else
        Mix(rgb, 0.10f, 0.22f, 0.85f);
    if (s.genre == kSportsPan && ly >= headH + 10 && ly < headH + 22 && std::fabs(lx - o.w * 0.5f) < 5.0f)
        Mix(rgb, 0.95f, 0.95f, 0.95f); // the number
    return true;
}

Px Sample(const Scene& s, int frame, float x, float y)
{
    Px p {};
    Background(s, frame, x, y, p);
    const float back = (float) (kFrames - 1 - frame);

    for (size_t i = 0; i < s.objects.size(); ++i)
    {
        const Object& o = s.objects[i];
        const float ox = o.x - o.mx * back, oy = o.y - o.my * back;
        float rgb[3];
        if (ObjectAt(s, o, x - ox, y - oy, rgb))
        {
            Mix(p.rgb, rgb[0], rgb[1], rgb[2]);
            p.layer = (int) i + 1;
            p.mx = o.mx;
            p.my = o.my;
            p.nearness = 0.1f / o.nearness;
        }
    }

    if (s.hud && x >= s.hudX0 && x < s.hudX1 && y >= s.hudY0 && y < s.hudY1)
    {
        // A still overlay: dark with white blocks like text. It has no depth of its own (drawn after the scene).
        const float lx = x - s.hudX0 - 6, ly = y - s.hudY0 - 8;
        const bool text = lx >= 0 && ly >= 0 && ly < 28 && x < s.hudX1 - 6 &&
                          Hash((int) (lx / 9), (int) (ly / 14), 3) > 0.45f && std::fmod(lx, 9.0f) < 7.0f;
        if (text)
            Mix(p.rgb, 0.95f, 0.95f, 0.95f);
        else
            Mix(p.rgb, 0.08f, 0.08f, 0.10f);
        p.layer = kHud;
        p.mx = p.my = 0;
    }
    return p;
}

// Picture-size grain: changes every frame (film grain, dithering), screen fixed.
float Grain(const Scene& s, int frame, uint32_t x, uint32_t y, int ch)
{
    return s.genre == kRealistic ? 0.07f * (Hash((int) x, (int) y, frame * 7 + ch + 1000) - 0.5f) : 0.0f;
}

struct Tally
{
    double within = 0, error = 0;
    uint64_t n = 0;

    void Add(float ex, float ey)
    {
        const double e = std::hypot(ex, ey);
        within += e <= 1.0;
        error += e;
        ++n;
    }
    double Share() const { return n ? within / n : 0.0; }
    double Mean() const { return n ? error / n : 0.0; }
};

enum Band
{
    kObjects,
    kEdges,
    kBack,
    kHudBand,
    kBands
};
const char* kBandNames[kBands] = { "objects", "edges", "back", "hud" };

struct Result
{
    Tally band[kBands];
    uint64_t revealed = 0, revealedHit = 0, others = 0, othersHit = 0;

    double Score() const
    {
        double sum = 0;
        int n = 0;
        for (const Tally& t : band)
            if (t.n)
            {
                sum += t.Share();
                ++n;
            }
        return n ? sum / n : 0.0;
    }
    double Reveal() const { return revealed ? (double) revealedHit / revealed : 0.0; }
    double False() const { return others ? (double) othersHit / others : 0.0; }
};

// key=value settings for the flow (OpticalFlowDx12::Settings) and the trust mask (TrustMaskDx12::Settings); false for
// an unknown key.
bool SetKnob(const std::string& key, float v, OpticalFlowDx12::Settings& f, TrustMaskDx12::Settings& t)
{
    if (key == "radius") f.radius = (int) v;
    else if (key == "coarse") f.coarseRadius = (int) v;
    else if (key == "lambda") f.lambda = v;
    else if (key == "history") f.useHistory = v != 0;
    else if (key == "cells") f.coarseCells = (int) v;
    else if (key == "smooth") f.smoothRadius = (int) v;
    else if (key == "knee") f.confidenceKnee = v;
    else if (key == "dmatch") f.depthMatching = v != 0;
    else if (key == "global") f.globalCandidate = v != 0;
    else if (key == "inverse") f.inverseRefinement = v != 0;
    else if (key == "zero") f.zeroMargin = v;
    else if (key == "zeroreach") f.zeroReach = (int) v;
    else if (key == "still") f.stillFrames = (int) v;
    else if (key == "stilleps") f.stillEpsilon = v;
    else if (key == "stillmargin") f.stillMargin = v;
    else if (key == "look") f.lookWeights = v != 0;
    else if (key == "lookrange") f.lookRange = v;
    else if (key == "lookdist") f.lookDistance = v;
    else if (key == "depthtol") t.depthTolerance = v;
    else if (key == "flowtol") t.flowTolerance = v;
    else if (key == "lumatol") t.lumaTolerance = v;
    else if (key == "revealtol") t.revealTolerance = v;
    else if (key == "decay") t.decay = v;
    else if (key == "surface") t.guideSurface = v != 0;
    else if (key == "uncover") t.uncoverBackground = v != 0;
    else if (key == "reach") t.uncoverReach = v;
    else return false;
    return true;
}

Result Run(Gpu& gpu, const Scene& s, bool depthOn, const OpticalFlowDx12::Settings& fs, const TrustMaskDx12::Settings& ts)
{
    Result result;
    OpticalFlowDx12 flow;
    TrustMaskDx12 trust;

    if (!flow.Init(gpu.device.Get()) || !trust.Init(gpu.device.Get()))
    {
        printf("init failed: %s %s\n", flow.Error().c_str(), trust.Error().c_str());
        exit(1);
    }
    flow.Tuning() = fs;
    trust.Tuning() = ts;

    for (int frame = 0; frame < kFrames; ++frame)
    {
        std::vector<Px> px((size_t) kWidth * kHeight);
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
                px[(size_t) y * kWidth + x] = Sample(s, frame, x + 0.5f, y + 0.5f);

        std::vector<ComPtr<ID3D12Resource>> keep;
        auto colour = gpu.Upload(DXGI_FORMAT_R8G8B8A8_UNORM, 4,
                                 [&](uint32_t x, uint32_t y, uint8_t* out)
                                 {
                                     const Px& p = px[(size_t) y * kWidth + x];
                                     for (int ch = 0; ch < 3; ++ch)
                                         out[ch] = (uint8_t) std::clamp((p.rgb[ch] + Grain(s, frame, x, y, ch)) * 255.0f + 0.5f,
                                                                        0.0f, 255.0f);
                                     out[3] = 255;
                                 },
                                 keep);

        ComPtr<ID3D12Resource> depth;
        if (depthOn)
            depth = gpu.Upload(DXGI_FORMAT_R32_FLOAT, 4,
                               [&](uint32_t x, uint32_t y, uint8_t* out)
                               {
                                   // The overlay has no depth: what is under it shows through.
                                   Px p = px[(size_t) y * kWidth + x];
                                   if (p.layer == kHud)
                                   {
                                       Scene under = s;
                                       under.hud = false;
                                       p = Sample(under, frame, x + 0.5f, y + 0.5f);
                                   }
                                   memcpy(out, &p.nearness, 4);
                               },
                               keep);

        flow.Dispatch(gpu.list.Get(), colour.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, depth.Get(),
                      depthOn ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_UNKNOWN, true);

        bool built = false;
        if (flow.FlowValid())
        {
            TrustMaskDx12::Inputs in;
            in.flow = flow.Flow();
            in.flowWidth = flow.FlowWidth();
            in.flowHeight = flow.FlowHeight();
            in.fullPerFlow = (float) kWidth / (float) flow.FlowWidth();
            in.lumaNow = flow.LumaOfLastFrame();
            in.lumaBefore = flow.LumaOfFrameBefore();
            in.sceneCut = flow.SceneCutFlag();

            if (depthOn)
            {
                in.depths[0] = depth.Get();
                in.depthCount = 1;
                in.depthFormat = DXGI_FORMAT_R32_FLOAT;
                in.depthWidth = kWidth;
                in.depthHeight = kHeight;
                in.depthReversed = true;
            }

            if (trust.Dispatch(gpu.list.Get(), in))
                built = trust.BuildGuides(gpu.list.Get(), in, kWidth, kHeight);
        }

        gpu.Submit();

        if (frame < kFrames - 1)
            continue;

        if (!built)
        {
            printf("%s: no guide on the last frame\n", kGenreNames[s.genre]);
            exit(1);
        }

        const auto guide = gpu.ReadMotion(trust.GuideMotion(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const auto mask = ReadMask(gpu, trust.Mask(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        const uint32_t fw = flow.FlowWidth(), fh = flow.FlowHeight();

        if (!dumpDir.empty())
        {
            // The last frame for a look by eye: the picture (PPM), and per pixel the guide's x y, the truth's x y (the
            // flow's sign: minus the motion), the layer and the mask, as floats.
            const std::string base = dumpDir + "\\" + kGenreNames[s.genre] + (depthOn ? "_depth" : "_nodepth");
            if (FILE* f = nullptr; fopen_s(&f, (base + ".ppm").c_str(), "wb") == 0 && f)
            {
                fprintf(f, "P6\n%u %u\n255\n", kWidth, kHeight);
                for (uint32_t y = 0; y < kHeight; ++y)
                    for (uint32_t x = 0; x < kWidth; ++x)
                        for (int ch = 0; ch < 3; ++ch)
                            fputc((int) std::clamp((px[(size_t) y * kWidth + x].rgb[ch] + Grain(s, frame, x, y, ch)) *
                                                           255.0f + 0.5f,
                                                   0.0f, 255.0f),
                                  f);
                fclose(f);
            }
            if (FILE* f = nullptr; fopen_s(&f, (base + ".bin").c_str(), "wb") == 0 && f)
            {
                for (uint32_t y = 0; y < kHeight; ++y)
                    for (uint32_t x = 0; x < kWidth; ++x)
                    {
                        const Px& p = px[(size_t) y * kWidth + x];
                        const size_t i = ((size_t) y * kWidth + x) * 2;
                        const float v[6] = { guide[i], guide[i + 1], -p.mx, -p.my, (float) p.layer,
                                             mask[(size_t) (y * fh / kHeight) * fw + x * fw / kWidth] };
                        fwrite(v, sizeof(v), 1, f);
                    }
                fclose(f);
            }
        }

        auto layerAt = [&](int x, int y)
        {
            x = std::clamp(x, 0, (int) kWidth - 1);
            y = std::clamp(y, 0, (int) kHeight - 1);
            return px[(size_t) y * kWidth + x].layer;
        };

        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const Px& p = px[(size_t) y * kWidth + x];
                bool edge = false;
                for (int dy = -3; dy <= 3 && !edge; ++dy)
                    for (int dx = -3; dx <= 3 && !edge; ++dx)
                        edge = layerAt((int) x + dx, (int) y + dy) != p.layer;

                const int band = edge ? kEdges : p.layer == kHud ? kHudBand : p.layer == 0 ? kBack : kObjects;
                const size_t i = ((size_t) y * kWidth + x) * 2;
                // The flow points from here to where the content was: minus its motion.
                result.band[band].Add(guide[i] + p.mx, guide[i + 1] + p.my);

                // Uncovered: where this content was a frame ago, something else was on top.
                const float sx = x + 0.5f - p.mx, sy = y + 0.5f - p.my;
                if (sx < 0 || sy < 0 || sx >= kWidth || sy >= kHeight)
                    continue;
                const bool revealed = Sample(s, frame - 1, sx, sy).layer != p.layer;
                const float m = mask[(size_t) (y * fh / kHeight) * fw + x * fw / kWidth];
                if (revealed)
                {
                    ++result.revealed;
                    result.revealedHit += m >= 0.5f;
                }
                else if (!edge)
                {
                    ++result.others;
                    result.othersHit += m >= 0.5f;
                }
            }
    }

    return result;
}

} // namespace

int main(int argc, char** argv)
{
    Gpu gpu;
    if (!gpu.Init())
    {
        printf("no D3D12 device\n");
        return 2;
    }

    OpticalFlowDx12::Settings fs;
    TrustMaskDx12::Settings ts;
    bool onlyDepth = false, onlyNoDepth = false;
    int onlyScene = -1;
    std::string knobs;

    for (int a = 1; a < argc; ++a)
    {
        const std::string arg = argv[a];
        const auto eq = arg.find('=');
        const std::string key = arg.substr(0, eq);
        const std::string text = eq == std::string::npos ? "" : arg.substr(eq + 1);
        const float value = (float) atof(text.c_str());

        if (key == "depth")
            (value != 0 ? onlyDepth : onlyNoDepth) = true;
        else if (key == "dump")
            dumpDir = text;
        else if (key == "scene")
        {
            for (int g = 0; g < kGenres; ++g)
                if (text == kGenreNames[g])
                    onlyScene = g;
            if (onlyScene < 0)
            {
                printf("unknown scene %s\n", text.c_str());
                return 2;
            }
        }
        else if (SetKnob(key, value, fs, ts))
            knobs += " " + arg;
        else
        {
            printf("unknown argument %s\n", arg.c_str());
            return 2;
        }
    }

    printf("settings:%s\n", knobs.empty() ? " default" : knobs.c_str());
    for (int d = 1; d >= 0; --d)
    {
        if ((d && onlyNoDepth) || (!d && onlyDepth))
            continue;
        double total = 0;
        int n = 0;
        for (int g = 0; g < kGenres; ++g)
        {
            if (onlyScene >= 0 && g != onlyScene)
                continue;
            const Result r = Run(gpu, MakeScene((Genre) g), d != 0, fs, ts);
            printf("%-10s %-8s score %.4f |", kGenreNames[g], d ? "depth" : "no depth", r.Score());
            for (int b = 0; b < kBands; ++b)
                if (r.band[b].n)
                    printf(" %s %.3f/%.2f", kBandNames[b], r.band[b].Share(), r.band[b].Mean());
            printf(" | mask reveal %.3f false %.3f\n", r.Reveal(), r.False());
            total += r.Score();
            ++n;
        }
        if (n > 1)
            printf("TOTAL %-8s %.4f\n", d ? "depth" : "no depth", total / n);
    }
    return 0;
}
