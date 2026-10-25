#include "pch.h"
#include "superior_input_fixture.h"
#include "superior_numeric_metrics.h"
#include "../../OptiScaler/dlssnr/DlssNr_SuperiorScaling.h"


// Exercise all binary16 classes/payloads and high-frequency signed inputs.
void fillHalf(InputFixture& f,int pattern) {
    auto& r=f.r;auto d=f.source->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes;
    r.device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
    auto upload=r.buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);char* p;
    check(upload->Map(0,nullptr,reinterpret_cast<void**>(&p)));
    const uint16_t special[]={0,0x8000,1,0x8001,0x3ff,0x83ff,0x400,0x8400,0x3c00,0xbc00,0x7bff,0xfbff,0x7c00,0xfc00,0x7c01,0x7e55};
    for(UINT y=0;y<f.nh;++y)for(UINT x=0;x<f.nw;++x)for(UINT k=0;k<4;++k) {
        uint32_t v=(x+1)*0x9e3779b9u^(y+1)*0x85ebca6bu^(k+3)*0xc2b2ae35u;v^=v>>16;v*=0x7feb352du;v^=v>>15;
        uint16_t h=pattern==4?uint16_t(v):pattern==5?special[(x+3*y+5*k)%16]:InputFixture::halfBits(float(int(v%4097)-2048)/2048.f);
        std::memcpy(p+y*fp.Footprint.RowPitch+(x*4+k)*2,&h,2);
    }
    upload->Unmap(0,nullptr);r.begin();
    r.barrier(f.source.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=upload.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;a.PlacedFootprint=fp;
    b.pResource=f.source.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;r.list->CopyTextureRegion(&b,0,0,0,&a,nullptr);
    r.barrier(f.source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);r.finish();
}

int main(int argc,char** argv) {
    try {
        if(argc!=3 && argc!=4)throw std::runtime_error("Usage: superior_tall_low_quality baseline candidate [--4k]");
        const UINT candidateY=DlssNr::SuperiorScaling::LowGroupHeight;
        const bool full=argc==4 && std::string(argv[3])=="--4k";
        if(argc==4 && !full)throw std::runtime_error("Unknown quality profile");
        Runner r(true,32);auto before=r.pipeline(argv[1],"dlssnr_superior_low"),after=r.pipeline(argv[2],"dlssnr_superior_low");
        unsigned checks=0;size_t total=0;SuperiorNumericMetrics metrics[2];
        const std::vector<std::pair<UINT,UINT>> sizes=full?std::vector<std::pair<UINT,UINT>>{{1920,1080},{2266,1274},{2496,1404}}:
            std::vector<std::pair<UINT,UINT>>{{1,1},{1,19},{23,1},{7,9},{16,16},{17,11},{65,37},{129,73},{257,143}};
        for(auto dims : sizes)
        for(bool halfSource : {false,true})for(bool halfTarget : {false,true})for(int stress=0;stress<7;++stress) {
            if(stress>=4 && !halfSource)continue; // Raw binary16 patterns apply to the native cache only.
            const UINT lw=(dims.first+7)/8,lh=(dims.second+7)/8;
            DlssNrConstants c{};c.Mode=18;c.Transfer=1;c.Width=lw;c.Height=lh;
            c.DirectResolveFlags=DlssNr::SuperiorScaling::ShaderFlag;
            if(!DlssNr::SuperiorScaling::LowResidual(c,dims.first,dims.second))throw std::runtime_error("Low selector failed");
            InputFixture f(r,dims.first,dims.second,lw,lh,c,std::min(stress,3),halfTarget,halfSource);
            if(stress>=4 && halfSource)fillHalf(f,stress);
            auto cb=f.cb(c);
            for(UINT i=0;i<2;++i)f.table(i*16+8,f.source.Get(),f.outputs[i].reduced.Get(),f.outputs[i].reduced.Get());
            r.begin();f.roots();std::array<InputFixture::Read,2> reads;
            for(UINT i=0;i<2;++i) {
                const bool mixed=i!=0;
                f.dispatch(mixed?after.Get():before.Get(),cb.Get(),i*16+8,lw,lh,
                           mixed?candidateY:16u);
                r.barrier(f.outputs[i].reduced.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                reads[i]=f.read(f.outputs[i].reduced.Get());
            }r.finish();
            const auto a=f.words(reads[0]),b=f.words(reads[1]);

            if(a!=b)throw std::runtime_error("Tall low is not bit exact at " + std::to_string(dims.first)+"x"+std::to_string(dims.second));
            metrics[halfTarget].compare(a,b,halfTarget);
            total+=a.size();++checks;
        }
        for(int i=0;i<2;++i) { metrics[i].print("Cached low",i!=0); }
        std::cout<<"Superior tall low GPU bit parity PASS: "<<checks<<" cases, "<<total<<" words, FP16/FP32 source and target, signed HDR/nonfinite data and edges\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
