// Direct production-fitter regression. Inject known correspondence/confidence fields;
// this deliberately bypasses estimation to isolate sampling/refit failures.
#define main OriginalOpticalFlowRegressionMain
#include "nr_optical_flow_gpu.cpp"
#undef main
#include "../OptiScaler/motion/OpticalFlow_Hlsl.h"
#include <d3dcompiler.h>
#include <array>
#include <cstring>

namespace {
static constexpr float refitSamples[16][3] = {
    { -7.465384359f, 3.130839934f, 0.483088748f },
    { -4.433384975f, 2.561719294f, 0.442562138f },
    { -6.638673722f, 4.682375500f, 0.506693396f },
    { -4.368727656f, 3.382605479f, 0.773208579f },
    { -5.904151248f, 3.779830257f, 0.248571400f },
    { -7.237571918f, 3.854772129f, 0.230327132f },
    { -6.923692274f, 1.975862921f, 0.241712811f },
    { -8.430739430f, 3.532771579f, 0.050000000f },
    { -5.617833080f, 3.388436723f, 0.341644643f },
    { -6.597108228f, 2.094897498f, 0.210308807f },
    { -5.800966518f, 2.979403426f, 0.727384618f },
    { -7.488434117f, 2.376923854f, 0.074366539f },
    { -6.532193504f, 2.798282328f, 0.440776775f },
    { -5.287885057f, 3.258064723f, 0.223756949f },
    { -4.639161359f, 2.956689154f, 0.051595309f },
    { -6.112772364f, 4.014011142f, 0.128242448f },
};

void Barrier(Gpu& gpu,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{};x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};gpu.list->ResourceBarrier(1,&x);
}
ComPtr<ID3D12Resource> FloatTexture(Gpu& gpu,unsigned w,unsigned h,unsigned channels,const std::vector<float>& data,bool output=false) {
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
    d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;
    d.Format=channels==1?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT;
    d.Flags=output?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> tex;
    if(FAILED(gpu.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,
        output?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&tex))))return {};
    if(output)return tex;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 total=0;
    gpu.device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&total);
    auto upload=gpu.Buffer(total,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    unsigned char* mapped=nullptr;upload->Map(0,nullptr,(void**)&mapped);
    for(unsigned y=0;y<h;++y)memcpy(mapped+y*fp.Footprint.RowPitch,data.data()+size_t(y)*w*channels,size_t(w)*channels*sizeof(float));
    upload->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
    dst.pResource=tex.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    gpu.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    Barrier(gpu,tex.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);gpu.Submit();return tex;
}
std::array<float,12> ReadModel(Gpu& gpu,ID3D12Resource* tex) {
    auto d=tex->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes=0;
    gpu.device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
    auto read=gpu.Buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    Barrier(gpu,tex,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=tex;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=read.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
    gpu.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    Barrier(gpu,tex,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);gpu.Submit();
    std::array<float,12> result{};void* p=nullptr;D3D12_RANGE range{0,size_t(bytes)};
    read->Map(0,&range,&p);memcpy(result.data(),p,sizeof(result));read->Unmap(0,nullptr);return result;
}
}

