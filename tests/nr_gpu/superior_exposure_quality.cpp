#include "pch.h"
#include "superior_exposure_fixture.h"

int main(int argc,char** argv) {
    try {
        if(argc!=3 && argc!=4)throw std::runtime_error("Usage: superior_exposure_quality baseline candidate [--4k]");
        const bool full=argc==4 && std::string(argv[3])=="--4k";
        if(argc==4 && !full)throw std::runtime_error("Unknown profile");
        Runner r(true,48);auto standard=r.pipeline(argv[1],"DlssNr");
        auto meter=r.pipeline(argv[2],"dlssnr_superior_meter"),exposure=r.pipeline(argv[2],"dlssnr_superior_exposure");
        auto encode=r.pipeline(argv[1],"dlssnr_superior_encode"),area=r.pipeline(argv[1],"dlssnr_superior_area");
        const std::vector<std::pair<UINT,UINT>> sizes=full?std::vector<std::pair<UINT,UINT>>{{3840,2160}}:
            std::vector<std::pair<UINT,UINT>>{{1,1},{3,5},{17,11},{65,37},{129,73},{257,143}};
        SuperiorNumericMetrics mean,white,images[2];double whiteRelative=0;unsigned cases=0;size_t words=0;
        for(auto size : sizes)for(bool halfSource : {false,true})for(bool halfOutput : {false,true})
        for(UINT scenario=0;scenario<(full?6u:30u);++scenario)for(int stress=0;stress<(full?1:4);++stress) {
            auto c=ExposureFixture::settings(float((scenario%3)*50));c.PreExposure=scenario%2?.125f:8;
            c.ReversibleMode=(scenario/3)%5;c.ExposureTrim=scenario%2?5:1;
            c.ExposureAnchorCount=scenario%5==0?8:0;
            for(UINT i=0;i<8;++i) { c.ExposureAnchors[i*2]=.125f*std::pow(2.f,float(i));c.ExposureAnchors[i*2+1]=.5f+.125f*i; }
            const UINT w=full?UINT(std::lround(size.first*(scenario<3?.5f:.65f))):std::max(1u,UINT(std::lround(size.first*.65f)));
            const UINT h=full?UINT(std::lround(size.second*(scenario<3?.5f:.65f))):std::max(1u,UINT(std::lround(size.second*.65f)));
            ExposureFixture f(r,size.first,size.second,w,h,halfSource,halfOutput,stress,c);
            std::array<std::vector<uint32_t>,5> before;
            for(UINT variant=0;variant<2;++variant) {
                r.begin();f.input.roots();f.prepare(variant && halfSource?meter.Get():standard.Get(),variant && halfSource?exposure.Get():standard.Get(),encode.Get(),area.Get());
                std::array<InputFixture::Read,5> reads={f.input.read(f.meter.Get()),f.input.read(f.white.Get()),
                    f.input.read(f.input.outputs[0].color.Get()),f.input.read(f.input.outputs[0].keep.Get()),f.input.read(f.input.outputs[0].reduced.Get())};
                f.rest();r.finish();
                for(UINT field=0;field<reads.size();++field) {
                    auto value=f.input.words(reads[field]);
                    if(variant==0)before[field]=std::move(value);
                    else {
                        if(field==0)mean.compare(before[field],value,false);
                        else if(field==1) {
                            white.compare(before[field],value,false);
                            const double a=SuperiorNumericMetrics::value(before[field],0,false),b=SuperiorNumericMetrics::value(value,0,false);
                            whiteRelative=std::max(whiteRelative,std::abs(a-b)/std::max(std::abs(a),1e-8));
                        } else images[halfOutput].compare(before[field],value,halfOutput);
                        words+=value.size();
                    }
                }
            }++cases;
        }
        mean.print("Meter",false);white.print("White",false);
        for(int i=0;i<2;++i)images[i].print("Proxy/HDR/Area",i!=0);
        std::cout<<"White relative="<<whiteRelative<<'\n';
        if(whiteRelative>1e-5 || images[0].maxNormalized>1e-5 || images[1].maxNormalized>.0011)
            throw std::runtime_error("Exposure numerical error exceeded budget");
        std::cout<<"Superior exposure numerical audit PASS: "<<cases<<" cases, "<<words<<" words\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
