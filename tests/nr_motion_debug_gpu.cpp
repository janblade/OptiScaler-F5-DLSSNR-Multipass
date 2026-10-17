// Reuses the optical-flow fixtures; runs the production DX12 visualizer and reads its output.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <d3d12.h>
#include <d3d12sdklayers.h>
#define main OriginalOpticalFlowRegressionMain
#include "nr_optical_flow_gpu.cpp"
#undef main
#include <fstream>

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    printf("D3D12 debug layer: %s\n", debug ? "enabled" : "unavailable; hardware execution only");
    kWidth = argc > 1 ? (uint32_t)atoi(argv[1]) : 641;
    kHeight = argc > 2 ? (uint32_t)atoi(argv[2]) : 361;
    Gpu gpu;
    if (!gpu.Init()) return 2;
    OpticalFlowDx12 flow;
    if (!flow.Init(gpu.device.Get())) return 2;
    flow.Tuning().sceneCutDetector = false;
    flow.Tuning().stillFrames = 0;
    auto a = gpu.Picture(0, 0);
    auto b = gpu.Picture(5, -3);
    flow.Dispatch(gpu.list.Get(), a.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    gpu.Submit();
    bool ok = flow.Preview() == nullptr;
    printf("debug disabled: no preview allocated: %s\n", ok ? "PASS" : "FAIL");
    flow.Dispatch(gpu.list.Get(), b.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    const auto before = ReadFlow(gpu, flow.Flow());
    double epe = 0.0;
    unsigned samples = 0, good = 0;
    for (unsigned y = 24; y+24 < flow.FlowHeight(); ++y)
    for (unsigned x = 24; x+24 < flow.FlowWidth(); ++x)
    {
        const size_t i = 2*(y*flow.FlowWidth()+x);
        const double error = std::hypot(before[i]+5.0, before[i+1]-3.0);
        ok &= std::isfinite(error);
        epe += error; good += error <= 1.0; ++samples;
    }
    epe /= (std::max)(1u, samples);
    printf("%ux%u, expected backward (-5,+3): EPE %.4f px, within 1px %.2f%%\n",
           kWidth, kHeight, epe, 100.0*good/(std::max)(1u,samples));
    ok &= epe < 1.25;
    flow.DebugTuning().background = false;
    ok &= flow.Visualise(gpu.list.Get(), 24, 7);
    const auto arrows = ReadPreview(gpu, flow.Preview());
    size_t colored = 0, transparent = 0;
    for (size_t i = 0; i < arrows.size(); i += 4)
    {
        const int lo = (std::min)({arrows[i], arrows[i+1], arrows[i+2]});
        const int hi = (std::max)({arrows[i], arrows[i+1], arrows[i+2]});
        colored += hi-lo > 50 && arrows[i+3] > 180;
        transparent += arrows[i+3] == 0;
    }
    ok &= colored > 10 && transparent > arrows.size()/8;
    printf("transparent colored arrows: %zu colored, %zu transparent pixels\n", colored, transparent);
    const auto arrowDesc = flow.Preview()->GetDesc();
    auto coloredNear = [&](float x, float y)
    {
        const int cx = (int)(x*arrowDesc.Width/kWidth), cy = (int)(y*arrowDesc.Height/kHeight);
        bool found = false;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
        {
            const int px = cx+dx, py = cy+dy;
            if (px<0 || py<0 || px>=(int)arrowDesc.Width || py>=(int)arrowDesc.Height) continue;
            const size_t i = 4*(py*arrowDesc.Width+px);
            const int hi = (std::max)({arrows[i],arrows[i+1],arrows[i+2]});
            const int lo = (std::min)({arrows[i],arrows[i+1],arrows[i+2]});
            found |= hi-lo>50 && arrows[i+3]>180;
        }
        return found;
    };
    const float dx = 8.0f*5.0f/std::sqrt(34.0f), dy = -8.0f*3.0f/std::sqrt(34.0f);
    const bool forwardArrow = coloredNear(192+dx,96+dy) && !coloredNear(192-dx,96-dy);
    ok &= forwardArrow;
    printf("arrows point with visible (+5,-3) motion: %s\n", forwardArrow ? "PASS" : "FAIL");
    flow.DebugTuning().background = true;
    ok &= flow.Visualise(gpu.list.Get(), 24, 7, b.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    const auto scene = ReadPreview(gpu, flow.Preview());
    const auto desc = flow.Preview()->GetDesc();
    if (argc > 3)
    {
        std::ofstream file(argv[3], std::ios::binary);
        file << "P6\n" << desc.Width << ' ' << desc.Height << "\n255\n";
        for (size_t i = 0; i < scene.size(); i += 4) file.write((const char*)&scene[i], 3);
    }
    for (size_t i = 3; i < scene.size(); i += 4) ok &= scene[i] == 255;
    const auto sources = ReadFlow(gpu, flow.FusionInfo(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ok &= flow.Visualise(gpu.list.Get(), 24, 8);
    const auto rejection = ReadPreview(gpu, flow.Preview());
    bool maskExact = true;
    for (size_t px = 0; px < rejection.size()/4; ++px)
    {
        const uint8_t expected = sources[2*px] < 1.5f ? 255 : 0;
        maskExact &= rejection[4*px] == expected && rejection[4*px+1] == expected &&
                     rejection[4*px+2] == expected && rejection[4*px+3] == 255;
    }
    ok &= maskExact;
    printf("isolated rejection mask exactly matches local/invalid versus accepted sources: %s\n", maskExact ? "PASS" : "FAIL");
    ok &= flow.Visualise(gpu.list.Get(), 24, 9);
    const auto model = ReadPreview(gpu, flow.Preview());
    size_t modelBlack = 0, modelCyan = 0;
    for (size_t i = 0; i < model.size(); i += 4)
    {
        modelBlack += model[i] == 0 && model[i+1] == 0 && model[i+2] == 0;
        modelCyan += model[i+2] > 80 && model[i+1] > 60 && model[i] < 30;
        ok &= model[i+3] == 255;
    }
    const bool isolatedModel = modelBlack > model.size()/8 && modelCyan > 10;
    ok &= isolatedModel;
    printf("isolated projective grid: %zu cyan / %zu black pixels: %s\n", modelCyan, modelBlack, isolatedModel ? "PASS" : "FAIL");
    for (int view = 0; view < 11; ++view)
    {
        ok &= flow.Visualise(gpu.list.Get(), 24, view);
        gpu.Submit();
    }
    const auto after = ReadFlow(gpu, flow.Flow());
    const bool unchanged = before == after;
    ok &= unchanged;
    printf("all debug views preserve vectors: %s\n", unchanged ? "PASS" : "FAIL");
    flow.Reset();
    const bool reset = !flow.FlowValid() && !flow.Visualise(gpu.list.Get(), 24, 7);
    ok &= reset;
    printf("reset invalidates previous vectors: %s\n", reset ? "PASS" : "FAIL");
    auto flat = gpu.Picture(0, 0, 0);
    flow.Dispatch(gpu.list.Get(), flat.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    flow.Dispatch(gpu.list.Get(), flat.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    ok &= flow.Visualise(gpu.list.Get(), 24, 2);
    const auto camera = ReadPreview(gpu, flow.Preview());
    bool invalidBlack = true;
    for (size_t i = 0; i < camera.size(); i += 4)
        invalidBlack &= camera[i] == 0 && camera[i+1] == 0 && camera[i+2] == 0 && camera[i+3] == 255;
    ok &= invalidBlack;
    printf("unsupported homography is black, not invented motion: %s\n", invalidBlack ? "PASS" : "FAIL");
    ok &= flow.Visualise(gpu.list.Get(), 24, 9);
    const auto noModel = ReadPreview(gpu, flow.Preview());
    bool noModelBlack = true;
    for (size_t i = 0; i < noModel.size(); i += 4)
        noModelBlack &= noModel[i] == 0 && noModel[i+1] == 0 && noModel[i+2] == 0 && noModel[i+3] == 255;
    ok &= noModelBlack;
    printf("invalid model-only view stays black: %s\n", noModelBlack ? "PASS" : "FAIL");
    ok &= flow.Visualise(gpu.list.Get(), 24, 10);
    const auto reason = ReadPreview(gpu, flow.Preview());
    bool invalidMagenta = true;
    for (size_t i = 0; i < reason.size(); i += 4)
        invalidMagenta &= reason[i] == 255 && reason[i+1] == 0 && reason[i+2] == 255 && reason[i+3] == 255;
    ok &= invalidMagenta;
    printf("rejection reason distinguishes invalid global model as magenta: %s\n", invalidMagenta ? "PASS" : "FAIL");
    ok &= SUCCEEDED(gpu.device->GetDeviceRemovedReason());
    printf("motion debug GPU: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
