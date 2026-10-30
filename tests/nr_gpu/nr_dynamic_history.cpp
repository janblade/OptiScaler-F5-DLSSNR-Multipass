#include "pch.h"
#include <filesystem>
#define main SpatialTestMain
#include "../nr_spatial_dx12_gpu.cpp"
#undef main
#include "../../external/nvngx_dlss_sdk/nvsdk_ngx_params.h"
int wmain(int argc, wchar_t** argv)
{
    if (argc != 4)
    {
        std::cerr << "Usage: nr_dynamic_history.exe <driver _nvngx.dll> <NR model dll> <test forwarder dll>\n";
        return 3;
    }
    Gpu gpu;
    if (!gpu.Init())
        return 2;
    const auto dataPath = std::filesystem::absolute("artifacts/validation").wstring();
    auto core = LoadLibraryW(argv[1]);
    auto helper = LoadLibraryW(argv[3]);
    if (!core || !helper)
    {
        std::printf("load error %u\n", GetLastError());
        return 3;
    }
    using Init = int (*)(unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
    auto init = (Init) GetProcAddress(core, "NVSDK_NGX_D3D12_Init_Ext");
    auto cap = (int (*)(NVSDK_NGX_Parameter**)) GetProcAddress(core, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    std::printf("Core init %x\n", init(0x24480451ull, dataPath.c_str(), gpu.device.Get(), 0x15, nullptr));
    NVSDK_NGX_Parameter* params = nullptr;
    int result = cap(&params);
    std::printf("Capabilities %x %p\n", result, params);
    if (result != 1 || !params)
        return 3;
    auto probe = (void (*)(void*, const char*, float, int)) GetProcAddress(helper, "dlssnr_call_probe_float");
    auto setSlot = (void (*)(int)) GetProcAddress(helper, "dlssnr_call_set_float_slot");
    bool slot = false;
    for (int i : { 1, 2, 5, 6, 7, 4, 3, 0 })
    {
        float v = 0;
        probe(params, "DLSSNR.Probe", .375f, i);
        if (params->Get("DLSSNR.Probe", &v) == 1 && v == .375f)
        {
            setSlot(i);
            slot = true;
            std::printf("float slot %d\n", i);
            break;
        }
    }
    if (!slot)
        return 3;
    using Create = void* (*) (const wchar_t*, const wchar_t*, ID3D12Device*, ID3D12GraphicsCommandList*, void*,
                              unsigned, unsigned, int, float, int, float, float, float, int, int);
    auto create = (Create) GetProcAddress(helper, "dlssnr_call_create");
    auto error = (const char* (*) ()) GetProcAddress(helper, "dlssnr_call_error");

    for (int preset : { 1, 2 })
        for (int style : { 0, 1, 2 })
        {
            void* features[3] {};
            for (unsigned k = 0; k < 3; ++k)
            {
                gpu.Begin();
                features[k] = create(argv[2], dataPath.c_str(), gpu.device.Get(), gpu.list.Get(), params, 256, 144,
                                     preset, 1, style, 1, 1, 1, 1, 0);
                if (!features[k])
                {
                    std::printf("NR create failed: %s\n", error());
                    return 3;
                }
                gpu.End();
            }
            using Evaluate =
                int (*)(ID3D12GraphicsCommandList*, void*, void*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
                        ID3D12Resource*, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                        unsigned, unsigned, int, int, float, int, float, float, float, int, float, float);
            auto evaluate = (Evaluate) GetProcAddress(helper, "dlssnr_call_evaluate_v2");
            constexpr UINT W = 256, H = 144;
            ComPtr<ID3D12Resource> output[3];
            for (auto& t : output)
                t = gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, true);
            double sum = 0, resetSum = 0, maximum = 0;
            size_t count = 0, different = 0, changed = 0;
            for (unsigned frame = 0; frame < 48; ++frame)
            {
                std::vector<uint16_t> color(size_t(W) * H * 4);
                for (UINT y = 0; y < H; ++y)
                    for (UINT x = 0; x < W; ++x)
                    {
                        unsigned world = (x + W - frame * 2 % W) % W;
                        size_t i = (size_t(y) * W + x) * 4;
                        float z = float((world * 13 + y * 7) % 251) / 512.f;
                        for (unsigned c = 0; c < 3; ++c)
                            color[i + c] = ToHalf(z + float(c) * .03125f);
                        color[i + 3] = ToHalf(1);
                    }
                auto input = gpu.Upload(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, 8, color.data());
                for (unsigned variant = 0; variant < 3; ++variant)
                {
                    const UINT gw = variant == 1 ? (frame % 3 == 0   ? 128
                                                    : frame % 3 == 1 ? 192
                                                                     : 256)
                                                 : W,
                               gh = gw * H / W;
                    std::vector<float> depth(size_t(W) * H, .4f), motion(size_t(W) * H * 2, 0);
                    for (size_t i = 0; i < motion.size() / 2; ++i)
                        motion[i * 2] = -2.f * gw / W;
                    auto dt = gpu.Upload(DXGI_FORMAT_R32_FLOAT, W, H, 4, depth.data());
                    auto mt = gpu.Upload(DXGI_FORMAT_R32G32_FLOAT, W, H, 8, motion.data());
                    gpu.Begin();
                    int e = evaluate(gpu.list.Get(), features[variant], params, input.Get(), dt.Get(), mt.Get(),
                                     output[variant].Get(), W, H, gw, gh, gw, gh, 0, 0, 0, 0, 0,
                                     (frame == 0 || variant == 2) ? 1 : 0, 1, style, 1, 1, 1, 1, 1, 1);
                    if (e != 1)
                    {
                        std::printf("NR evaluate failed frame %u variant %u: %s\n", frame, variant, error());
                        return 3;
                    }
                    gpu.End();
                }
                auto a = gpu.Read(output[0].Get(), 8), b = gpu.Read(output[1].Get(), 8),
                     c = gpu.Read(output[2].Get(), 8);
                if (frame < 8)
                    continue;
                const auto* av = reinterpret_cast<const uint16_t*>(a.data());
                const auto* bv = reinterpret_cast<const uint16_t*>(b.data());
                const auto* cv = reinterpret_cast<const uint16_t*>(c.data());
                for (UINT y = 16; y < H - 16; ++y)
                    for (UINT x = 16; x < W - 16; ++x)
                        for (unsigned k = 0; k < 3; ++k)
                        {
                            size_t i = (size_t(y) * W + x) * 4 + k;
                            double d = FromHalf(av[i]) - FromHalf(bv[i]), q = FromHalf(av[i]) - FromHalf(cv[i]);
                            sum += d * d;
                            resetSum += q * q;
                            maximum = std::max(maximum, std::abs(d));
                            different += av[i] != bv[i];
                            changed += av[i] != color[i];
                            ++count;
                        }
            }
            std::printf("Actual NR history preset %d style %d: %zu components, varying-guide RMS %.12g max %.12g, "
                        "differing %zu, reset-control RMS %.12g, model-changed %zu\n",
                        preset, style, count, std::sqrt(sum / count), maximum, different, std::sqrt(resetSum / count),
                        changed);
            for (auto f : features)
                ((void (*)(void*)) GetProcAddress(helper, "dlssnr_call_release"))(f);
            CHECK(sum == 0 && maximum == 0 && resetSum > 1e-9 && changed > count / 4);
        }
    return fails ? 1 : 0;
}