int main(int argc,char** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);const bool legacy=argc>1 && strcmp(argv[1],"legacy")==0;
    Gpu gpu;if(!gpu.Init())return 2;
    ComPtr<ID3DBlob> code,error;
    if(FAILED(D3DCompile(OpticalFlowHlsl::kSource,strlen(OpticalFlowHlsl::kSource),nullptr,nullptr,nullptr,"FitGeometry","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error))){
        if(error)printf("%s\n",(const char*)error->GetBufferPointer());return 2;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,8,0,0,0};ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,0};
    D3D12_ROOT_PARAMETER params[3]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[0].Constants={0,0,28};
    for(int i=1;i<3;++i){params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i].DescriptorTable={1,&ranges[i-1]};}
    D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=3;rd.pParameters=params;rd.NumStaticSamplers=1;rd.pStaticSamplers=&sampler;
    ComPtr<ID3DBlob> serialized;ComPtr<ID3D12RootSignature> root;
    if(FAILED(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error)) ||
       FAILED(gpu.device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root))))return 2;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={code->GetBufferPointer(),code->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso;if(FAILED(gpu.device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso))))return 2;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=10;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;if(FAILED(gpu.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))))return 2;
    unsigned stride=gpu.device->GetDescriptorHandleIncrementSize(hd.Type);
    auto cpu=[&](unsigned i){auto h=heap->GetCPUDescriptorHandleForHeapStart();h.ptr+=size_t(i)*stride;return h;};
    auto gpuHandle=[&](unsigned i){auto h=heap->GetGPUDescriptorHandleForHeapStart();h.ptr+=size_t(i)*stride;return h;};
    auto luma=FloatTexture(gpu,320,180,1,std::vector<float>(320*180,.5f));
    auto model=FloatTexture(gpu,3,1,4,{},true);
    if(!luma || !model)return 2;
    struct Case{const char* name;unsigned mask;float dx,dy;bool expected;bool refit=false;};
    Case cases[]={{"full-support translation",0xffff,-6,3,true},
                  {"eight supported sites missed by old sampling",0x053f,-6,3,!legacy},
                  {"fewer than six supported sites",0x0507,-6,3,false},
                  {"outside previous image",0xffff,1000,0,false},
                  {"no supported sites",0,-6,3,false},
                  {"refit loses sixth supported site",0xffff,-6,3,!legacy,true}};
    bool ok=true;
    for(const auto& c:cases){
        std::vector<float> field(320*180*4);
        for(unsigned y=0;y<180;++y)for(unsigned x=0;x<320;++x){
            unsigned site=(y*4/180)*4+x*4/320;size_t i=(size_t(y)*320+x)*4;
            field[i]=c.dx;field[i+1]=c.dy;field[i+2]=(c.mask&(1u<<site))?1.f:0.f;field[i+3]=1;
            if(c.refit){field[i]=refitSamples[site][0];field[i+1]=refitSamples[site][1];field[i+2]=refitSamples[site][2];}
        }
        auto flow=FloatTexture(gpu,320,180,4,field);if(!flow)return 2;
        for(unsigned t=0;t<8;++t){
            auto* resource=(t==1 || t==4)?luma.Get():flow.Get();
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=resource->GetDesc().Format;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;
            gpu.device->CreateShaderResourceView(resource,&d,cpu(t));
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};ud.Format=model->GetDesc().Format;ud.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        gpu.device->CreateUnorderedAccessView(model.Get(),nullptr,&ud,cpu(8));gpu.device->CreateUnorderedAccessView(model.Get(),nullptr,&ud,cpu(9));
        unsigned constants[28]{};constants[0]=3;constants[1]=1;constants[2]=640;constants[3]=360;
        ID3D12DescriptorHeap* heaps[]={heap.Get()};gpu.list->SetDescriptorHeaps(1,heaps);gpu.list->SetComputeRootSignature(root.Get());
        gpu.list->SetComputeRoot32BitConstants(0,28,constants,0);gpu.list->SetComputeRootDescriptorTable(1,gpuHandle(0));
        gpu.list->SetComputeRootDescriptorTable(2,gpuHandle(8));gpu.list->SetPipelineState(pso.Get());gpu.list->Dispatch(1,1,1);
        auto values=ReadModel(gpu,model.Get());bool valid=values[10]>.5f;bool passed=valid==c.expected;
        if(valid && !c.refit){float vx=values[2]*.5f*640,vy=values[5]*.5f*360;passed&=std::hypot(vx-c.dx,vy-c.dy)<.1f;}
        printf("%s: valid=%d support=%.4f error=%.5f count=%.0f expected=%d %s\n",c.name,valid,values[8],values[9],values[11],c.expected,passed?"PASS":"FAIL");ok&=passed;
    }
    printf("production geometric fitting GPU: %s\n",ok?"PASS":"FAIL");return ok?0:1;
}
