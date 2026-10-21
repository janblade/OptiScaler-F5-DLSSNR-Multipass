// Reuses the optical-flow fixtures; runs the production DX12 visualizer and reads its output.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <d3d12.h>
#include <d3d12sdklayers.h>
#define main OriginalOpticalFlowRegressionMain
#include "nr_optical_flow_gpu.cpp"
#undef main
#include "nr_motion_readback.h"
#include <fstream>
#include "../OptiScaler/motion/TrustMask_Dx12.h"

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
    bool ok = flow.Preview() == nullptr && flow.CandidateTrace()==nullptr;
    printf("debug disabled: no preview allocated: %s\n", ok ? "PASS" : "FAIL");
    flow.Tuning().trace=true;
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

    TrustMaskDx12 trust;
    ok &= trust.Init(gpu.device.Get());
    TrustMaskDx12::Inputs inputs;
    inputs.flow=flow.Flow();inputs.flowWidth=flow.FlowWidth();inputs.flowHeight=flow.FlowHeight();
    inputs.fullPerFlow=float(kWidth)/flow.FlowWidth();
    inputs.lumaNow=flow.LumaOfLastFrame();inputs.lumaBefore=flow.LumaOfFrameBefore();inputs.sceneCut=flow.SceneCutFlag();
    ok &= trust.Dispatch(gpu.list.Get(),inputs);
    ok &= trust.BuildGuides(gpu.list.Get(),inputs,kWidth,kHeight);
    gpu.Submit();
    const auto guideBefore=ReadFlow(gpu,trust.GuideMotion(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for(int view:{7,6,0,18,5,17,12}){
        ok &= flow.Visualise(gpu.list.Get(),24,view,b.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,
                             nullptr,DXGI_FORMAT_UNKNOWN,true,trust.GuideMotion(),trust.Mask());
        gpu.Submit();
    }
    const bool unchanged=before==ReadFlow(gpu,flow.Flow()) &&
        guideBefore==ReadFlow(gpu,trust.GuideMotion(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ok &= unchanged;
    printf("seven public debug views preserve matching and full-resolution consumer vectors: %s\n",unchanged?"PASS":"FAIL");
    // The first mask is full distrust: confidence's blue channel must be zero.
    ok &= flow.Visualise(gpu.list.Get(),24,5,b.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,
                         nullptr,DXGI_FORMAT_UNKNOWN,true,trust.GuideMotion(),trust.Mask());
    auto confidence=ReadPreview(gpu,flow.Preview());bool coldHistory=true,texture=false,fit=false;
    for(size_t i=0;i<confidence.size();i+=4){coldHistory &= i/4/flow.Preview()->GetDesc().Width<40 || confidence[i+2]==0;texture |= confidence[i]>0;fit |= confidence[i+1]>0;}
    ok &= coldHistory && texture && fit;
    printf("confidence RGB: nonzero texture/fit, first history distrusted: %s\n",coldHistory&&texture&&fit?"PASS":"FAIL");
    // Missing depth has the documented magenta checker, never stale depth.
    for(int view:{6}){
        ok &= flow.Visualise(gpu.list.Get(),24,view);
        auto image=ReadPreview(gpu,flow.Preview());bool checker=true;
        for(size_t i=0;i<image.size();i+=4)checker &= i/4/flow.Preview()->GetDesc().Width<40 || (image[i]>0 && image[i+1]==0 && image[i+2]==image[i] && image[i+3]==255);
        ok &= checker;printf("unavailable depth view %d shows magenta checker: %s\n",view,checker?"PASS":"FAIL");
    }
    // The seven public modes all preserve estimator/consumer fields. Rejection
    // is a real validation input; still age does not require trace allocation.
    for(int mode:{7,6,0,18,5,17,12}) {
        ok &= flow.Visualise(gpu.list.Get(),24,mode,b.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,
            nullptr,DXGI_FORMAT_UNKNOWN,true,trust.GuideMotion(),trust.Mask());gpu.Submit();
    }
    ok &= before==ReadFlow(gpu,flow.Flow()) && guideBefore==ReadFlow(gpu,trust.GuideMotion(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ok &= flow.Visualise(gpu.list.Get(),24,17,b.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,
        nullptr,DXGI_FORMAT_UNKNOWN,true,trust.GuideMotion(),trust.Mask());
    auto rejected=ReadPreview(gpu,flow.Preview());bool coldReject=true;
    for(size_t i=0;i<rejected.size();i+=4)if(i/4/flow.Preview()->GetDesc().Width>=40)coldReject &= rejected[i]==255 && rejected[i+1]==255 && rejected[i+2]==255;
    ok &= coldReject;printf("seven public views; cold history fully rejected: %s\n",coldReject?"PASS":"FAIL");
    OpticalFlowDx12 ageFlow;ok &= ageFlow.Init(gpu.device.Get());ageFlow.Tuning().stillFrames=2;ageFlow.Tuning().sceneCutDetector=false;
    for(int n=0;n<5;++n){ok &= ageFlow.Dispatch(gpu.list.Get(),a.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);gpu.Submit();}
    ok &= ageFlow.Visualise(gpu.list.Get(),24,12);auto ageImage=ReadPreview(gpu,ageFlow.Preview());bool aged=true;
    for(size_t i=0;i<ageImage.size();i+=4)if(i/4/ageFlow.Preview()->GetDesc().Width>=40)aged &= ageImage[i]==255 && ageImage[i+1]==255 && ageImage[i+2]==255;
    bool noTrace=!ageFlow.CandidateTrace()&&!ageFlow.RefinementTrace()&&!ageFlow.EventTrace();
    ok &= aged&&noTrace;printf("still age reaches threshold with no trace allocation: %s\n",aged&&noTrace?"PASS":"FAIL");
    flow.Reset();
    const bool reset=!flow.FlowValid()&&!flow.Visualise(gpu.list.Get(),24,7);
    ok &= reset;printf("reset invalidates previous vectors: %s\n",reset?"PASS":"FAIL");
    ok &= SUCCEEDED(gpu.device->GetDeviceRemovedReason());
    ComPtr<ID3D12InfoQueue> info;
    if(SUCCEEDED(gpu.device.As(&info))){
        unsigned errors=0;
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);
            std::vector<unsigned char> data(size);auto m=reinterpret_cast<D3D12_MESSAGE*>(data.data());
            info->GetMessage(i,m,&size);
            if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;printf("D3D12: %s\n",m->pDescription);}}
        ok &= errors==0;printf("D3D12 validation errors: %u\n",errors);
    }
    printf("motion debug GPU: %s\n",ok?"PASS":"FAIL");return ok?0:1;
}
