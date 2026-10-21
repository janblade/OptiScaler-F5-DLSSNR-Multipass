// Known camera motion over smooth sky/defocus, with independent per-frame grain.
// Reuses the production dispatch and readback helpers, not an alternate estimator.
#define main OriginalFlowMain
#include "nr_optical_flow_gpu.cpp"
#undef main
#include "nr_motion_readback.h"
#include <cstring>

static ComPtr<ID3D12Resource> SkyPicture(Gpu& gpu,int frame,float dx,float dy,int scene) {
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=kWidth;d.Height=kHeight;
    d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D12Resource> tex;
    gpu.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&tex));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes=0;
    gpu.device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
    auto upload=gpu.Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    unsigned char* data=nullptr;upload->Map(0,nullptr,(void**)&data);
    for(unsigned y=0;y<kHeight;++y)for(unsigned x=0;x<kWidth;++x) {
        float sx=x-frame*dx,sy=y-frame*dy;
        if((scene==3 || scene==4) && dx!=0) {
            float angle=frame*.004f,rx=float(x)-kWidth*.5f,ry=float(y)-kHeight*.5f;
            sx=std::cos(angle)*rx+std::sin(angle)*ry+kWidth*.5f;
            sy=-std::sin(angle)*rx+std::cos(angle)*ry+kHeight*.5f;
        }
        if(scene==5 && dx!=0){
            float angle=frame*.006f,nx=float(x)/kWidth*2-1,ny=float(y)/kHeight*2-1;
            float denominator=std::cos(angle)-std::sin(angle)*nx;
            sx=((std::cos(angle)*nx+std::sin(angle))/denominator+1)*kWidth*.5f;
            sy=(ny/denominator+1)*kHeight*.5f;
        }
        bool sky=sy<kHeight*.56f;
        float base=sky ? .65f+.00015f*sy : .1f+.5f*Scene(sx,sy,0);
        if(sky && (scene==1 || scene>=3))base+=.07f*(ValueNoise(sx,sy,55,97)-.5f)+.04f*(ValueNoise(sx,sy,110,39)-.5f);
        if(sky && scene==2)base+=.22f*(ValueNoise(sx,sy,30,91)-.5f)+.11f*(ValueNoise(sx,sy,70,11)-.5f);
        float noise=sky ? (Hash(x,y,frame+320)*2-1)*(scene==0?1.f:2.f)/255.f : 0;
        if(scene==4 && x>kWidth*.4f && x<kWidth*.8f && y>kHeight*.08f && y<kHeight*.24f){base=.2f;noise=0;}
        auto px=data+y*fp.Footprint.RowPitch+x*4;
        for(int c=0;c<3;++c)px[c]=(uint8_t)std::clamp((base+noise)*255+.5f,0.f,255.f);
        px[3]=255;
    }
    upload->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=tex.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
    gpu.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={tex.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    gpu.list->ResourceBarrier(1,&b);gpu.Submit();return tex;
}

int main(int argc,char** argv) {
    unsigned flags=argc>1?unsigned(atoi(argv[1])):1;
    setvbuf(stdout,nullptr,_IONBF,0);kWidth=960;kHeight=540;
    Gpu gpu;if(!gpu.Init())return 2;
    bool finite=true,quality=true;
    for(int scene=0;scene<6;++scene)for(bool pan:{false,true})for(int repeat=0;repeat<1;++repeat) {
        OpticalFlowDx12 flow;if(!flow.Init(gpu.device.Get()))return 3;
        flow.Tuning().sceneCutDetector=false;flow.Tuning().referenceMode=flags==0;flow.Tuning().reuseCandidateCost=(flags&1)!=0;
        flow.Tuning().refinementAcceptance=(flags&2)!=0;flow.Tuning().evidenceFilter=(flags&4)!=0;
        float dx=pan?5.5f:0,dy=pan?-1.25f:0;
        std::vector<double> errors;double conf=0,rough=0,ground=0;unsigned n=0,gn=0,rn=0,hudN=0,hudGood=0;
        for(int frame=0;frame<12;++frame) {
            auto pic=SkyPicture(gpu,frame,dx,dy,scene);
            if(!flow.Dispatch(gpu.list.Get(),pic.Get(),DXGI_FORMAT_R8G8B8A8_UNORM))return 4;
            auto f=ReadFlow(gpu,flow.Flow());auto c=ReadConfidence(gpu,flow.Flow());
            if(frame<4)continue;
            unsigned w=flow.FlowWidth(),h=flow.FlowHeight();
            for(unsigned y=12;y<h-12;++y)for(unsigned x=12;x<w-12;++x) {
                auto i=y*w+x;double expectedX=-dx,expectedY=-dy;
                if((scene==3 || scene==4) && pan) {
                    double rx=(x+.5)*2-kWidth*.5,ry=(y+.5)*2-kHeight*.5;
                    expectedX=(std::cos(.004)-1)*rx+std::sin(.004)*ry;
                    expectedY=-std::sin(.004)*rx+(std::cos(.004)-1)*ry;
                }
                if(scene==5 && pan){
                    double nx=(x+.5)*2/kWidth*2-1,ny=(y+.5)*2/kHeight*2-1;
                    double d=std::cos(.006)-std::sin(.006)*nx;
                    expectedX=((std::cos(.006)*nx+std::sin(.006))/d-nx)*kWidth*.5;
                    expectedY=(ny/d-ny)*kHeight*.5;
                }
                double err=std::hypot(f[i*2]-expectedX,f[i*2+1]-expectedY);finite&=std::isfinite(err);
                bool hud=scene==4 && x>w*.38f && x<w*.82f && y>h*.06f && y<h*.26f;
                if(scene==4 && frame>=9 && x>w*.44f && x<w*.76f && y>h*.11f && y<h*.21f){++hudN;hudGood+=std::hypot(f[i*2],f[i*2+1])<1;}
                if(y<h*.35f && !hud){errors.push_back(err);conf+=c[i];++n;
                    rough+=std::hypot(f[i*2]-f[(i+1)*2],f[i*2+1]-f[(i+1)*2+1]);++rn;}
                if(y>h*.70f){ground+=err;++gn;}
            }
        }
        std::sort(errors.begin(),errors.end());double total=0;unsigned outliers=0;
        for(double e:errors){total+=e;outliers+=e>3;}
        printf("scene%d %s flags%u sky mean %.5f p95 %.5f >3px %.3f%% rough %.5f conf %.5f ground %.5f\n",scene,pan?"pan":"still",flags,total/n,errors[errors.size()*95/100],100.*outliers/n,rough/rn,conf/n,ground/gn);
        // Record ambiguous sky/rotation errors; compare to the base, without inheriting obsolete camera-model gates.
        if(hudN){printf("static HUD after warmup: within 1 px %.3f%%\n",100.*hudGood/hudN);quality&=hudGood>.9*hudN;}
    }
    printf("finite sky/rotation fields and static-HUD gate: %s\n",finite&&quality?"PASS":"FAIL");
    return finite&&quality?0:5;
}
