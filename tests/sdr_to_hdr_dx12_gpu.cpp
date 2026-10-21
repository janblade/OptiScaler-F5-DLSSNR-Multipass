// GPU test for output/SdrToHdr_Dx12.cpp, the SDR-to-HDR pass of HDR output, no game and no swap chain. A proxy texture is
// filled with known 8-bit values and the pass writes it into an FP16 (scRGB) or R10G10B10A2 (PQ) texture; the read-back
// is compared with a CPU reference of the maths:
//   - gamma 2.2 and sRGB decode, game white at 203 and 80 nits, a peak clamp (400 nit white, 300 nit peak),
//   - Convert off (the Windows baseline: piecewise sRGB at the SDR white level) with 80 and 240 nits,
//   - R8G8B8A8_UNORM and B8G8R8A8_UNORM proxies (the channel order is the view's job),
//   - relaxed format casting, where the adapter has it: a B8G8R8A8_UNORM proxy drawn through an _SRGB render target view
//     holds the encoded values the pass reads raw,
//   - the same back buffer index run twice in a row (the fence wait), three indexes, Init for another format,
//   - no debug layer error or warning, and the device's reference count is the same after Release as before Init.
//
// Run on the hardware adapter or on WARP (argument "warp").
//
//   vcvars64, then from the repo root:
//   cl /std:c++20 /EHsc /O2 tests\sdr_to_hdr_dx12_gpu.cpp OptiScaler\output\SdrToHdr_Dx12.cpp d3d12.lib dxgi.lib
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
#include <vector>

#include "../OptiScaler/output/SdrToHdr_Dx12.h"

using Microsoft::WRL::ComPtr;

namespace
{
int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

constexpr UINT kWidth = 16, kHeight = 2;

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

    void Wait()
    {
        queue->Signal(fence.Get(), ++value);
        fence->SetEventOnCompletion(value, event);
        WaitForSingleObject(event, 10000);
    }

    void Submit()
    {
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        Wait();
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
    }
};

bool MakeGpu(Gpu& g, bool warp)
{
    ComPtr<ID3D12Debug> debug;

    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();

    ComPtr<IDXGIAdapter> adapter;

    if (warp)
    {
        ComPtr<IDXGIFactory4> factory;

        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
            FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))))
            return false;
    }

    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device))))
        return false;

    g.device.As(&g.info);

    // The relaxed casting case clears a proxy that has no optimized clear value, as a test's own draw: a performance note
    // about those clears, not about the pass
    if (g.info != nullptr)
    {
        D3D12_MESSAGE_ID hide[] = { D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE };
        D3D12_INFO_QUEUE_FILTER filter {};
        filter.DenyList.NumIDs = 1;
        filter.DenyList.pIDList = hide;
        g.info->AddStorageFilterEntries(&filter);
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g.queue));
    g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator));
    g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator.Get(), nullptr, IID_PPV_ARGS(&g.list));
    g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
    g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return g.queue != nullptr && g.list != nullptr && g.fence != nullptr;
}

int DebugMessages(Gpu& g)
{
    if (g.info == nullptr)
        return 0;

    int count = 0;

    for (UINT64 i = 0; i < g.info->GetNumStoredMessages(); ++i)
    {
        SIZE_T size = 0;
        g.info->GetMessage(i, nullptr, &size);
        auto* message = (D3D12_MESSAGE*) malloc(size);

        if (message != nullptr && SUCCEEDED(g.info->GetMessage(i, message, &size)) &&
            (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
             message->Severity == D3D12_MESSAGE_SEVERITY_WARNING))
        {
            std::printf("      debug layer: %s\n", message->pDescription);
            ++count;
        }

        free(message);
    }

    g.info->ClearStoredMessages();
    return count;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

// A back buffer as a swap chain makes one: a render target resting in PRESENT (COMMON)
ComPtr<ID3D12Resource> BackBuffer(Gpu& g, DXGI_FORMAT format)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> texture;
    g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                      IID_PPV_ARGS(&texture));
    return texture;
}

ComPtr<ID3D12Resource> Buffer(Gpu& g, D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer));
    return buffer;
}

D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint(Gpu& g, ID3D12Resource* texture, UINT64& total)
{
    D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    g.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    return footprint;
}

struct Rgb
{
    uint8_t r, g, b;
};

