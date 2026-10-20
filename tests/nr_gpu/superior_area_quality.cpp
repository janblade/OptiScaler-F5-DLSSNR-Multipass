#include "pch.h"
#include "superior_input_fixture.h"
#include "superior_numeric_metrics.h"
#include "../../OptiScaler/dlssnr/DlssNr_SuperiorScaling.h"

int main(int argc,char** argv) {
    try {
        if(argc!=3 && argc!=4)throw std::runtime_error("Usage: superior_area_quality baseline-dir candidate-dir [--4k]");
        const bool full=argc==4 && std::string(argv[3])=="--4k";if(argc==4 && !full)throw std::runtime_error("Unknown profile");
        Runner r(true,32);auto standard=r.pipeline(argv[1],"DlssNr"),encode=r.pipeline(argv[1],"dlssnr_superior_encode"),area=r.pipeline(argv[2],"dlssnr_superior_area");
        SuperiorNumericMetrics metrics[2];
        struct Geometry { UINT nw,nh,ww,wh; };unsigned cases=0;size_t words=0;
        const std::vector<Geometry> sizes=full?std::vector<Geometry>{{3840,2160,1920,1080},{3840,2160,2266,1274},{3840,2160,2496,1404}}:
            std::vector<Geometry>{{1,1,1,1},{3,5,1,2},{17,9,9,5},{65,37,33,19},{129,73,84,47},{65,37,65,19}};
        for(auto d : sizes)
        for(bool half : {false,true})for(bool halfSource : {false,true})for(UINT pass : {0u,1u})for(UINT mapping=0;mapping<(full?1u:5u);++mapping)
        for(UINT exposure=0;exposure<(full?1u:4u);++exposure)for(int fixture=0;fixture<(full?1:4);++fixture) {
            DlssNrConstants c{};c.Passthrough=pass;c.ReversibleMode=mapping;c.WhitePoint=2;c.PreExposure=1;
            c.ExposureMode=exposure==0?0:exposure==1?3:1;c.ExposureTrim=exposure==1?5.0f:1.0f;
            c.ExposureAnchorCount=exposure==2?8:0;for(UINT i=0;i<8;++i) { c.ExposureAnchors[i*2]=.125f*std::pow(2.0f,float(i));c.ExposureAnchors[i*2+1]=.5f+.125f*i; }
            if(exposure==3)c.WhitePoint=1.0e-5f;
            c.Mode=0;c.Width=d.nw;c.Height=d.nh;c.DirectResolveFlags=DlssNr::SuperiorScaling::ShaderFlag;
            if(!DlssNr::SuperiorScaling::InputEncode(c,d.nw,d.nh))throw std::runtime_error("Production input selector rejected valid input");
            InputFixture f(r,d.nw,d.nh,d.ww,d.wh,c,fixture,half,halfSource);
            DlssNrConstants down{};down.Mode=2;down.Width=d.ww;down.Height=d.wh;down.DirectResolveFlags=DlssNr::SuperiorScaling::ShaderFlag;
            const bool selected=DlssNr::SuperiorScaling::AreaDownsample(down,d.nw,d.nh);
            if(!selected && (d.nw!=d.ww || d.nh!=d.wh))throw std::runtime_error("Area selector rejected reduction");
            try {
                r.begin();f.roots();std::array<InputFixture::Read,2> reads;
                for(UINT i=0;i<2;++i){f.prepare(i,encode.Get(),i && selected?area.Get():standard.Get(),2);reads[i]=f.read(f.outputs[i].reduced.Get());}r.finish();
                auto a=f.words(reads[0]),b=f.words(reads[1]);
                metrics[half].compare(a,b,half);words+=a.size();
            }
            catch(...) { std::cerr<<"native="<<d.nw<<'x'<<d.nh<<" work="<<d.ww<<'x'<<d.wh<<" half="<<half<<" halfSource="<<halfSource<<" pass="<<pass<<" mapping="<<mapping<<" exposure="<<exposure<<" fixture="<<fixture<<'\n';throw; }
            ++cases;
        }
        for(int i=0;i<2;++i) {
            metrics[i].print("Area",i!=0);
            if(metrics[i].maxNormalized>(i?0.0011:0.00025))throw std::runtime_error("Area exceeded the numerical error budget");
        }
        std::cout<<"Superior Area numerical audit PASS: "<<cases<<" cases, "<<words<<" words\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
