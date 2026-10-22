#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vector>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <iterator>
#include <cstring>
#include <d3dcompiler.h>
#include "../OptiScaler/motion/OpticalFlow_Dx12.h"
using Microsoft::WRL::ComPtr;
struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 fenceValue = 0;

    bool Init()
    {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return false;

        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
            {
                wprintf(L"adapter: %s\n", desc.Description);
                break;
            }
        }

        if (!device)
            return false;

        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return false;

        event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        return true;
    }

    void Submit()
    {
        list->Close();
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), ++fenceValue);
        fence->SetEventOnCompletion(fenceValue, event);
        WaitForSingleObject(event, INFINITE);
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
    }

    ComPtr<ID3D12Resource> Buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r));
        return r;
    }

};
struct Entry{unsigned w,h,format,stride;};
static std::vector<unsigned char> FileBytes(const std::string& path){std::ifstream f(path,std::ios::binary);return {(std::istreambuf_iterator<char>(f)),{}};}
static void Barrier(Gpu& g,ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};g.list->ResourceBarrier(1,&b);}
static ComPtr<ID3D12Resource> Upload(Gpu& g,const Entry& e,const std::vector<unsigned char>& data){
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};
    d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=e.w;d.Height=e.h;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Format=DXGI_FORMAT(e.format);
    if(d.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    if(d.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    ComPtr<ID3D12Resource> tex;if(FAILED(g.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&tex))))return {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 size=0;g.device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&size);
    auto upload=g.Buffer(size,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);unsigned char* p=nullptr;
    upload->Map(0,nullptr,(void**)&p);for(unsigned y=0;y<e.h;++y)memcpy(p+y*fp.Footprint.RowPitch,data.data()+size_t(y)*e.w*e.stride,size_t(e.w)*e.stride);upload->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
    dst.pResource=tex.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;g.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    Barrier(g,tex.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);g.Submit();return tex;
}
static std::vector<unsigned char> Read(Gpu& g,ID3D12Resource* r,unsigned stride,D3D12_RESOURCE_STATES state){
    auto d=r->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 size=0;g.device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&size);
    auto read=g.Buffer(size,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    Barrier(g,r,state,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION src{},dst{};
    src.pResource=r;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.pResource=read.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
    g.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Barrier(g,r,D3D12_RESOURCE_STATE_COPY_SOURCE,state);g.Submit();
    unsigned char* p=nullptr;D3D12_RANGE range{0,size_t(size)};read->Map(0,&range,(void**)&p);
    std::vector<unsigned char> result(size_t(d.Width)*d.Height*stride);
    for(unsigned y=0;y<d.Height;++y)memcpy(result.data()+size_t(y)*d.Width*stride,p+y*fp.Footprint.RowPitch,size_t(d.Width)*stride);
    read->Unmap(0,nullptr);return result;
}

static const char* referenceSource=R"REF(Texture2D<float> Source:register(t0);
RWTexture2D<float4> Target:register(u0);
SamplerState Linear:register(s0);
cbuffer RefSettings:register(b0){float2 viewport;float2 scale;float2 offset;float multiplier;float farPlane;uint reversed;uint linearize;float2 padding;};
#define BUFFER_PIXEL_SIZE (1.0/viewport)
#define BUFFER_SCREEN_SIZE viewport
float GetLinearizedDepth(float2 texcoord){
 texcoord/=scale;texcoord+=offset;
 float depth=Source.SampleLevel(Linear,texcoord,0).x*multiplier;
 if(reversed!=0)depth=1.0-depth;
 const float N=1.0;
 depth/=farPlane-depth*(farPlane-N);
 return depth;
}
float3 GetScreenSpaceNormal(float2 texcoord)
{
    float3 offset = float3(BUFFER_PIXEL_SIZE, 0.0);
    float2 posCenter = texcoord.xy;
    float2 posNorth  = posCenter - offset.zy;
    float2 posEast   = posCenter + offset.xz;

    float3 vertCenter = float3(posCenter - 0.5, 1) * GetLinearizedDepth(posCenter);
    float3 vertNorth  = float3(posNorth - 0.5,  1) * GetLinearizedDepth(posNorth);
    float3 vertEast   = float3(posEast - 0.5,   1) * GetLinearizedDepth(posEast);

    return normalize(cross(vertCenter - vertNorth, vertCenter - vertEast)) * 0.5 + 0.5;
}
[numthreads(8,8,1)] void Reference(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=uint2(viewport)))return;
 float2 texcoord=(float2(id.xy)+0.5)/viewport;
    float3 depth = GetLinearizedDepth(texcoord).xxx;
    float3 normal = GetScreenSpaceNormal(texcoord);

    // Ordered dithering