// Known values: grey steps (0, 0.25, 0.5, 0.735, 1) and colours
std::vector<Rgb> Picture()
{
    const Rgb list[] = { { 0, 0, 0 },     { 64, 64, 64 },  { 128, 128, 128 }, { 187, 187, 187 }, { 255, 255, 255 },
                         { 255, 0, 0 },   { 0, 255, 0 },   { 0, 0, 255 },     { 204, 102, 26 },  { 51, 153, 230 },
                         { 10, 200, 90 }, { 255, 255, 0 }, { 128, 0, 255 },   { 250, 250, 250 }, { 5, 5, 5 },
                         { 32, 32, 32 } };
    std::vector<Rgb> picture;

    for (UINT y = 0; y < kHeight; ++y)
        for (UINT x = 0; x < kWidth; ++x)
            picture.push_back(list[(x + y * 7) % (sizeof(list) / sizeof(list[0]))]);

    return picture;
}

// Writes the picture into `proxy` (resting in PRESENT) as `format` stores it: B8G8R8A8 swaps red and blue in memory
void Fill(Gpu& g, ID3D12Resource* proxy, bool bgra, const std::vector<Rgb>& picture)
{
    UINT64 total = 0;
    const auto footprint = Footprint(g, proxy, total);
    auto upload = Buffer(g, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* mapped = nullptr;
    upload->Map(0, nullptr, (void**) &mapped);

    for (UINT y = 0; y < kHeight; ++y)
    {
        for (UINT x = 0; x < kWidth; ++x)
        {
            const Rgb& p = picture[x + y * kWidth];
            uint8_t* texel = mapped + footprint.Offset + (size_t) y * footprint.Footprint.RowPitch + (size_t) x * 4;
            texel[0] = bgra ? p.b : p.r;
            texel[1] = p.g;
            texel[2] = bgra ? p.r : p.b;
            texel[3] = 255;
        }
    }

    upload->Unmap(0, nullptr);

    const auto in = Transition(proxy, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    g.list->ResourceBarrier(1, &in);
    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = proxy;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const auto out = Transition(proxy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    g.list->ResourceBarrier(1, &out);
    g.Submit();
}

// The first subresource of `texture` (resting in PRESENT) as bytes, row pitch removed: kWidth * bytesPerPixel per row
std::vector<uint8_t> Read(Gpu& g, ID3D12Resource* texture, UINT bytesPerPixel)
{
    UINT64 total = 0;
    const auto footprint = Footprint(g, texture, total);
    auto readback = Buffer(g, D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);

    const auto in = Transition(texture, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.list->ResourceBarrier(1, &in);
    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = texture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const auto out = Transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    g.list->ResourceBarrier(1, &out);
    g.Submit();

    std::vector<uint8_t> bytes;
    uint8_t* mapped = nullptr;

    if (FAILED(readback->Map(0, nullptr, (void**) &mapped)))
        return bytes;

    for (UINT y = 0; y < kHeight; ++y)
    {
        const uint8_t* line = mapped + footprint.Offset + (size_t) y * footprint.Footprint.RowPitch;
        bytes.insert(bytes.end(), line, line + (size_t) kWidth * bytesPerPixel);
    }

    readback->Unmap(0, nullptr);
    return bytes;
}

float HalfToFloat(uint16_t h)
{
    const int sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
    float value;

    if (exponent == 0)
        value = std::ldexp((float) mantissa, -24);
    else if (exponent == 31)
        value = mantissa != 0 ? NAN : INFINITY;
    else
        value = std::ldexp((float) (mantissa + 1024), exponent - 25);

    return sign != 0 ? -value : value;
}

// The size of one step of a half at `value`: the best an FP16 buffer can do
float HalfUlp(float value)
{
    int exponent = 0;
    std::frexp(std::fabs(value) < 6.1e-5f ? 6.1e-5f : std::fabs(value), &exponent);
    return std::ldexp(1.0f, exponent - 11);
}

// ---- the reference (independent of the shader's code) ----
double SrgbDecode(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }

double PqEncode(double nits)
{
    const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    const double p = std::pow(std::clamp(nits / 10000.0, 0.0, 1.0), m1);
    return std::pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

// The three output channels (scRGB units, or PQ signal) for one pixel
void Reference(const SdrToHdr::PassConstants& k, const Rgb& p, double out[3])
{
    const double c[3] = { p.r / 255.0, p.g / 255.0, p.b / 255.0 };
    double nits[3];

    for (int i = 0; i < 3; ++i)
    {
        double lin;

        if (k.convert != 0)
            lin = k.decode == 0 ? std::pow(c[i], 2.2) : SrgbDecode(c[i]);
        else
            lin = SrgbDecode(c[i]);

        nits[i] = std::min(lin * (k.convert != 0 ? k.gameNits : k.sdrWhiteNits), (double) k.peakNits);
    }

    if (k.mode == 0)
    {
        for (int i = 0; i < 3; ++i)
            out[i] = nits[i] / 80.0;

        return;
    }

    out[0] = PqEncode(0.6274039 * nits[0] + 0.3292830 * nits[1] + 0.0433131 * nits[2]);
    out[1] = PqEncode(0.0690973 * nits[0] + 0.9195404 * nits[1] + 0.0113623 * nits[2]);
    out[2] = PqEncode(0.0163914 * nits[0] + 0.0880133 * nits[1] + 0.8955953 * nits[2]);
}

// Compares the pass's output with the reference; returns the largest error (scRGB units, or PQ codes at 10 bit)
// `relative`: an extra allowance (scRGB only) as a fraction of the value, for a typed _SRGB proxy (gSrcLinear): the hardware's
// fixed-function sRGB decoder is not bit-exact with the exact CPU curve in the reference, but stays below one 8-bit step (0.5%).
// Every other case keeps max(1e-3, one FP16 step) and 1 PQ code.
bool Compare(const std::vector<uint8_t>& out, const SdrToHdr::PassConstants& k, const std::vector<Rgb>& picture,
             double& worst, double relative = 0.0)
{
    worst = 0.0;
    bool ok = out.size() == (size_t) kWidth * kHeight * (k.mode == 0 ? 8 : 4);

    for (size_t i = 0; ok && i < picture.size(); ++i)
    {
        double expected[3];
        Reference(k, picture[i], expected);

        for (int channel = 0; channel < 3; ++channel)
        {
            if (k.mode == 0)
            {
                uint16_t half;
                std::memcpy(&half, &out[i * 8 + channel * 2], 2);
                const float got = HalfToFloat(half);
                const double error = std::fabs(got - expected[channel]);
                // FP16 near 2.5 cannot be closer than a step of 2e-3: allow one step where that exceeds 1e-3
                const double allowed = std::max({ 1e-3, (double) HalfUlp((float) expected[channel]), relative * expected[channel] });
                if (error > worst && error > 1e-3)
                    std::printf("      worst so far: pixel %zu channel %d (%d,%d,%d) got %.6f expected %.6f\n", i, channel, picture[i].r, picture[i].g,
                                picture[i].b, got, expected[channel]);

                worst = std::max(worst, error);
                ok = ok && error <= allowed;
            }
            else
            {
                uint32_t texel;
                std::memcpy(&texel, &out[i * 4], 4);
                const double code = (double) ((texel >> (channel * 10)) & 1023);
                const double error = std::fabs(code - expected[channel] * 1023.0);
                worst = std::max(worst, error);
                ok = ok && error <= 1.0;
            }
        }

        // Alpha 1
        if (k.mode == 0)
        {
            uint16_t half;
            std::memcpy(&half, &out[i * 8 + 6], 2);
            ok = ok && HalfToFloat(half) == 1.0f;
        }
        else
        {
            uint32_t texel;
            std::memcpy(&texel, &out[i * 4], 4);
            ok = ok && (texel >> 30) == 3u;
        }
    }

    return ok;
}

ULONG RefCount(IUnknown* object)
{
    object->AddRef();
    return object->Release();
}

struct Case
{
    const char* name;
    SdrToHdr::PassConstants k;
};

Case Cases(int mode, int which)
{
    SdrToHdr::PassConstants k;
    k.mode = (uint32_t) mode;
    k.peakNits = 1000.0f;

    switch (which)
    {
    case 0:
        return { "gamma 2.2, 203 nits", k };
    case 1:
        k.decode = 1;
        return { "sRGB curve, 203 nits", k };
    case 2:
        k.gameNits = 80.0f;
        return { "gamma 2.2, 80 nits", k };
    case 3:
        k.gameNits = 400.0f;
        k.peakNits = 300.0f;
        return { "gamma 2.2, 400 nit white clamped to a 300 nit peak", k };
    case 4:
        k.convert = 0;
        k.sdrWhiteNits = 80.0f;
        return { "Convert off, SDR white 80 nits", k };
    default:
        k.convert = 0;
        k.sdrWhiteNits = 240.0f;
        return { "Convert off, SDR white 240 nits", k };
    }
}

// One proxy format, both outputs, every case; three back buffer indexes, each case run twice in a row
void Matrix(Gpu& g, SdrToHdr::Dx12Pass& pass, DXGI_FORMAT proxyFormat, const char* label)
{
    const bool bgra = proxyFormat == DXGI_FORMAT_B8G8R8A8_UNORM || proxyFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    // A typed _SRGB proxy (no relaxed casting) is read through an _SRGB view, which decodes: the shader encodes it again
    const bool srgbTyped = proxyFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || proxyFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const auto picture = Picture();
    constexpr UINT kBuffers = 3;

    for (int mode = 0; mode < 2; ++mode)
    {
        const DXGI_FORMAT realFormat = mode == 0 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R10G10B10A2_UNORM;
        const char* init = pass.Init(g.device.Get(), realFormat, kBuffers);
        Check(init == nullptr, "Init");

        if (init != nullptr)
        {
            std::printf("      %s\n", init);
            return;
        }

        ComPtr<ID3D12Resource> proxies[kBuffers], reals[kBuffers];

        for (UINT i = 0; i < kBuffers; ++i)
        {
            proxies[i] = BackBuffer(g, proxyFormat);
            reals[i] = BackBuffer(g, realFormat);
            Fill(g, proxies[i].Get(), bgra, picture);
        }

        for (int which = 0; which < 6; ++which)
        {
            Case c = Cases(mode, which);
            c.k.srcLinear = srgbTyped ? 1u : 0u;
            const UINT index = (UINT) which % kBuffers;

            // twice without waiting: the second must wait for the first's allocator
            const char* first = pass.Run(g.queue.Get(), index, proxies[index].Get(), reals[index].Get(), c.k);
            const char* second = pass.Run(g.queue.Get(), index, proxies[index].Get(), reals[index].Get(), c.k);
            g.Wait();

            const auto out = Read(g, reals[index].Get(), mode == 0 ? 8 : 4);
            double worst = 0.0;
            const double relative = srgbTyped ? 0.005 : 0.0; // WARP and hardware alike (see Compare)
            const bool ok = first == nullptr && second == nullptr && Compare(out, c.k, picture, worst, relative);
            char what[320];
            std::snprintf(what, sizeof(what), "%s -> %s, %s: max error %.5f%s", label, mode == 0 ? "scRGB FP16" : "PQ 10 bit",
                          c.name, worst, mode == 0 ? "" : " codes");
            Check(ok, what);

            // The tolerance must not hide the bug beside it: if the shader did not encode again (gSrcLinear not applied), the
            // view's decode would be decoded a second time. The output of this run, compared with what that bug gives (the
            // reference for the picture decoded once beforehand), must be far outside the tolerance.
            if (srgbTyped && mode == 0)
            {
                std::vector<Rgb> twice = picture;

                for (auto& px : twice)
                {
                    px.r = (uint8_t) std::lround(255.0 * SrgbDecode(px.r / 255.0));
                    px.g = (uint8_t) std::lround(255.0 * SrgbDecode(px.g / 255.0));
                    px.b = (uint8_t) std::lround(255.0 * SrgbDecode(px.b / 255.0));
                }

                double bugWorst = 0.0;
                const bool bugPasses = Compare(out, c.k, twice, bugWorst, relative);
                std::snprintf(what, sizeof(what),
                              "%s, %s: a double decode would be caught: it differs by %.4f, the seen error is %.5f (x%.0f margin)", label,
                              c.name, bugWorst, worst, bugWorst / std::max(worst, 1e-9));
                Check(!bugPasses && bugWorst > 10.0 * worst, what);
            }
        }
    }
}

// The live Convert toggle: two passes over the same proxy, Convert on then off (the hotkey / checkbox), each presented
// frame has its own values: ours (gamma 2.2, 203 nits) and the Windows baseline (piecewise sRGB at the SDR white level)
void ConvertToggle(Gpu& g, SdrToHdr::Dx12Pass& pass)
{
    const auto picture = Picture();
    Check(pass.Init(g.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 1) == nullptr, "Init");
    auto proxy = BackBuffer(g, DXGI_FORMAT_R8G8B8A8_UNORM);
    auto real = BackBuffer(g, DXGI_FORMAT_R16G16B16A16_FLOAT);
    Fill(g, proxy.Get(), false, picture);
    std::vector<uint8_t> frames[3];
    SdrToHdr::PassConstants k = Cases(0, 0).k;
    k.sdrWhiteNits = 120.0f;
    const uint32_t converts[3] = { 1, 0, 1 };

    for (int i = 0; i < 3; ++i)
    {
        k.convert = converts[i];
        const char* ran = pass.Run(g.queue.Get(), 0, proxy.Get(), real.Get(), k);
        g.Wait();
        frames[i] = Read(g, real.Get(), 8);
        double worst = 0.0;
        char what[160];
        std::snprintf(what, sizeof(what), "Convert toggle, frame %d (Convert %u) matches its reference", i, converts[i]);
        Check(ran == nullptr && Compare(frames[i], k, picture, worst), what);
    }

    Check(frames[0] != frames[1] && frames[0] == frames[2],
          "Convert toggle: the next presented frame changes (on, off, on gives A, B, A)");
}

// Relaxed format casting: a B8G8R8A8_UNORM proxy the game draws through an _SRGB view
void Relaxed(Gpu& g, SdrToHdr::Dx12Pass& pass)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12 {};
    const bool featureOk = SUCCEEDED(g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12,
                                                                   sizeof(options12)));
    ComPtr<ID3D12Device10> device10;
    g.device.As(&device10);
    std::printf("relaxed format casting: feature query %s, RelaxedFormatCastingSupported %d, ID3D12Device10 %s\n",
                featureOk ? "ok" : "failed", featureOk ? (int) options12.RelaxedFormatCastingSupported : -1,
                device10 != nullptr ? "yes" : "no");

    if (!featureOk || !options12.RelaxedFormatCastingSupported || device10 == nullptr)
    {
        std::printf("      not supported on this adapter: the sRGB-view case is skipped\n");
        return;
    }

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC1 desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const DXGI_FORMAT castable[] = { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB };
    ComPtr<ID3D12Resource> proxy;
    const HRESULT made = device10->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                            D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 1, castable,
                                                            IID_PPV_ARGS(&proxy));
    Check(SUCCEEDED(made) && proxy != nullptr, "relaxed casting: proxy made with a castable _SRGB format");

    if (proxy == nullptr)
        return;

    Check(proxy->GetDesc().Format == DXGI_FORMAT_B8G8R8A8_UNORM, "relaxed casting: GetDesc still shows B8G8R8A8_UNORM");

    // Draw (clear) through an _SRGB RTV: each column a different linear value; the hardware encodes on write
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    g.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap));
    D3D12_RENDER_TARGET_VIEW_DESC rtv {};
    rtv.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    const auto rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(proxy.Get(), &rtv, rtvHandle);

    const auto in = Transition(proxy.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g.list->ResourceBarrier(1, &in);

    const float linear[kWidth] = { 0.0f, 0.001f, 0.005f, 0.02f, 0.05f, 0.1f, 0.2f, 0.3f,
                                   0.4f, 0.5f,   0.6f,   0.7f,  0.8f,  0.9f, 0.95f, 1.0f };

    for (UINT x = 0; x < kWidth; ++x)
    {
        const float color[4] = { linear[x], linear[x], linear[x], 1.0f };
        const D3D12_RECT rect { (LONG) x, 0, (LONG) x + 1, (LONG) kHeight };
        g.list->ClearRenderTargetView(rtvHandle, color, 1, &rect);
    }

    const auto out = Transition(proxy.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    g.list->ResourceBarrier(1, &out);
    g.Submit();

    // What memory holds is the sRGB-encoded value; the reference input is those bytes
    const auto bytes = Read(g, proxy.Get(), 4);
    std::vector<Rgb> picture;
    bool encoded = bytes.size() == (size_t) kWidth * kHeight * 4;

    for (size_t i = 0; encoded && i < (size_t) kWidth * kHeight; ++i)
        picture.push_back({ bytes[i * 4 + 2], bytes[i * 4 + 1], bytes[i * 4 + 0] });

    // the mid grey 0.5 linear is 188 encoded (0.735): the draw really encoded
    encoded = encoded && picture.size() == (size_t) kWidth * kHeight && picture[9].r >= 187 && picture[9].r <= 189;
    Check(encoded, "relaxed casting: the _SRGB view wrote sRGB-encoded bytes (0.5 linear = 188)");

    if (!encoded)
        return;

    for (int mode = 0; mode < 2; ++mode)
    {
        const DXGI_FORMAT realFormat = mode == 0 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R10G10B10A2_UNORM;
        Check(pass.Init(g.device.Get(), realFormat, 1) == nullptr, "Init");
        auto real = BackBuffer(g, realFormat);
        const Case c = Cases(mode, 1); // the sRGB curve: the pass undoes the hardware's encode
        const char* ran = pass.Run(g.queue.Get(), 0, proxy.Get(), real.Get(), c.k);
        g.Wait();
        const auto result = Read(g, real.Get(), mode == 0 ? 8 : 4);
        double worst = 0.0;
        char what[200];
        const bool ok = ran == nullptr && Compare(result, c.k, picture, worst);
        std::snprintf(what, sizeof(what), "relaxed casting: sRGB-view proxy -> %s, sRGB curve: max error %.5f",
                      mode == 0 ? "scRGB FP16" : "PQ 10 bit", worst);
        Check(ok, what);
    }
}
} // namespace

int main(int argc, char** argv)
{
    const bool warp = argc > 1 && std::strcmp(argv[1], "warp") == 0;
    Gpu g;

    if (!MakeGpu(g, warp))
    {
        std::printf("no D3D12 device\n");
        return 2;
    }

    std::printf("%s, debug layer: %s\n", warp ? "WARP" : "hardware adapter",
                g.info != nullptr ? "on" : "not installed (its check is skipped)");

    const ULONG before = RefCount(g.device.Get());

    {
        SdrToHdr::Dx12Pass pass;
        Check(!pass.Ready(), "not ready before Init");
        Check(pass.Init(nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT, 2) != nullptr, "Init without a device says why");
        Check(pass.Init(g.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 2) != nullptr, "Init for a 8-bit output says why");
        Check(pass.Init(g.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 17) != nullptr, "Init for 17 buffers says why");

        Matrix(g, pass, DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM proxy");
        Matrix(g, pass, DXGI_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM proxy");
        Matrix(g, pass, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "R8G8B8A8_UNORM_SRGB typed proxy (gSrcLinear)");
        Matrix(g, pass, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, "B8G8R8A8_UNORM_SRGB typed proxy (gSrcLinear)");
        ConvertToggle(g, pass);
        Relaxed(g, pass);

        // A bad call is refused, not run
        Check(pass.Init(g.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 2) == nullptr, "Init again");
        auto proxy = BackBuffer(g, DXGI_FORMAT_R8G8B8A8_UNORM);
        auto real = BackBuffer(g, DXGI_FORMAT_R16G16B16A16_FLOAT);
        Check(pass.Run(g.queue.Get(), 2, proxy.Get(), real.Get(), SdrToHdr::PassConstants {}) != nullptr,
              "an index past the buffer count is refused");
        auto wrong = BackBuffer(g, DXGI_FORMAT_R10G10B10A2_UNORM);
        Check(pass.Run(g.queue.Get(), 0, proxy.Get(), wrong.Get(), SdrToHdr::PassConstants {}) != nullptr,
              "an output that is not the Init format is refused");
        g.Wait();

        pass.Release();
        Check(!pass.Ready(), "not ready after Release");
    }

    g.Wait();
    // Everything the pass made held a reference on the device; they are gone, the resources of this test are not counted
    // because they are released as their scope ends
    const ULONG after = RefCount(g.device.Get());
    char what[160];
    std::snprintf(what, sizeof(what), "device reference count after Release and the test's own objects: %lu before, %lu after",
                  before, after);
    Check(after == before, what);

    const int messages = DebugMessages(g);
    char debug[96];
    std::snprintf(debug, sizeof(debug), "the debug layer reported %d errors or warnings", messages);
    Check(messages == 0, debug);

    std::printf("%s (%d failed)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
