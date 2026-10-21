// Known independent low-contrast advection over a static/panning background.
// Stage attribution distinguishes missing evidence from later regularization.
#define main UnusedFlowRegressionMain
#include "nr_optical_flow_gpu.cpp"
#undef main
#include "nr_motion_readback.h"
#include <array>

static ComPtr<ID3D12Resource> MicroFrame(Gpu& g,int n,float pan,float dx,float contrast,int mode) {
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width=kWidth;d.Height=kHeight;d.DepthOrArraySize=d.MipLevels=1;
    d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;
    ComPtr<ID3D12Resource> tex;
    if(FAILED(g.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&tex))))return {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes=0;
    g.device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
    auto upload=g.Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* data=nullptr;upload->Map(0,nullptr,(void**)&data);
    for(unsigned y=0;y<kHeight;++y)for(unsigned x=0;x<kWidth;++x) {
        bool patch=x>kWidth*.2f&&x<kWidth*.8f&&y>kHeight*.2f&&y<kHeight*.8f;
        float px=float(x)-n*(pan+dx),py=float(y)-n*dx*.35f;
        float wave=.55f*ValueNoise(px,py,16,91)+.3f*ValueNoise(px,py,7,121)+.15f*ValueNoise(px,py,3,181);
        for(int ch=0;ch<3;++ch) {
            float v=patch?.5f+contrast*(wave-.5f):Scene(float(x)-n*pan,float(y),ch);
            if(patch&&mode==1)v+=.003f*(Hash(x,y,n+ch)-.5f); // independent sensor/film grain control
            data[size_t(y)*fp.Footprint.RowPitch+4*x+ch]=uint8_t(std::clamp(v*255.f+.5f,0.f,255.f));
        }
        data[size_t(y)*fp.Footprint.RowPitch+4*x+3]=255;
    }
    upload->Unmap(0,nullptr);D3D12_TEXTURE_COPY_LOCATION src{},dst{};
    src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
    dst.pResource=tex.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;g.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={tex.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};g.list->ResourceBarrier(1,&b);g.Submit();return tex;
}
int main(int argc,char**argv) {
    kWidth=321;kHeight=181;setvbuf(stdout,nullptr,_IONBF,0);Gpu g;if(!g.Init())return 2;
    puts("pan,motion,contrast,noise,camera,stage,frames,samples,EPE,magnitude,aligned30,zero,meanX,meanY");
    int camera=argc>1?atoi(argv[1]):1;
    for(float pan:{0.f,.7f})for(float dx:{0.f,.05f,.15f,.4f,.8f})for(float contrast:{.04f,.12f}) {
        int noise=dx==0?1:0;OpticalFlowDx12 f;if(!f.Init(g.device.Get()))return 3;f.Tuning().cameraPrior=camera!=0;
        struct Stat{double error=0,mag=0,sx=0,sy=0;size_t count=0,aligned=0,zero=0;}stats[3];
        for(int n=0;n<24;++n) {
            auto picture=MicroFrame(g,n,pan,dx,contrast,noise);if(!picture)return 4;
            if(!f.Dispatch(g.list.Get(),picture.Get(),DXGI_FORMAT_R8G8B8A8_UNORM))return 5;
            g.Submit();if(n<8||!f.FlowValid())continue;
            ID3D12Resource* fields[]={f.MatchedFlow(),f.MedianFlow(),f.Flow()};
            for(int stage=0;stage<3;++stage) {
                auto field=ReadFlow(g,fields[stage],stage==2?(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE):D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                auto desc=fields[stage]->GetDesc();float units=stage==0?2.f:1.f;
                float tx=-(pan+dx),ty=-dx*.35f;double truth=hypot(tx,ty);auto& s=stats[stage];
                for(unsigned y=0;y<desc.Height;++y)for(unsigned x=0;x<desc.Width;++x) {
                    float px=(x+.5f)*kWidth/desc.Width,py=(y+.5f)*kHeight/desc.Height;
                    if(px<kWidth*.3f||px>kWidth*.7f||py<kHeight*.3f||py>kHeight*.7f)continue;
                    size_t i=(size_t(y)*desc.Width+x)*2;double vx=field[i]*units,vy=field[i+1]*units,mag=hypot(vx,vy);
                    if(!std::isfinite(mag))return 6;
                    s.error+=hypot(vx-tx,vy-ty);s.mag+=mag;s.sx+=vx;s.sy+=vy;++s.count;s.zero+=mag<.02;
                    s.aligned+=truth>.001&&mag>.001&&(vx*tx+vy*ty)/(mag*truth)>=.8660254038;
                }
            }
        }
        for(int i=0;i<3;++i){auto& s=stats[i];printf("%.3f,%.3f,%.3f,%d,%d,%s,16,%zu,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f\n",pan,dx,contrast,noise,camera,i==0?"match":i==1?"median":"smooth",s.count,s.error/s.count,s.mag/s.count,double(s.aligned)/s.count,double(s.zero)/s.count,s.sx/s.count,s.sy/s.count);}
    }
    return FAILED(g.device->GetDeviceRemovedReason())?7:0;
}
