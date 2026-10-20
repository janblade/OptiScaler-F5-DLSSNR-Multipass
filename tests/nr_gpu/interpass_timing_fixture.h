#pragma once
#include "interpass_fixture.h"
#include "BenchmarkCases.h"
#include <cmath>
#include <iomanip>

// Isolated inter-pass GPU timing, not the complete in-game NR/NGX chain.
// Keep resources resident across all windows; uploads, allocations and fences are outside timestamps.
struct LowFixture
{
    Runner& r;
    UINT nw=3840,nh=2160,ww,wh;
    std::array<ComPtr<ID3D12Resource>,3> inputs;
    ComPtr<ID3D12Resource> native,work,classicCb,fusedCb,rgbCb,compactCb,fastCb,downCb,times;
    ComPtr<ID3D12Resource> low,lowCb,low36Cb,crossCb;
    bool shaping;
    ComPtr<ID3D12QueryHeap> queries;
    UINT64 frequency;
    LowFixture(Runner& runner,int scale,bool shape=false):r(runner),
        ww(UINT(std::lround(nw*scale/100.0f))),wh(UINT(std::lround(nh*scale/100.0f))),shaping(shape)
    {
        r.begin();
        std::vector<ComPtr<ID3D12Resource>> uploads;
        for(int n=0;n<3;++n)
        {
            const UINT w=n==2?nw:ww,h=n==2?nh:wh;
            D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width=w;d.Height=h;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;
            d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
            inputs[n]=r.resource(d,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes;
            r.device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
            auto upload=r.buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            char* data;check(upload->Map(0,nullptr,reinterpret_cast<void**>(&data)));
            for(UINT y=0;y<h;++y) for(UINT x=0;x<w;++x)
            {
                const float u=(x+0.5f)/w,v=(y+0.5f)/h;
                float pixel[4]={0.4f+0.5f*std::sin(7*u)+0.1f*std::cos(15*v),
                    0.4f+0.5f*std::cos(8*v)+0.1f*std::sin(17*u),
                    0.4f+0.5f*std::sin(5*(u+v)),0.7f};
                if(n==1) for(int c=0;c<3;++c) pixel[c]+=0.03f*std::sin(37*u+29*v+c);
                std::memcpy(data+y*fp.Footprint.RowPitch+x*16,pixel,16);
            }
            upload->Unmap(0,nullptr);uploads.push_back(upload);
            D3D12_TEXTURE_COPY_LOCATION src{},dst{};
            src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
            dst.pResource=inputs[n].Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            r.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            r.barrier(inputs[n].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        r.finish();
        const auto texture=[&](UINT w,UINT h)
        {
            D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width=w;d.Height=h;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;
            d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            return r.resource(d,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        };
        native=texture(nw,nh);work=texture(ww,wh);
        if(shaping) low=texture((ww+7)/8,(wh+7)/8);
        const auto table=[&](UINT offset,ID3D12Resource* source,ID3D12Resource* model,ID3D12Resource* out,bool half)
        {
            const auto cpu=r.heap->GetCPUDescriptorHandleForHeapStart();
            for(UINT i=0;i<6;++i)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
                v.Format=(half&&i!=2)?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT;
                const bool lowInput=shaping && (offset==0 || offset==8) && i==5;
                if(lowInput) v.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
                v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.Texture2D.MipLevels=1;
                r.device->CreateShaderResourceView(lowInput ? low.Get() :
                    i==2?inputs[2].Get():i==1?model:source,&v,{cpu.ptr+SIZE_T(offset+i)*r.stride});
            }
            for(UINT i=6;i<8;++i)
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
                u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
                r.device->CreateUnorderedAccessView(out,nullptr,&u,{cpu.ptr+SIZE_T(offset+i)*r.stride});
            }
        };
        table(0,inputs[0].Get(),inputs[1].Get(),work.Get(),false);
        table(8,inputs[0].Get(),inputs[1].Get(),native.Get(),false);
        table(16,native.Get(),native.Get(),work.Get(),true);
        if(shaping) table(24,inputs[0].Get(),inputs[1].Get(),low.Get(),false);
        const auto cb=[&](const DlssNrConstants& c)
        {
            auto result=r.buffer(sizeof(c),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            void* p;check(result->Map(0,nullptr,&p));std::memcpy(p,&c,sizeof(c));result->Unmap(0,nullptr);
            return result;
        };
        DlssNrConstants c{};c.Mode=28;c.Width=ww;c.Height=wh;
        c.ResidualHistoryValid=1;c.ResidualScale=1.2f;c.ResidualBlend=0.015f;c.ResidualConfidenceSensitivity=1;
        if(shaping){c.GuideWidth=1;c.MvScaleX=1.13f;c.MvScaleY=0.71f;c.ResidualScale=3.0f;}
        c.DirectResolveFlags=16|64|128;
        const float rx=float(ww)/nw,ry=float(wh)/nh;
        std::memcpy(&c.ResidualMotionBaseX,&rx,4);std::memcpy(&c.ResidualMotionBaseY,&ry,4);
        fusedCb=cb(c);
        c.Mode=33;c.DirectResolveFlags=0;fastCb=cb(c);
        c.Mode=36;crossCb=cb(c);
        c.Mode=28;c.DirectResolveFlags=16|64|128|2048;rgbCb=cb(c);
        c.DirectResolveFlags|=8192;compactCb=cb(c);
        c.Mode=27;c.Width=nw;c.Height=nh;c.DirectResolveFlags=16|64|1024;
        c.DirectDetailMode=ww;c.DirectResolveUpscaler=wh;classicCb=cb(c);
        c={};c.Mode=32;c.Width=ww;c.Height=wh;c.DirectResolveFlags=512;downCb=cb(c);
        if(shaping)
        {
            c={};c.Mode=18;c.Width=(ww+7)/8;c.Height=(wh+7)/8;lowCb=cb(c);
            c.Mode=34;low36Cb=cb(c);
        }
        D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=32;
        check(r.device->CreateQueryHeap(&q,IID_PPV_ARGS(&queries)));
        times=r.buffer(32*sizeof(UINT64),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        check(r.queue->GetTimestampFrequency(&frequency));
    }
    std::vector<double> batch(DlssNr::InterPass::Path path,ID3D12PipelineState* standard,ID3D12PipelineState* rgb,UINT count,ID3D12PipelineState* fast=nullptr,ID3D12PipelineState* experiment=nullptr,ID3D12PipelineState* lowPipeline=nullptr)
    {
        r.begin();r.list->SetComputeRootSignature(r.root.Get());
        ID3D12DescriptorHeap* heaps[]={r.heap.Get()};r.list->SetDescriptorHeaps(1,heaps);
        const auto bind=[&](ID3D12PipelineState* pso,ID3D12Resource* cb,UINT table)
        {
            r.list->SetPipelineState(pso);
            auto gpu=r.heap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=UINT64(table)*r.stride;
            r.list->SetComputeRootDescriptorTable(0,gpu);
            r.list->SetComputeRootConstantBufferView(1,cb->GetGPUVirtualAddress());
        };
        for(UINT sample=0;sample<count;++sample)
        {
            r.list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,sample*2);
            for(UINT repeat=0;repeat<8;++repeat)
            {
                if(shaping)
                {
                    const bool low36=DlssNr::InterPass::Fast(path);
                    bind(low36 && lowPipeline?lowPipeline:standard,low36?low36Cb.Get():lowCb.Get(),24);
                    r.list->Dispatch(((ww+7)/8+7)/8,((wh+7)/8+7)/8,1);
                    r.barrier(low.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                }
                if(path==DlssNr::InterPass::Path::ClassicOptimized)
                {
                    bind(standard,classicCb.Get(),8);r.list->Dispatch((nw+7)/8,(nh+7)/8,1);
                    r.barrier(native.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    bind(standard,downCb.Get(),16);r.list->Dispatch((ww+7)/8,(wh+7)/8,1);
                    r.barrier(native.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }
                else
                {
                    const bool tiled=path==DlssNr::InterPass::Path::Rgb20 || path==DlssNr::InterPass::Path::Rgb16;
                    const bool approximate=DlssNr::InterPass::Fast(path);
                    const bool cross=path==DlssNr::InterPass::Path::FastCombinedCross5;
                    if(approximate && !fast) throw std::runtime_error("Missing Fast PSO");
                    bind(cross?experiment:approximate?fast:tiled?rgb:standard,
                         cross?crossCb.Get():approximate?fastCb.Get():path==DlssNr::InterPass::Path::Rgb16?compactCb.Get():
                         tiled?rgbCb.Get():fusedCb.Get(),0);
                    r.list->Dispatch((ww+7)/8,(wh+7)/8,1);
                }
                D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;b.UAV.pResource=work.Get();
                r.list->ResourceBarrier(1,&b);
                if(shaping) r.barrier(low.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            r.list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,sample*2+1);
        }
        r.list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,count*2,times.Get(),0);
        r.finish();
        UINT64* stamps;check(times->Map(0,nullptr,reinterpret_cast<void**>(&stamps)));
        std::vector<double> result;
        for(UINT i=0;i<count;++i)result.push_back(double(stamps[i*2+1]-stamps[i*2])*1000/frequency/8);
        times->Unmap(0,nullptr);return result;
    }
};
