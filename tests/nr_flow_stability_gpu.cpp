// Stage attribution for F5Low: known backward motion, separate border bands,
// optional same-dispatch candidate/LK trace. No NR output is used as flow truth.
#define main UnusedFlowRegressionMain
#include "nr_optical_flow_gpu.cpp"
#undef main
#include "nr_motion_readback.h"
#include "../OptiScaler/motion/TrustMask_Dx12.h"
#include <array>
#include <map>

static constexpr auto kRead=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
static constexpr auto kCombined=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
static std::vector<float> Float4(Gpu& g,ID3D12Resource* r){auto b=ReadMotionBytes(g,r,16,kRead);std::vector<float> f(b.size()/4);memcpy(f.data(),b.data(),b.size());return f;}
struct Stats {
    std::vector<double> errors;double sum=0;unsigned lost=0;double jitter=0;unsigned jitterN=0,aligned=0,opposed=0,zero=0,moving=0;
    void Add(double e){errors.push_back(e);sum+=e;lost+=e>1.;}
    void Direction(float vx,float vy,float tx,float ty){ double a=std::hypot(vx,vy),b=std::hypot(tx,ty);if(b<.05)return;++moving;if(a<.05){++zero;return;}double cosine=(vx*tx+vy*ty)/(a*b);aligned+=cosine>=.8660254038;opposed+=cosine<0;}
    void Print(const char* scene,const char* stage,const char* band){
        if(errors.empty())return;std::sort(errors.begin(),errors.end());
        printf("%s,%s,%s,%zu,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f\n",scene,stage,band,errors.size(),sum/errors.size(),errors[errors.size()/2],errors[errors.size()*95/100],double(lost)/errors.size(),moving?double(aligned)/moving:0,moving?double(opposed)/moving:0,moving?double(zero)/moving:0,jitterN?jitter/jitterN:0);
    }
};
int main(int argc,char** argv){
    kWidth=641;kHeight=361;setvbuf(stdout,nullptr,_IONBF,0);
    Gpu gpu;if(!gpu.Init())return 2;
    unsigned flags=argc>1?unsigned(atoi(argv[1])):1;
    printf("scene,stage,band,samples,EPE,p50,p95,over1px,aligned30,opposed,zero,jitter\n");
    struct Case{const char* name;float panx,pany,noise;int scene;bool square,depth;};
    const Case cases[]={
        {"pan-horizon-no-depth",.7f,.3f,0,3,false,false},
        {"creep-horizon",.3f,.12f,0,3,false,false},
        {"fast-horizon",7.3f,-3.1f,0,3,false,false},
        {"static-clean-horizon",0,0,0,3,false,false},
        {"textured",.7f,.3f,0,0,false,false},
        {"static-horizon-grain",0,0,3,3,false,false},
        {"independent-no-depth",.7f,.3f,0,0,true,false},
        {"independent-depth",.7f,.3f,0,0,true,true},
        {"sparse",.7f,.3f,0,1,false,false},
        {"slow-lines",.5f,0,0,2,false,false}
    };
    for(const auto& c:cases){
        OpticalFlowDx12 flow;TrustMaskDx12 trust;
        if(!flow.Init(gpu.device.Get())||!trust.Init(gpu.device.Get()))return 3;
        flow.Tuning().inverseRefinement=argc>2&&atoi(argv[2])!=0;flow.Tuning().perceptualLuma=argc<4||atoi(argv[3])!=0;flow.Tuning().trace=true;flow.Tuning().reuseCandidateCost=(flags&1)!=0;
        flow.Tuning().refinementAcceptance=(flags&2)!=0;flow.Tuning().evidenceFilter=(flags&4)!=0;
        std::map<std::string,std::array<Stats,3>> stats;std::array<unsigned long long,64> origins{};
        std::map<std::string,std::vector<float>> previous;
        unsigned long long nonfinite=0,saturated=0,outside=0,integerGoodRefinedBad=0,refinedGoodIntegerBad=0,negativeTensor=0,negativeStructure=0;
        for(int n=0;n<18;++n){
            float sx=240+n*-1.2f,sy=150+n*.6f,ss=80;
            auto picture=gpu.Picture(c.panx*n,c.pany*n,1,c.noise,n,sx,sy,c.square?ss:0,c.scene,0,1,.05f);
            ComPtr<ID3D12Resource> depth;if(c.depth)depth=gpu.DepthMap(sx,sy,ss);
            if(!flow.Dispatch(gpu.list.Get(),picture.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,depth.Get(),c.depth?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_UNKNOWN,true))return 4;
            gpu.Submit();if(!flow.FlowValid())continue;
            TrustMaskDx12::Inputs in;in.flow=flow.Flow();in.flowWidth=flow.FlowWidth();in.flowHeight=flow.FlowHeight();
            in.fullPerFlow=float(kWidth)/flow.FlowWidth();in.lumaNow=flow.LumaOfLastFrame();in.lumaBefore=flow.LumaOfFrameBefore();in.sceneCut=flow.SceneCutFlag();
            if(depth){in.depthCount=1;in.depths[0]=depth.Get();in.depthFormat=DXGI_FORMAT_R32_FLOAT;in.depthWidth=kWidth;in.depthHeight=kHeight;in.depthReversed=true;}
            if(!trust.Dispatch(gpu.list.Get(),in)||!trust.BuildGuides(gpu.list.Get(),in,kWidth,kHeight))return 5;
            gpu.Submit();if(n<4)continue;
            auto structureBytes=ReadMotionBytes(gpu,flow.MatchedFlow(),8,kRead);for(size_t si=0;si<structureBytes.size();si+=8){uint16_t h;memcpy(&h,structureBytes.data()+si+4,2);negativeStructure+=Half(h)<0;}
            auto candidates=Float4(gpu,flow.CandidateTrace());auto refined=Float4(gpu,flow.RefinementTrace());auto events=Float4(gpu,flow.EventTrace());
            std::vector<std::pair<std::string,ID3D12Resource*>> fields={{"match",flow.MatchedFlow()},{"median",flow.MedianFlow()},{"smooth",flow.Flow()},{"guide",trust.GuideMotion()},{"integer",nullptr}};
            for(int level=1;level<OpticalFlowDx12::kLevels;++level)fields.push_back({"matchL"+std::to_string(level),flow.LevelFlow(level)});
            for(const auto& item:fields){
                bool full=std::string(item.first)=="guide",integer=item.second==nullptr;
                unsigned w=integer?flow.FlowWidth():unsigned(item.second->GetDesc().Width),h=integer?flow.FlowHeight():item.second->GetDesc().Height;
                auto f=integer?std::vector<float>{}:ReadFlow(gpu,item.second,item.second==flow.Flow()?kCombined:kRead);
                float units=item.first=="match"?2.f:item.first.starts_with("matchL")?float(2u<<unsigned(item.first.back()-'0')):1.f;
                unsigned mx=std::max(1u,std::min(16u,w/8)),my=std::max(1u,std::min(16u,h/8));
                for(unsigned y=my;y+my<h;++y)for(unsigned x=mx;x+mx<w;++x){
                    float px=(x+.5f)*kWidth/w,py=(y+.5f)*kHeight/h;
                    bool own=c.square&&px>=sx&&px<sx+ss&&py>=sy&&py<sy+ss;
                    bool was=c.square&&px>=sx+1.2f&&px<sx+ss+1.2f&&py>=sy-.6f&&py<sy+ss-.6f;
                    if(c.square&&!own&&was)continue; // newly revealed: no correspondence existed
                    float tx=own?1.2f:-c.panx,ty=own?-.6f:-c.pany;
                    float dx=std::max({sx-px,0.f,px-sx-ss}),dy=std::max({sy-py,0.f,py-sy-ss});
                    float edge=c.square?(own?std::min({px-sx,sx+ss-px,py-sy,sy+ss-py}):std::hypot(dx,dy)):fabsf(py-(.4f*kHeight+c.pany*n));
                    unsigned band=c.square?(edge<4?0:edge<12?1:2):(py<.3f*kHeight?0:py>.5f*kHeight?2:1);
                    size_t p=size_t(y)*w+x;float vx,vy;
                    if(integer){vx=refined[p*4]*2;vy=refined[p*4+1]*2;}else{vx=f[p*2]*units;vy=f[p*2+1]*units;}
                    auto& st=stats[item.first][band];st.Add(std::hypot(vx-tx,vy-ty));st.Direction(vx,vy,tx,ty);if(!integer&&!c.square&&previous[item.first].size()==f.size()){auto& old=previous[item.first];st.jitter+=std::hypot(vx-old[p*2]*units,vy-old[p*2+1]*units);++st.jitterN;}
                    if(integer){
                        ++origins[unsigned(candidates[p*4])&63u];negativeTensor+=events[p*4+1]<0;unsigned ev=unsigned(events[p*4+3]);nonfinite+=(ev&1)!=0;outside+=(ev&2)!=0;saturated+=unsigned(events[p*4+2]);
                        auto ei=std::hypot(vx-tx,vy-ty),er=std::hypot(vx+refined[p*4+2]*2-tx,vy+refined[p*4+3]*2-ty);
                        integerGoodRefinedBad+=ei<=.5&&er>1.;refinedGoodIntegerBad+=er<=.5&&ei>1.;
                    }
                }
                if(!integer)previous[item.first]=std::move(f);
            }
        }
        for(auto& stage:stats)for(unsigned b=0;b<3;++b)stage.second[b].Print(c.name,stage.first.c_str(),c.square?(b==0?"0-4px":b==1?"4-12px":">12px"):(b==0?"upper":b==1?"middle":"lower"));
        unsigned long long zero=0,global=0,coarse=0,history=0,local=0;
        for(unsigned k=0;k<64;++k){unsigned origin=k%32u;zero+=origin==0?origins[k]:0;global+=origin==1?origins[k]:0;coarse+=origin>=2&&origin<=10?origins[k]:0;history+=origin==11?origins[k]:0;local+=k>=32?origins[k]:0;}
        printf("origins,%s,zero=%llu,global=%llu,coarse=%llu,history=%llu,local_search=%llu\n",c.name,zero,global,coarse,history,local);
        printf("trace,%s,nonfinite=%llu,saturated_steps=%llu,outside=%llu,integerGood_LKbad=%llu,integerBad_LKgood=%llu,negative_tensor=%llu,negative_structure=%llu\n",c.name,nonfinite,saturated,outside,integerGoodRefinedBad,refinedGoodIntegerBad,negativeTensor,negativeStructure);
    }
    return FAILED(gpu.device->GetDeviceRemovedReason())?6:0;
}
