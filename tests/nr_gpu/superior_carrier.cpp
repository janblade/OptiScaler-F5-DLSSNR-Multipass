#include "pch.h"
#include "interpass_fixture.h"

int main(int argc,char** argv) {
    try {
        if(argc<2 || argc>3)throw std::runtime_error("Usage: superior_carrier shader-dir [--warp]");
        Runner r(argc!=3 || std::string(argv[2])!="--warp"); // No proprietary NGX required.
        auto legacy=r.pipeline(argv[1],"DlssNr");
        auto fixed=r.pipeline(argv[1],"dlssnr_superior_carrier");
        auto resolve=r.pipeline(argv[1],"dlssnr_superior_resolve");
        auto cross=r.pipeline(argv[1],"dlssnr_superior_cross5");
        unsigned checks=0;
        for(auto dims : {std::pair<UINT,UINT>{1,1},{17,9},{65,37}})
        for(bool half : {false,true})
        for(int fixture=0;fixture<4;++fixture) {
            const std::vector<uint32_t> unit=half?std::vector<uint32_t>{0x3c003c00u,0x3c003c00u}:
                std::vector<uint32_t>{0x3f800000u,0x3f800000u,0x3f800000u,0x3f800000u};
            DlssNrConstants c{};c.Width=dims.first;c.Height=dims.second;
            c.Mode=22;c.Transfer=1;c.ResidualScale=1;c.WhitePoint=-1;c.DebugScale=2;
            const auto before=r.run(legacy.Get(),c.Width,c.Height,c.Width,c.Height,c,fixture,half,
                8,0,0,nullptr,0,&unit,1,1);
            c.Mode=37;
            const auto after=r.run(fixed.Get(),c.Width,c.Height,c.Width,c.Height,c,fixture,half);
            if(before!=after) {
                for(size_t i=0;i<before.size();++i)if(before[i]!=after[i]) {
                    std::cerr<<"Encode mismatch "<<c.Width<<'x'<<c.Height<<" half="<<half<<" fixture="<<fixture
                        <<" word="<<i<<" legacy="<<std::hex<<before[i]<<" fixed="<<after[i]<<std::dec<<'\n';break;
                }
                throw std::runtime_error("Fixed carrier differs from legacy linear K=1 encode");
            }
            c.Mode=23;
            const auto beforeDecode=r.run(legacy.Get(),c.Width,c.Height,c.Width,c.Height,c,fixture,half,
                8,0,0,nullptr,0,&unit,1,1);
            c.Mode=38;
            const auto afterDecode=r.run(fixed.Get(),c.Width,c.Height,c.Width,c.Height,c,fixture,half);
            if(beforeDecode!=afterDecode)throw std::runtime_error("Fixed carrier differs from legacy K=1 decode");
            checks+=2;
        }
        // Same Cross5 field with confidence disabled: specialization must preserve its output.
        for(UINT mode : {1u,36u})
        for(bool half : {false,true}) {
            DlssNrConstants c{};c.Mode=mode;c.Width=17;c.Height=9;
            c.Transfer=9;c.Passthrough=1;c.WhitePoint=1;c.MaxRatio=2;c.MaxDarkening=100;
            c.ResidualHistoryValid=1;c.ResidualScale=3;c.ResidualBlend=.015f;c.ResidualConfidenceSensitivity=1;
            c.DirectResolveFlags=65536|16|64;c.GuideWidth=1;c.MvScaleX=.8f;c.MvScaleY=.609f;
            const auto a=r.run(legacy.Get(),33,17,17,9,c,0,half);
            const auto b=r.run(mode==1?resolve.Get():cross.Get(),33,17,17,9,c,0,half);
            if(a!=b)throw std::runtime_error("Shadow-free shaping specialization changed output");
            ++checks;
        }
        std::cout<<"Superior GPU parity PASS: "<<checks<<" encode/decode/shaping cases, signed HDR, NaN, FP16/FP32 and edges\n";
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
