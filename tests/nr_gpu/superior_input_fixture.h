#pragma once
#include "interpass_fixture.h"
#include <cmath>
#include <iomanip>

// Encode two native outputs, then Area the materialized proxy. No fused arithmetic or lost FP16 store.
struct InputFixture {
    Runner& r;UINT nw,nh,ww,wh;
    ComPtr<ID3D12Resource> source,exposure,encodeCb,areaCb,times;
    struct Outputs { ComPtr<ID3D12Resource> color,keep,reduced; } outputs[2];
    ComPtr<ID3D12QueryHeap> queries;UINT64 frequency;
    // Test-data conversion only: IEEE binary16 round-to-nearest/even, with canonical NaN.
    static uint16_t halfBits(float value) {
        uint32_t bits;std::memcpy(&bits,&value,4);const uint32_t sign=(bits>>16)&0x8000u,mantissa=bits&0x7fffffu;
        const uint32_t exponent=(bits>>23)&255u;if(exponent==255u)return uint16_t(sign|(mantissa?0x7e00u:0x7c00u));
        const int e=int(exponent)-112;if(e>=31)return uint16_t(sign|0x7c00u);
        if(e<=0) {
            if(e< -10)return uint16_t(sign);const uint32_t m=mantissa|0x800000u,shift=uint32_t(14-e);
            const uint32_t base=m>>shift,rem=m&((1u<<shift)-1u),mid=1u<<(shift-1);
            return uint16_t(sign|(base+(rem>mid || (rem==mid && (base&1u)))));
        }
        const uint32_t base=(uint32_t(e)<<10)|(mantissa>>13),rem=mantissa&8191u;
        return uint16_t(sign|(base+(rem>4096u || (rem==4096u && (base&1u)))));
    }
    InputFixture(Runner& runner,UINT nativeW,UINT nativeH,UINT workW,UINT workH,
                 const DlssNrConstants& constants,int fixture,bool half,bool halfSource=false):r(runner),nw(nativeW),nh(nativeH),ww(workW),wh(workH) {
        r.begin();std::vector<ComPtr<ID3D12Resource>> uploads;
        auto input=[&](UINT w,UINT h,bool meter) {
            D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
            d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
            if(halfSource && !meter)d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
            auto texture=r.resource(d,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes;r.device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
            auto upload=r.buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);char* data;
            check(upload->Map(0,nullptr,reinterpret_cast<void**>(&data)));
            for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x) {
                float pixel[4];const float u=(x+.5f)/w,v=(y+.5f)/h;
                for(UINT k=0;k<4;++k) {
                    pixel[k]=meter?.5f:k==3?.7f:.4f+.5f*std::sin(7*u+9*v+k)+.1f*std::cos(15*v-17*u+k);
                    if(fixture==1)pixel[k]*=1.0e-4f;
                    if(fixture==2)pixel[k]*=1.0e4f;
                    if(fixture==3 && (x+y+k)%5==0) {
                        const uint32_t special[]={0x7fc00001u,0x7f800000u,0xff800000u,0x80000000u,1u,0x7f7fffffu};
                        const auto bits=special[(x*3+y+k)%6];std::memcpy(&pixel[k],&bits,4);
                    }
                }
                if(halfSource && !meter) {
                    uint16_t packed[4];for(UINT k=0;k<4;++k)packed[k]=halfBits(pixel[k]);
                    std::memcpy(data+y*fp.Footprint.RowPitch+x*8,packed,8);
                } else std::memcpy(data+y*fp.Footprint.RowPitch+x*16,pixel,16);
            }
            upload->Unmap(0,nullptr);uploads.push_back(upload);
            D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=upload.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;a.PlacedFootprint=fp;
            b.pResource=texture.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;r.list->CopyTextureRegion(&b,0,0,0,&a,nullptr);
            r.barrier(texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);return texture;
        };
        source=input(nw,nh,false);exposure=input(1,1,true);r.finish();
        auto output=[&](UINT w,UINT h) {
            D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
            d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            d.Format=half?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT;
            return r.resource(d,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        };
        for(UINT i=0;i<2;++i) {
            auto& o=outputs[i];o.color=output(nw,nh);o.keep=output(nw,nh);o.reduced=output(ww,wh);
            table(i*16,source.Get(),o.color.Get(),o.keep.Get());table(i*16+8,o.color.Get(),o.reduced.Get(),o.reduced.Get());
        }
        auto c=constants;c.Mode=0;c.Width=nw;c.Height=nh;encodeCb=cb(c);
        c={};c.Mode=2;c.Transfer=0;c.Width=ww;c.Height=wh;areaCb=cb(c);
        D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=32;
        check(r.device->CreateQueryHeap(&q,IID_PPV_ARGS(&queries)));
        times=r.buffer(32*sizeof(UINT64),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);check(r.queue->GetTimestampFrequency(&frequency));
    }
    void table(UINT offset,ID3D12Resource* input,ID3D12Resource* target,ID3D12Resource* keep) {
        const auto cpu=r.heap->GetCPUDescriptorHandleForHeapStart();
        for(UINT i=0;i<6;++i) {
            auto* texture=i==3?exposure.Get():input;D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=texture->GetDesc().Format;
            d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;
            r.device->CreateShaderResourceView(texture,&d,{cpu.ptr+SIZE_T(offset+i)*r.stride});
        }
        for(UINT i=6;i<8;++i) {
            auto* texture=i==6?target:keep;D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=texture->GetDesc().Format;d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            r.device->CreateUnorderedAccessView(texture,nullptr,&d,{cpu.ptr+SIZE_T(offset+i)*r.stride});
        }
    }
    ComPtr<ID3D12Resource> cb(const DlssNrConstants& c) {
        auto out=r.buffer(sizeof(c),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* p;
        check(out->Map(0,nullptr,&p));std::memcpy(p,&c,sizeof(c));out->Unmap(0,nullptr);return out;
    }
    void roots() { r.list->SetComputeRootSignature(r.root.Get());ID3D12DescriptorHeap* heaps[]={r.heap.Get()};r.list->SetDescriptorHeaps(1,heaps); }
    void dispatch(ID3D12PipelineState* pso,ID3D12Resource* c,UINT table,UINT w,UINT h,UINT groupY=8) {
        auto gpu=r.heap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=UINT64(table)*r.stride;r.list->SetPipelineState(pso);
        r.list->SetComputeRootDescriptorTable(0,gpu);r.list->SetComputeRootConstantBufferView(1,c->GetGPUVirtualAddress());r.list->Dispatch((w+7)/8,(h+groupY-1)/groupY,1);
    }
    void prepare(UINT i,ID3D12PipelineState* encode,ID3D12PipelineState* area,UINT kind) {
        auto& o=outputs[i];
        if(kind!=1) {
            dispatch(encode,encodeCb.Get(),i*16,nw,nh);
            for(auto* t : {o.color.Get(),o.keep.Get()})r.barrier(t,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if(kind!=0) {
            dispatch(area,areaCb.Get(),i*16+8,ww,wh);
            r.barrier(o.reduced.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }
    void rest(UINT i,UINT kind) {
        auto& o=outputs[i];if(kind!=1)for(auto* t : {o.color.Get(),o.keep.Get()})r.barrier(t,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if(kind!=0)r.barrier(o.reduced.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    void seed(ID3D12PipelineState* standard) {
        r.begin();roots();for(UINT i=0;i<2;++i) {
            dispatch(standard,encodeCb.Get(),i*16,nw,nh);
            r.barrier(outputs[i].color.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }r.finish();
    }
    struct Read { ComPtr<ID3D12Resource> buffer;D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot{};UINT w,h,bytes; };
    Read read(ID3D12Resource* source) {
        Read out{};const auto d=source->GetDesc();UINT64 size;r.device->GetCopyableFootprints(&d,0,1,0,&out.foot,nullptr,nullptr,&size);
        out.w=UINT(d.Width);out.h=d.Height;out.bytes=d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:16;
        out.buffer=r.buffer(size,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        r.barrier(source,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=source;a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        b.pResource=out.buffer.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint=out.foot;r.list->CopyTextureRegion(&b,0,0,0,&a,nullptr);
        r.barrier(source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);return out;
    }
    std::vector<uint32_t> words(Read& value) {
        std::vector<uint32_t> out(size_t(value.w)*value.h*value.bytes/4);void* data;check(value.buffer->Map(0,nullptr,&data));
        for(UINT y=0;y<value.h;++y)std::memcpy(out.data()+size_t(y)*value.w*value.bytes/4,static_cast<char*>(data)+y*value.foot.Footprint.RowPitch,value.w*value.bytes);
        value.buffer->Unmap(0,nullptr);return out;
    }
    size_t quality(ID3D12PipelineState* standard,ID3D12PipelineState* encode,ID3D12PipelineState* area) {
        r.begin();roots();prepare(0,standard,standard,2);
        std::array<Read,3> a={read(outputs[0].color.Get()),read(outputs[0].keep.Get()),read(outputs[0].reduced.Get())};rest(0,2);
        prepare(1,encode,area,2);
        std::array<Read,3> b={read(outputs[1].color.Get()),read(outputs[1].keep.Get()),read(outputs[1].reduced.Get())};rest(1,2);r.finish();
        size_t total=0;for(UINT field=0;field<3;++field) {
            const auto left=words(a[field]),right=words(b[field]);if(left!=right) {
                for(size_t j=0;j<left.size();++j)if(left[j]!=right[j]) { std::cerr<<"Input mismatch field="<<field<<" word="<<j<<" before="<<std::hex<<left[j]<<" after="<<right[j]<<std::dec<<'\n';break; }
                throw std::runtime_error("Input preparation is not bit-exact");
            }total+=left.size();
        }return total;
    }
    std::vector<double> batch(UINT index,ID3D12PipelineState* encode,ID3D12PipelineState* area,UINT kind,UINT count) {
        r.begin();roots();for(UINT sample=0;sample<count;++sample) {
            r.list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,sample*2);
            for(UINT repeat=0;repeat<4;++repeat) { prepare(index,encode,area,kind);rest(index,kind); }
            r.list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,sample*2+1);
        }r.list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,count*2,times.Get(),0);r.finish();
        UINT64* p;check(times->Map(0,nullptr,reinterpret_cast<void**>(&p)));std::vector<double> out;
        for(UINT i=0;i<count;++i)out.push_back(double(p[i*2+1]-p[i*2])*1000/frequency/4);times->Unmap(0,nullptr);return out;
    }
};
