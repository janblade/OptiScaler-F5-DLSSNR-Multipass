#include "pch.h"
#include "superior_prepare_fixture.h"

int main(int argc,char** argv) {
    try {
        if(argc!=3)throw std::runtime_error("Usage: superior_prepare_quality baseline-dir candidate-dir");
        Runner r(true,32);auto standard=r.pipeline(argv[1],"DlssNr"),legacy=r.pipeline(argv[1],"dlssnr_superior_carrier");
        auto candidate=r.pipeline(argv[2],"dlssnr_superior_carrier");size_t total=0;unsigned checks=0;
        for(auto d : {std::pair<UINT,UINT>{1,1},{3,5},{17,9},{65,37},{129,73},{257,131}})
        for(auto scale : {std::pair<float,float>{1.0f,1.0f},{.75f,-.5f},{-1.5f,.003f},{0.0f,0.0f},{-0.0f,0.0f}})
        for(int fixture=0;fixture<4;++fixture)for(bool half : {false,true}) {
            const UINT dw=d.first+11,dh=d.second+13,mw=d.first*2+17,mh=d.second*2+19;
            PrepareFixture f(r,d.first,d.second,dw,dh,mw,mh,{3,5,dw-7,dh-9},{7,3,mw-11,mh-7},scale.first,scale.second,fixture,half);
            total+=f.quality(standard.Get(),legacy.Get(),candidate.Get());++checks;
        }
        std::cout<<"Superior preparation GPU parity PASS: "<<checks<<" cases, "<<total
            <<" words, FP16/FP32 carrier, R32 depth, RG32 motion, crops, odd sizes, signed/non-finite data and hold\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
