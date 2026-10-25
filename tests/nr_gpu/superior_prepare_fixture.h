#pragma once
#include "interpass_fixture.h"
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Guides.h"
#include <cmath>
#include <iomanip>

// Real R32 depth, RG32 motion, FP16/FP32 carrier. Resident inputs and immutable descriptors/CBs.
struct PrepareFixture {
    Runner& r;UINT w,h;
    ComPtr<ID3D12Resource> depth,motion,proxy,answer,nrDepth,nrMotion,oldDepth,oldMotion,newMotion,oldCarrier,newCarrier;
    ComPtr<ID3D12Resource> nrCb,guideCb,carrierCb,fusedCb,times;
    ComPtr<ID3D12QueryHeap> queries;UINT64 frequency;
    PrepareFixture(Runner& runner,UINT width,UINT height,UINT dw,UINT dh,UINT mw,UINT mh,
                   DlssNr::GuideRegion dr,DlssNr::GuideRegion mr,float sx,float sy,int fixture,bool half):r(runner),w(width),h(height) {
        r.begin();std::vector<ComPtr<ID3D12Resource>> uploads;
        auto input=[&](UINT iw,UINT ih,DXGI_FORMAT format,int kind) {
            D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=iw;d.Height=ih;
            d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Format=format;
            auto texture=r.resource(d,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes;
            r.device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
            auto upload=r.buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            char* data;check(upload->Map(0,nullptr,reinterpret_cast<void**>(&data)));
            const UINT channels=kind==0?1:kind==1?2:4;
            for(UINT y=0;y<ih;++y)for(UINT x=0;x<iw;++x) {
                const float u=(x+.5f)/iw,v=(y+.5f)/ih;
                float pixel[4]={};
                for(UINT c=0;c<channels;++c) {
                    if(kind==0)pixel[c]=float((x*13+y*29)%1001)/1000;
                    else if(kind==1)pixel[c]=float(int((x*53+y*17+c*13)%4001)-2000)*.125f;
                    else pixel[c]=.4f+.25f*std::sin(7*u+9*v+c)+(kind==3?.03f*std::cos(19*u+17*v+c):0);
                    if(fixture==1)pixel[c]*=1.0e-4f;
                    if(fixture==2)pixel[c]*=1.0e30f;
                    if(fixture==3 && (x+y+c)%5==0) {
                        static const uint32_t special[]={0x7fc00001u,0x7f800000u,0xff800000u,0x80000000u,1u,0x7f7fffffu};
                        const auto bits=special[(x*3+y+c)%6];std::memcpy(&pixel[c],&bits,4);
                    }
                }
                std::memcpy(data+y*fp.Footprint.RowPitch+x*channels*4,pixel,channels*4);
            }
            upload->Unmap(0,nullptr);uploads.push_back(upload);
            D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
            dst.pResource=texture.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;r.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            r.barrier(texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);return texture;
        };
        depth=input(dw,dh,DXGI_FORMAT_R32_FLOAT,0);motion=input(mw,mh,DXGI_FORMAT_R32G32_FLOAT,1);
        proxy=input(w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,2);answer=input(w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,3);r.finish();
        auto output=[&](DXGI_FORMAT format) {
            D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
            d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Format=format;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            return r.resource(d,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        };
        nrDepth=output(DXGI_FORMAT_R32_FLOAT);oldDepth=output(DXGI_FORMAT_R32_FLOAT);
        nrMotion=output(DXGI_FORMAT_R32G32_FLOAT);oldMotion=output(DXGI_FORMAT_R32G32_FLOAT);newMotion=output(DXGI_FORMAT_R32G32_FLOAT);
        oldCarrier=output(half?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT);
        newCarrier=output(half?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT);
        table(0,depth.Get(),motion.Get(),motion.Get(),nrDepth.Get(),nrMotion.Get());
        table(8,proxy.Get(),answer.Get(),motion.Get(),oldCarrier.Get(),oldCarrier.Get());
        table(16,depth.Get(),motion.Get(),motion.Get(),oldDepth.Get(),oldMotion.Get());
        table(24,proxy.Get(),answer.Get(),motion.Get(),newCarrier.Get(),newMotion.Get());
        DlssNrConstants c{};c.Mode=10;c.Width=w;c.Height=h;c.GuideWidth=dr.width;c.GuideHeight=dr.height;
        c.DebugView=dr.x;c.CompareMode=dr.y;c.TransferStrength=float(mr.width);c.ColourStrength=float(mr.height);
        c.CompareSwap=mr.x;c.Transfer=mr.y;c.MvScaleX=c.MvScaleY=1;nrCb=cb(c);
        c.MvScaleX=sx;c.MvScaleY=sy;guideCb=cb(c);c.Mode=39;fusedCb=cb(c);
        c={};c.Mode=37;c.Width=w;c.Height=h;carrierCb=cb(c);
        D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=32;
        check(r.device->CreateQueryHeap(&q,IID_PPV_ARGS(&queries)));
        times=r.buffer(32*sizeof(UINT64),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);check(r.queue->GetTimestampFrequency(&frequency));
    }
    void table(UINT offset,ID3D12Resource* source,ID3D12Resource* model,ID3D12Resource* original,ID3D12Resource* target,ID3D12Resource* keep) {
        auto cpu=r.heap->GetCPUDescriptorHandleForHeapStart();
        for(UINT i=0;i<6;++i) {
            auto* texture=i==1?model:i==2?original:source;
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=texture->GetDesc().Format;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            d.Texture2D.MipLevels=1;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            r.device->CreateShaderResourceView(texture,&d,{cpu.ptr+SIZE_T(offset+i)*r.stride});
        }
        for(UINT i=6;i<8;++i) {
            auto* texture=i==6?target:keep;D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=texture->GetDesc().Format;d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            r.device->CreateUnorderedAccessView(texture,nullptr,&d,{cpu.ptr+SIZE_T(offset+i)*r.stride});
        }
    }
    ComPtr<ID3D12Resource> cb(const DlssNrConstants& c) {
        auto b=r.buffer(sizeof(c),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* p;
        check(b->Map(0,nullptr,&p));std::memcpy(p,&c,sizeof(c));b->Unmap(0,nullptr);return b;
    }
    void roots() { r.list->SetComputeRootSignature(r.root.Get());ID3D12DescriptorHeap* heaps[]={r.heap.Get()};r.list->SetDescriptorHeaps(1,heaps); }
    void dispatch(ID3D12PipelineState* pso,ID3D12Resource* constants,UINT offset) {
        auto gpu=r.heap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=UINT64(offset)*r.stride;r.list->SetPipelineState(pso);
        r.list->SetComputeRootDescriptorTable(0,gpu);r.list->SetComputeRootConstantBufferView(1,constants->GetGPUVirtualAddress());r.list->Dispatch((w+7)/8,(h+7)/8,1);
    }
    void prepare(bool fused,ID3D12PipelineState* standard,ID3D12PipelineState* legacy,ID3D12PipelineState* candidate) {
        dispatch(standard,nrCb.Get(),0);
        r.barrier(nrDepth.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        r.barrier(nrMotion.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if(fused)dispatch(candidate,fusedCb.Get(),24);
        else { dispatch(legacy,carrierCb.Get(),8);dispatch(standard,guideCb.Get(),16); }
        for(auto* texture : {fused?newCarrier.Get():oldCarrier.Get(),fused?newMotion.Get():oldMotion.Get()})
            r.barrier(texture,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if(!fused)r.barrier(oldDepth.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    void rest(bool fused) {
        for(auto* texture : {nrDepth.Get(),nrMotion.Get(),fused?newCarrier.Get():oldCarrier.Get(),fused?newMotion.Get():oldMotion.Get()})
            r.barrier(texture,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if(!fused)r.barrier(oldDepth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    struct Read { ComPtr<ID3D12Resource> buffer;D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot{};UINT bytes; };
    Read read(ID3D12Resource* source) {
        Read out{};auto d=source->GetDesc();UINT64 bytes;r.device->GetCopyableFootprints(&d,0,1,0,&out.foot,nullptr,nullptr,&bytes);
        out.bytes=d.Format==DXGI_FORMAT_R32_FLOAT?4:d.Format==DXGI_FORMAT_R32G32B32A32_FLOAT?16:8;
        out.buffer=r.buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        r.barrier(source,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=source;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=out.buffer.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=out.foot;
        r.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);r.barrier(source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);return out;
    }
    std::vector<uint32_t> words(Read& value) {
        std::vector<uint32_t> out(size_t(w)*h*value.bytes/4);void* data;check(value.buffer->Map(0,nullptr,&data));
        for(UINT y=0;y<h;++y)std::memcpy(out.data()+size_t(y)*w*value.bytes/4,static_cast<char*>(data)+y*value.foot.Footprint.RowPitch,w*value.bytes);
        value.buffer->Unmap(0,nullptr);return out;
    }
    size_t quality(ID3D12PipelineState* standard,ID3D12PipelineState* legacy,ID3D12PipelineState* candidate) {
        r.begin();roots();prepare(false,standard,legacy,candidate);
        std::array<Read,3> a={read(oldCarrier.Get()),read(oldDepth.Get()),read(oldMotion.Get())};rest(false);
        prepare(true,standard,legacy,candidate);
        std::array<Read,3> b={read(newCarrier.Get()),read(nrDepth.Get()),read(newMotion.Get())};rest(true);r.finish();
        size_t total=0;for(UINT field=0;field<3;++field) {
            const auto left=words(a[field]),right=words(b[field]);
            if(left!=right) {
                for(size_t i=0;i<left.size();++i)if(left[i]!=right[i]) {
                    std::cerr<<"Prepare mismatch field="<<field<<" work="<<w<<'x'<<h<<" word="<<i
                        <<" before="<<std::hex<<left[i]<<" after="<<right[i]<<std::dec<<'\n';break;
                }
                throw std::runtime_error("Carrier/depth/motion is not bit-exact");
            }
            total+=left.size();
        }
        return total;
    }
    std::vector<double> batch(bool fused,ID3D12PipelineState* standard,ID3D12PipelineState* legacy,ID3D12PipelineState* candidate,UINT count) {
        r.begin();roots();for(UINT sample=0;sample<count;++sample) {
            r.list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,sample*2);
            for(UINT repeat=0;repeat<8;++repeat) { prepare(fused,standard,legacy,candidate);rest(fused); }
            r.list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,sample*2+1);
        }
        r.list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,count*2,times.Get(),0);r.finish();
        UINT64* stamps;check(times->Map(0,nullptr,reinterpret_cast<void**>(&stamps)));std::vector<double> out;
        for(UINT i=0;i<count;++i)out.push_back(double(stamps[i*2+1]-stamps[i*2])*1000/frequency/8);times->Unmap(0,nullptr);return out;
    }
};
