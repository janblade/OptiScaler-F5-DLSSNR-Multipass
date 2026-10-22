#pragma once
#include "interpass_timing_fixture.h"

struct ResolveFixture {
    Runner& r;LowFixture images;
    ComPtr<ID3D12Resource> edit,encodeCb,decodeCb,lowCb,resolveCb;
    ResolveFixture(Runner& runner,int scale,ID3D12PipelineState* carrier,int hdr=-1):
        r(runner),images(runner,scale,true) {
        auto d=images.work->GetDesc();edit=r.resource(d,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        table(32,images.inputs[0].Get(),images.inputs[1].Get(),images.work.Get());
        table(40,images.work.Get(),images.work.Get(),edit.Get());
        table(48,images.inputs[0].Get(),edit.Get(),images.low.Get());
        table(56,images.inputs[0].Get(),edit.Get(),images.native.Get());
        DlssNrConstants c{};c.Mode=37;c.Width=images.ww;c.Height=images.wh;encodeCb=cb(c);
        c.Mode=38;decodeCb=cb(c);
        c={};c.Mode=18;c.Transfer=1;c.Width=(images.ww+7)/8;c.Height=(images.wh+7)/8;lowCb=cb(c);
        c={};c.Mode=1;c.Width=images.nw;c.Height=images.nh;c.Transfer=9;c.Passthrough=1;c.ApplyModel=1;
        if(hdr>=0) { c.Passthrough=0;c.ReversibleMode=UINT(hdr); }
        c.WhitePoint=1;c.TransferStrength=1;c.ColourStrength=1;c.MaxRatio=2;c.MaxDarkening=100;
        c.ResidualHistoryValid=1;c.ResidualScale=3;c.ResidualBlend=.015f;c.ResidualConfidenceSensitivity=1;
        c.GuideWidth=1;c.MvScaleX=.8f;c.MvScaleY=.661f;c.DirectResolveFlags=65536;
        c.SkinDetail=c.SkinColour=c.EnvironmentDetail=c.EnvironmentColour=1;resolveCb=cb(c);
        r.begin();bindRoots();
        dispatch(carrier,encodeCb.Get(),32,images.ww,images.wh);
        r.barrier(images.work.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        dispatch(carrier,decodeCb.Get(),40,images.ww,images.wh);
        r.barrier(edit.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        r.barrier(images.work.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        r.finish();
    }
    void table(UINT offset,ID3D12Resource* source,ID3D12Resource* model,ID3D12Resource* target) {
        auto cpu=r.heap->GetCPUDescriptorHandleForHeapStart();
        for(UINT i=0;i<6;++i) {
            auto* resource=i==1?model:i==2?images.inputs[2].Get():i==5?images.low.Get():source;
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=resource->GetDesc().Format;
            d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;d.Texture2D.MipLevels=1;
            d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            r.device->CreateShaderResourceView(resource,&d,{cpu.ptr+SIZE_T(offset+i)*r.stride});
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=target->GetDesc().Format;d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        for(UINT i=6;i<8;++i)r.device->CreateUnorderedAccessView(target,nullptr,&d,{cpu.ptr+SIZE_T(offset+i)*r.stride});
    }
    ComPtr<ID3D12Resource> cb(const DlssNrConstants& c) {
        auto b=r.buffer(sizeof(c),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* p;
        check(b->Map(0,nullptr,&p));std::memcpy(p,&c,sizeof(c));b->Unmap(0,nullptr);return b;
    }
    void bindRoots() {
        r.list->SetComputeRootSignature(r.root.Get());ID3D12DescriptorHeap* heaps[]={r.heap.Get()};r.list->SetDescriptorHeaps(1,heaps);
    }
    void dispatch(ID3D12PipelineState* pso,ID3D12Resource* constants,UINT offset,UINT w,UINT h,UINT groupY=8) {
        auto gpu=r.heap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=UINT64(offset)*r.stride;
        r.list->SetPipelineState(pso);r.list->SetComputeRootDescriptorTable(0,gpu);
        r.list->SetComputeRootConstantBufferView(1,constants->GetGPUVirtualAddress());r.list->Dispatch((w+7)/8,(h+groupY-1)/groupY,1);
    }
    std::vector<double> batch(ID3D12PipelineState* resolve,ID3D12PipelineState* low,UINT count,bool lowOnly=false,UINT lowGroupY=8) {
        r.begin();bindRoots();
        for(UINT sample=0;sample<count;++sample) {
            r.list->EndQuery(images.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,sample*2);
            for(UINT repeat=0;repeat<4;++repeat) {
                dispatch(low,lowCb.Get(),48,(images.ww+7)/8,(images.wh+7)/8,lowGroupY);
                r.barrier(images.low.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if(!lowOnly)dispatch(resolve,resolveCb.Get(),56,images.nw,images.nh);
                r.barrier(images.low.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if(!lowOnly) { D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;b.UAV.pResource=images.native.Get();r.list->ResourceBarrier(1,&b); }
            }
            r.list->EndQuery(images.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,sample*2+1);
        }
        r.list->ResolveQueryData(images.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,count*2,images.times.Get(),0);r.finish();
        UINT64* stamps;check(images.times->Map(0,nullptr,reinterpret_cast<void**>(&stamps)));std::vector<double> result;
        for(UINT i=0;i<count;++i)result.push_back(double(stamps[i*2+1]-stamps[i*2])*1000/images.frequency/4);
        images.times->Unmap(0,nullptr);return result;
    }
    // Record A/B/B/A (or B/A/A/B) together, reducing idle-clock and CPU-submission drift.
    std::vector<std::array<double,4>> pairedBatch(ID3D12PipelineState* resolve,ID3D12PipelineState* before,
        ID3D12PipelineState* after,bool lowOnly,UINT candidateGroupY,bool reverse,UINT count) {
        if(count>4)throw std::runtime_error("Paired timestamp capacity exceeded");
        r.begin();bindRoots();
        for(UINT sample=0;sample<count;++sample)for(UINT position=0;position<4;++position) {
            const bool candidate=(position==1 || position==2)!=reverse;const UINT query=(sample*4+position)*2;
            r.list->EndQuery(images.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,query);
            for(UINT repeat=0;repeat<4;++repeat) {
                dispatch(candidate?after:before,lowCb.Get(),48,(images.ww+7)/8,(images.wh+7)/8,candidate?candidateGroupY:8u);
                r.barrier(images.low.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if(!lowOnly)dispatch(resolve,resolveCb.Get(),56,images.nw,images.nh);
                r.barrier(images.low.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                if(!lowOnly) { D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;b.UAV.pResource=images.native.Get();r.list->ResourceBarrier(1,&b); }
            }
            r.list->EndQuery(images.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,query+1);
        }
        r.list->ResolveQueryData(images.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,count*8,images.times.Get(),0);r.finish();
        UINT64* stamps;check(images.times->Map(0,nullptr,reinterpret_cast<void**>(&stamps)));
        std::vector<std::array<double,4>> result(count);
        for(UINT sample=0;sample<count;++sample)for(UINT position=0;position<4;++position) {
            const UINT query=(sample*4+position)*2;
            result[sample][position]=double(stamps[query+1]-stamps[query])*1000/images.frequency/4;
        }images.times->Unmap(0,nullptr);return result;
    }
};