#if 1
    const float dither_bit = 8.0; // Number of bits per channel. Should be 8 for most monitors.
    // Calculate grid position
    float grid_position = frac(dot(texcoord, (BUFFER_SCREEN_SIZE * float2(1.0 / 16.0, 10.0 / 36.0)) + 0.25));
    // Calculate how big the shift should be
    float dither_shift = 0.25 * (1.0 / (pow(2, dither_bit) - 1.0));
    // Shift the individual colors differently, thus making it even harder to see the dithering pattern
    float3 dither_shift_RGB = float3(dither_shift, -dither_shift, dither_shift); // Subpixel dithering
    // Modify shift acording to grid position.
    dither_shift_RGB = lerp(2.0 * dither_shift_RGB, -2.0 * dither_shift_RGB, grid_position);
    depth += dither_shift_RGB;
#endif


 if(linearize==0u){
  float raw=Source.SampleLevel(Linear,texcoord/scale+offset,0).x*multiplier;
  float grid_position=frac(dot(texcoord,(BUFFER_SCREEN_SIZE*float2(1.0/16.0,10.0/36.0))+0.25));
  float dither_shift=0.25/255.0;
  float3 rgb=float3(dither_shift,-dither_shift,dither_shift);
  depth=raw.xxx+lerp(2.0*rgb,-2.0*rgb,grid_position);
 }
 Target[id.xy]=float4(texcoord.x<0.5?normal:depth,1.0);
}
)REF";

