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
#include "../OptiScaler/native/DepthCalibrationDx12.h"
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
int main(){Gpu g;if(!g.Init())return 2;native::DepthCalibrationDx12 align;
 constexpr unsigned sw=61,sh=47,w=131,h=77;
 std::vector<float> source(sw*sh);for(unsigned y=0;y<sh;++y)for(unsigned x=0;x<sw;++x)source[y*sw+x]=float(y*sw+x)/float(sw*sh);
 std::vector<unsigned char> data(source.size()*4);memcpy(data.data(),source.data(),data.size());auto input=Upload(g,{sw,sh,41,4},data);
 const float transforms[][4]={{1,1,0,0},{1,1.143f,-4,17},{.8f,1.2f,30,-18},{2,.6f,-400,300}};
 unsigned total=0;
 for(unsigned k=0;k<4;++k){auto* out=align.Run(g.device.Get(),g.list.Get(),input.Get(),DXGI_FORMAT_R32_FLOAT,w,h,k%3,transforms[k]);if(!out)return 3;g.Submit();auto bytes=Read(g,out,4,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);auto f=(float*)bytes.data();unsigned mismatches=0;
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){float u=(float(x)+.5f)/w/transforms[k][0]-transforms[k][2]/w;float v=(float(y)+.5f)/h/transforms[k][1]+transforms[k][3]/h;unsigned ix=unsigned(std::clamp(u,0.f,1.f)*sw),iy=unsigned(std::clamp(v,0.f,1.f)*sh);ix=(std::min)(ix,sw-1);iy=(std::min)(iy,sh-1);if(f[y*w+x]!=source[iy*sw+ix])++mismatches;}
  printf("case %u exact point-depth mismatches %u / %u\n",k,mismatches,w*h);total+=mismatches;
 }
 printf("%s native raw-depth calibration (identity, square scale/offset and clamped borders); device=0x%08x\n",total?"FAIL":"PASS",g.device->GetDeviceRemovedReason());return total?1:0;}
