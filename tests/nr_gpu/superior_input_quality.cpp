#include "pch.h"
#include "superior_input_fixture.h"
#include "../../OptiScaler/dlssnr/DlssNr_SuperiorScaling.h"

int main(int argc,char** argv) {
    try {
        if(argc!=3)throw std::runtime_error("Usage: superior_input_quality baseline-dir candidate-dir");
        Runner r(true,32);auto standard=r.pipeline(argv[1],"DlssNr"),encode=r.pipeline(argv[2],"dlssnr_superior_encode");
        struct Geometry { UINT nw,nh,ww,wh; };unsigned cases=0;size_t words=0;
        for(auto d : {Geometry{1,1,1,1},{3,5,1,2},{17,9,9,5},{65,37,33,19},{129,73,84,47},{65,37,65,19}})
        for(bool half : {false,true})for(bool halfSource : {false,true})for(UINT pass : {0u,1u})for(UINT mapping=0;mapping<5;++mapping)
        for(UINT exposure=0;exposure<4;++exposure)for(int fixture=0;fixture<4;++fixture) {
            DlssNrConstants c{};c.Passthrough=pass;c.ReversibleMode=mapping;c.WhitePoint=2;c.PreExposure=1;
            c.ExposureMode=exposure==0?0:exposure==1?3:1;c.ExposureTrim=exposure==1?5.0f:1.0f;
            c.ExposureAnchorCount=exposure==2?8:0;for(UINT i=0;i<8;++i) { c.ExposureAnchors[i*2]=.125f*std::pow(2.0f,float(i));c.ExposureAnchors[i*2+1]=.5f+.125f*i; }
            if(exposure==3)c.WhitePoint=1.0e-5f;
            c.Mode=0;c.Width=d.nw;c.Height=d.nh;c.DirectResolveFlags=DlssNr::SuperiorScaling::ShaderFlag;
            if(!DlssNr::SuperiorScaling::InputEncode(c,d.nw,d.nh))throw std::runtime_error("Production input selector rejected valid input");
            InputFixture f(r,d.nw,d.nh,d.ww,d.wh,c,fixture,half,halfSource);
            try { words+=f.quality(standard.Get(),encode.Get(),standard.Get()); }
            catch(...) { std::cerr<<"native="<<d.nw<<'x'<<d.nh<<" work="<<d.ww<<'x'<<d.wh<<" half="<<half<<" halfSource="<<halfSource<<" pass="<<pass<<" mapping="<<mapping<<" exposure="<<exposure<<" fixture="<<fixture<<'\n';throw; }
            ++cases;
        }
        std::cout<<"Superior input GPU parity PASS: "<<cases<<" cases, "<<words<<" words, encode/keep/Area, FP16/FP32, exposure/anchors, SDR/HDR and edges\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