static bool Reference(Gpu& g,ID3D12Resource* source,ID3D12Resource* target,unsigned w,unsigned h,const float* transform,bool reversed,bool linearize){
 ComPtr<ID3DBlob> code,error,rs;
 if(FAILED(D3DCompile(referenceSource,strlen(referenceSource),"DisplayDepth-original-reference",nullptr,nullptr,"Reference","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error))){if(error)printf("%s\n",(char*)error->GetBufferPointer());return false;}
 D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1}};
 D3D12_ROOT_PARAMETER params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={2,ranges};params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,12};
 D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.MaxAnisotropy=1;sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
 D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=params;rd.NumStaticSamplers=1;rd.pStaticSamplers=&sampler;
 ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pso;ComPtr<ID3D12DescriptorHeap> heap;
 if(FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&rs,&error))||FAILED(g.device->CreateRootSignature(0,rs->GetBufferPointer(),rs->GetBufferSize(),IID_PPV_ARGS(&root))))return false;
 D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={code->GetBufferPointer(),code->GetBufferSize()};if(FAILED(g.device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso))))return false;
 D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=2;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;if(FAILED(g.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))))return false;
 auto cpu=heap->GetCPUDescriptorHandleForHeapStart();D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R32_FLOAT;sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sv.Texture2D.MipLevels=1;g.device->CreateShaderResourceView(source,&sv,cpu);
 cpu.ptr+=g.device->GetDescriptorHandleIncrementSize(hd.Type);D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=DXGI_FORMAT_R8G8B8A8_UNORM;ud.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;g.device->CreateUnorderedAccessView(target,nullptr,&ud,cpu);
 struct Settings{float viewport[2],scale[2],offset[2],multiplier,farPlane;unsigned reversed,linearize;float padding[2];}s{};
 s.viewport[0]=float(w);s.viewport[1]=float(h);s.scale[0]=transform[0];s.scale[1]=transform[1];s.offset[0]=-transform[2]/float(w);s.offset[1]=transform[3]/float(h);s.multiplier=reversed?4500.f:1.f;s.farPlane=1000.f;s.reversed=reversed;s.linearize=linearize;
 g.list->SetComputeRootSignature(root.Get());ID3D12DescriptorHeap* heaps[]={heap.Get()};g.list->SetDescriptorHeaps(1,heaps);g.list->SetPipelineState(pso.Get());g.list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());g.list->SetComputeRoot32BitConstants(1,12,&s,0);g.list->Dispatch((w+7)/8,(h+7)/8,1);g.Submit();return true;
}
int main(int argc,char** argv){Gpu g;if(!g.Init())return 2;const unsigned w=322,h=182,sw=201,sh=171;
 unsigned totalMismatch=0,totalChecked=0;
 for(unsigned test=0;test<4;++test){OpticalFlowDx12 flow;if(!flow.Init(g.device.Get()))return 3;bool reversed=test<3,linearize=test!=2;float transform[4]={1.f,test?1.143f:1.f,test?-4.f:0.f,test?17.f:0.f};
 std::vector<unsigned char> color(w*h*4);for(unsigned i=0;i<w*h;++i){color[i*4]=i%251;color[i*4+1]=i/17%255;color[i*4+2]=i/41%255;color[i*4+3]=255;}
 std::vector<float> depth(sw*sh);for(unsigned y=0;y<sh;++y)for(unsigned x=0;x<sw;++x){float raw=1e-6f+float(x)*5e-9f+float(y)*2e-9f;if(x>sw/2 && y<sh/3)raw=0.f;depth[y*sw+x]=reversed?raw:1.f-raw*4500.f;}
 std::vector<unsigned char> db(depth.size()*4);memcpy(db.data(),depth.data(),db.size());auto c=Upload(g,{w,h,28,4},color),d=Upload(g,{sw,sh,41,4},db);
 for(unsigned frame=0;frame<3;++frame){if(!flow.Dispatch(g.list.Get(),c.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,d.Get(),DXGI_FORMAT_R32_FLOAT,reversed))return 4;g.Submit();}
 auto before=Read(g,flow.Flow(),8,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
 flow.DebugTuning().depthLinearize=linearize;flow.DebugTuning().depthMultiplier=reversed?4500.f:1.f;flow.DebugTuning().depthFarPlane=1000.f;
 if(!flow.Visualise(g.list.Get(),24,6,c.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,d.Get(),DXGI_FORMAT_R32_FLOAT,reversed,nullptr,nullptr,transform))return 5;g.Submit();
 auto desc=flow.Preview()->GetDesc();if(desc.Width!=w||desc.Height!=h)return 6;
 auto actual=Read(g,flow.Preview(),4,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
 D3D12_RESOURCE_DESC td=desc;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> reference;
 if(FAILED(g.device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&reference))))return 7;
 if(!Reference(g,d.Get(),reference.Get(),w,h,transform,reversed,linearize))return 8;auto expected=Read(g,reference.Get(),4,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 unsigned mismatch=0,checked=0,maxError=0;for(unsigned y=40;y<h;++y)for(unsigned x=0;x<w;++x)for(unsigned k=0;k<4;++k){size_t i=(y*w+x)*4+k;unsigned error=unsigned(abs(int(actual[i])-int(expected[i])));maxError=(std::max)(maxError,error);++checked;mismatch+=error>1;}
 printf("case %u reversed=%u linearize=%u calibrated=%u: %u/%u channel mismatches >1/255; max %u/255\n",test,reversed,linearize,test!=0,mismatch,checked,maxError);totalMismatch+=mismatch;totalChecked+=checked;
 if(Read(g,flow.Flow(),8,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)!=before)return 9;
 if(Read(g,d.Get(),4,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)!=db)return 10;
 if(argc>1){std::ofstream f(std::string(argv[1])+"/reference-case-"+std::to_string(test)+".rgba",std::ios::binary);f.write((char*)actual.data(),actual.size());}
 // Switching views uses separate retained targets, not reallocation in flight.
 if(!flow.Visualise(g.list.Get(),24,0))return 11;g.Submit();if(flow.Preview()->GetDesc().Width!=(w+1)/2)return 12;
 if(!flow.Visualise(g.list.Get(),24,6,c.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,nullptr,DXGI_FORMAT_UNKNOWN,true))return 13;g.Submit();auto missing=Read(g,flow.Preview(),4,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);size_t at=(50*w+10)*4;if(missing[at+1]!=0||missing[at]!=missing[at+2])return 14;
 }
 printf("TOTAL: %u/%u mismatches; raw model depth and motion fields unchanged; full/half debug switching and missing-depth checker passed\n",totalMismatch,totalChecked);return totalMismatch?15:(FAILED(g.device->GetDeviceRemovedReason())?16:0);
}
