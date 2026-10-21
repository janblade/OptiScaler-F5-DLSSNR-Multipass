// Complete motion GPU cost: estimator, trust, disocclusion and full-resolution guides.
// 64 warmup + 160 measured frames, stationary scene 5 or moving scene 0.
// Excludes neural rendering, display/debug, readback and transport from GPU timestamps.
#define main UnusedFlowRegressionMain
#include "nr_optical_flow_gpu.cpp"
#undef main
#include "../OptiScaler/motion/TrustMask_Dx12.h"
#include <cstdlib>
int main(int argc,char**argv){
    kWidth=argc>1?atoi(argv[1]):3413;kHeight=argc>2?atoi(argv[2]):1920;
    int scene=argc>3?atoi(argv[3]):0;
    Gpu g;if(!g.Init())return 2;OpticalFlowDx12 flow;TrustMaskDx12 trust;
    if(!flow.Init(g.device.Get())||!trust.Init(g.device.Get()))return 3;

    std::vector<ComPtr<ID3D12Resource>> pictures;
    for(int i=0;i<4;++i)pictures.push_back(g.Picture(scene==5?0.f:i*.7f,scene==5?0.f:i*.3f,1,0,i,0,0,0,scene));
    auto depth=g.DepthMap(kWidth*.4f,kHeight*.5f,kWidth*.18f);
    D3D12_QUERY_HEAP_DESC qd={D3D12_QUERY_HEAP_TYPE_TIMESTAMP,2,0};ComPtr<ID3D12QueryHeap> q;
    if(FAILED(g.device->CreateQueryHeap(&qd,IID_PPV_ARGS(&q))))return 4;
    auto read=g.Buffer(16,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 frequency=0;g.queue->GetTimestampFrequency(&frequency);std::vector<double> ms;
    for(int frame=0;frame<224;++frame){
        g.list->EndQuery(q.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        if(!flow.Dispatch(g.list.Get(),pictures[frame%4].Get(),DXGI_FORMAT_R8G8B8A8_UNORM,depth.Get(),DXGI_FORMAT_R32_FLOAT,true))return 5;
        if(flow.FlowValid()){
            TrustMaskDx12::Inputs in;in.flow=flow.Flow();in.flowWidth=flow.FlowWidth();in.flowHeight=flow.FlowHeight();
            in.fullPerFlow=float(kWidth)/flow.FlowWidth();in.lumaNow=flow.LumaOfLastFrame();in.lumaBefore=flow.LumaOfFrameBefore();
            in.sceneCut=flow.SceneCutFlag();in.depths[0]=depth.Get();in.depthCount=1;in.depthFormat=DXGI_FORMAT_R32_FLOAT;
            in.depthWidth=kWidth;in.depthHeight=kHeight;in.depthReversed=true;
            if(!trust.Dispatch(g.list.Get(),in)||!trust.BuildGuides(g.list.Get(),in,kWidth,kHeight))return 6;
        }
        g.list->EndQuery(q.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);g.list->ResolveQueryData(q.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,read.Get(),0);g.Submit();
        if(frame<64)continue;UINT64* ticks=nullptr;if(FAILED(read->Map(0,nullptr,(void**)&ticks)))return 7;
        ms.push_back(double(ticks[1]-ticks[0])*1000./frequency);read->Unmap(0,nullptr);
    }
    std::sort(ms.begin(),ms.end());double mean=0;for(double t:ms)mean+=t;
    printf("{\"width\":%u,\"height\":%u,\"scene\":%d,\"frames\":%zu,\"median_ms\":%.6f,\"p95_ms\":%.6f,\"mean_ms\":%.6f}\n",kWidth,kHeight,scene,ms.size(),ms[ms.size()/2],ms[ms.size()*95/100],mean/ms.size());
    return FAILED(g.device->GetDeviceRemovedReason())?8:0;
}
