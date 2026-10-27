#include "pch.h"
#define main LegacySpatialTestMain
#include "../nr_spatial_dx12_gpu.cpp"
#undef main
#include "../../OptiScaler/shaders/dlssnr/superior/precompile/host_joint_guides_Shader.h"
#include "../../OptiScaler/dlssnr/DlssNr_SuperiorScaling.h"

// Checks temporal correspondence in the fixed NR colour lattice while input,
// active guide size and vector encoding change. Does not run the NVIDIA model.
int main()
{
    Gpu gpu;
    if (!gpu.Init())
        return 2;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
    pd.pRootSignature = gpu.rootSignature.Get();
    pd.CS = { host_joint_guides_cso, sizeof(host_joint_guides_cso) };
    ComPtr<ID3D12PipelineState> pipeline;
    CHECK(SUCCEEDED(gpu.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pipeline))));
    gpu.guides = pipeline;
    Sp::Settings settings;
    settings.enabled = true;
    constexpr UINT W = 384, H = 216;
    const auto colour = Sp::Build(settings, W, H, 1.f, false);
    CHECK(colour.active);
    auto od = gpu.Texture(DXGI_FORMAT_R32_FLOAT, colour.modelW, colour.modelH, true);
    auto om = gpu.Texture(DXGI_FORMAT_R32G32_FLOAT, colour.modelW, colour.modelH, true);
    Sp::Tracker tracker;
    DlssNr::SuperiorScaling::PrivatePresetChoice preset;
    size_t checked = 0;
    double maxError = 0;
    for (unsigned frame = 0; frame < 32; ++frame)
    {
        const UINT rw = frame % 2 ? 255 : 192, rh = frame % 2 ? 143 : 108;
        const auto active = Sp::RenderGuideLayout(colour, { rw, rh });
        const float vx = frame % 3 ? -7.f : 35.f, vy = frame % 3 ? 3.f : -12.f;
        const UINT aw = rw + 13, ah = rh + 9;
        std::vector<float> depth(size_t(aw) * ah, .4f), motion(size_t(aw) * ah * 2, 0);
        const bool normalized = frame % 2 != 0;
        const float mx = normalized ? 1.f : float(rw), my = normalized ? 1.f : float(rh);
        for (size_t i = 0; i < motion.size() / 2; ++i)
        {
            motion[i * 2] = vx * mx / W;
            motion[i * 2 + 1] = vy * my / H;
        }
        auto dt = gpu.Upload(DXGI_FORMAT_R32_FLOAT, aw, ah, 4, depth.data());
        auto mt = gpu.Upload(DXGI_FORMAT_R32G32_FLOAT, aw, ah, 8, motion.data());
        const DlssNr::GuideRegions regions { { 3, 4, rw, rh }, { 3, 4, rw, rh } };
        auto c = Sp::MakeGuideConstants(active, regions, { rw, rh }, normalized ? float(rw) : 1.f,
                                        normalized ? float(rh) : 1.f);
        c.padding[0] = 1;
        gpu.Dispatch(c, dt.Get(), dt.Get(), mt.Get(), od.Get(), om.Get());
        auto bytes = gpu.Read(om.Get(), 8);
        const auto* values = reinterpret_cast<const float*>(bytes.data());
        const auto state = tracker.Begin(settings, W, H, 1.f, false, false, Sp::SuperiorSignature(10, 41, 16));
        CHECK(state.active && !state.resetHistory);
        CHECK(preset.Select(1, true, 1.f, frame % 2 ? .65f : 1.f) == 5);
        for (UINT y = 0; y < active.modelH; ++y)
            for (UINT x = 0; x < active.modelW; ++x)
            {
                const float nx = pw::Unpack(x + .5f, active.warp.x), ny = pw::Unpack(y + .5f, active.warp.y);
                const size_t i = (size_t(y) * colour.modelW + x) * 2;
                const float hx = (x + .5f + values[i]) * colour.modelW / active.modelW;
                const float hy = (y + .5f + values[i + 1]) * colour.modelH / active.modelH;
                const float ex = pw::Pack(nx + vx, colour.warp.x), ey = pw::Pack(ny + vy, colour.warp.y);
                maxError = std::max(maxError, double(std::max(std::abs(hx - ex), std::abs(hy - ey))));
                CHECK(std::abs(hx - ex) < .001f && std::abs(hy - ey) < .001f);
                ++checked;
            }
    }
    std::printf("Dynamic guide correspondence PASS: 32 frames, %zu points, maximum %.9f colour pixels; stable "
                "allocation, alternating subrect/units, center/edges/crossings, no tracker resets, fixed CNN E\n",
                checked, maxError);
    return fails ? 1 : 0;
}
