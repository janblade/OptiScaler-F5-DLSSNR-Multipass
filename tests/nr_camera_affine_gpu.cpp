// Independent affine-warp truth, including weak texture and a stationary HUD.
#define main UnusedFlowRegressionMain
#include "nr_optical_flow_gpu.cpp"
#undef main
#include "nr_motion_readback.h"
#include "../OptiScaler/native/F5LowShaderBytecode.h"
#include <array>

struct Warp {float angle,zoom,dx,dy;};
static std::array<float,2> World(float x,float y,int frame,const Warp& w){
    float a=w.angle*frame,z=std::exp(w.zoom*frame),c=cosf(a),s=sinf(a);
    x-=kWidth*.5f+w.dx*frame;y-=kHeight*.5f+w.dy*frame;
    return {(c*x+s*y)/z+kWidth*.5f,(-s*x+c*y)/z+kHeight*.5f};
}
static std::array<float,2> Screen(float x,float y,int frame,const Warp& w){
    float a=w.angle*frame,z=std::exp(w.zoom*frame),c=cosf(a),s=sinf(a);
    x-=kWidth*.5f;y-=kHeight*.5f;
    return {z*(c*x-s*y)+kWidth*.5f+w.dx*frame,z*(s*x+c*y)+kHeight*.5f+w.dy*frame};
}
static ComPtr<ID3D12Resource> Frame(Gpu& g,int n,const Warp& w,int scene,bool hud){
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
    for(unsigned y=0;y<kHeight;++y)for(unsigned x=0;x<kWidth;++x){
        auto p=World(float(x),float(y),n,w);auto dst=data+y*fp.Footprint.RowPitch+4*x;
        for(int ch=0;ch<3;++ch){
            float v=scene==3?LayoutScene(p[0],p[1],ch,false):Scene(p[0],p[1],ch);
            if(hud&&InHud(float(x),float(y)))v=InHudFill(float(x),float(y))?.55f:.15f;
            dst[ch]=uint8_t(std::clamp(v*255.f+.5f,0.f,255.f));
        }dst[3]=255;
    }
    upload->Unmap(0,nullptr);D3D12_TEXTURE_COPY_LOCATION src{},dst{};
    src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
    dst.pResource=tex.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={tex.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    g.list->ResourceBarrier(1,&b);g.Submit();return tex;
}
int main(int argc,char** argv){
    kWidth=641;kHeight=361;setvbuf(stdout,nullptr,_IONBF,0);Gpu g;if(!g.Init())return 2;
    puts("case,scene,hud,region,samples,EPE,over1px,aligned30,opposed");
    struct Case{const char* name;Warp w;};
    for(const auto& c:std::array<Case,3>{{{"rotate",{.004f,0,0,0}},{"zoom",{0,.004f,0,0}},{"combined",{.003f,.002f,.7f,.3f}}}})
    for(int scene:{0,3})for(bool hud:{false,true}){
        OpticalFlowDx12 f;if(!f.Init(g.device.Get()))return 3;
        double sum[3]{};unsigned count[3]{},lost[3]{},aligned[3]{},opposed[3]{},moving[3]{};
        for(int n=0;n<22;++n){
            auto picture=Frame(g,n,c.w,scene,hud);if(!picture)return 4;
            if(!f.Dispatch(g.list.Get(),picture.Get(),DXGI_FORMAT_R8G8B8A8_UNORM))return 5;
            g.Submit();if(n<5||!f.FlowValid())continue;
            auto field=ReadFlow(g,f.Flow(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            auto d=f.Flow()->GetDesc();
            for(unsigned y=10;y+10<d.Height;++y)for(unsigned x=10;x+10<d.Width;++x){
                float px=(x+.5f)*kWidth/d.Width,py=(y+.5f)*kHeight/d.Height;
                bool in=hud&&InHud(px,py);
                if(hud&&!in&&InHud(px+5,py+5))continue;
                if(in&&(!InHudFill(px-5,py-5)||!InHudFill(px+5,py+5)))continue;
                auto world=World(px,py,n,c.w),before=Screen(world[0],world[1],n-1,c.w);
                if(before[0]<10||before[1]<10||before[0]>kWidth-10||before[1]>kHeight-10)continue;
                if(hud&&!in&&InHud(before[0],before[1]))continue;
                unsigned region=in?2:world[1]<.3f*kHeight?0:world[1]>.5f*kHeight?1:3;if(region==3)continue;
                float tx=in?0:before[0]-px,ty=in?0:before[1]-py;
                auto p=(size_t(y)*d.Width+x)*2;double vx=field[p],vy=field[p+1];
                double e=hypot(vx-tx,vy-ty);if(!std::isfinite(e))return 6;
                sum[region]+=e;++count[region];lost[region]+=e>1;
                double mag=hypot(vx,vy),truth=hypot(tx,ty);
                if(truth>.05){++moving[region];aligned[region]+=mag>.05&&(vx*tx+vy*ty)/(mag*truth)>=.8660254;opposed[region]+=vx*tx+vy*ty<0;}
            }
        }
        for(unsigned r=0;r<3;++r)if(count[r])printf("%s,%d,%d,%s,%u,%.7f,%.7f,%.7f,%.7f\n",c.name,scene,hud?1:0,r==0?"upper":r==1?"lower":"hud",count[r],sum[r]/count[r],double(lost[r])/count[r],moving[r]?double(aligned[r])/moving[r]:0,moving[r]?double(opposed[r])/moving[r]:0);
    }
    return FAILED(g.device->GetDeviceRemovedReason())?7:0;
}
