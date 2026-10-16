// Build against the serial SGF and improved implementations, run with an output
// filename, then byte-compare the snapshots. Captures actual GPU vectors,
// confidence, fusion diagnostics and homography, excluding row padding.
#define main UnusedFlowRegressionMain
#include "nr_optical_flow_gpu.cpp"
#undef main
#include <fstream>

static bool Snapshot(Gpu& gpu, std::ofstream& file, ID3D12Resource* resource,
                     D3D12_RESOURCE_STATES resting)
{
    const auto desc = resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 bytes = 0;
    UINT rowCount = 0;
    UINT64 rowBytes = 0;
    gpu.device->GetCopyableFootprints(&desc,0,1,0,&footprint,&rowCount,&rowBytes,&bytes);
    auto readback = gpu.Buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,resting,D3D12_RESOURCE_STATE_COPY_SOURCE};
    gpu.list->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION dst {},src {};
    dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
    src.pResource=resource;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    gpu.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
    gpu.list->ResourceBarrier(1,&barrier);
    gpu.Submit();
    unsigned char* mapped=nullptr;
    if(FAILED(readback->Map(0,nullptr,(void**)&mapped)))return false;
    for(UINT row=0;row<rowCount;++row)
        file.write((const char*)mapped+row*footprint.Footprint.RowPitch,(std::streamsize)rowBytes);
    readback->Unmap(0,nullptr);
    return bool(file);
}

int main(int argc,char** argv)
{
    if(argc!=2)return 2;
    kWidth=641;kHeight=361;
    Gpu gpu;if(!gpu.Init())return 3;
    OpticalFlowDx12 flow;if(!flow.Init(gpu.device.Get()))return 4;
    std::ofstream file(argv[1],std::ios::binary);
    unsigned snapshots=0;
    for(int scene=0;scene<6;++scene)
    {
        flow.Reset();
        auto depth=gpu.DepthMap(150.f,100.f,120.f);
        for(int frame=0;frame<8;++frame)
        {
            const bool grain=scene==4,mixed=scene==5;
            auto picture=gpu.Picture(frame*1.75f,frame*-.8f,grain?.3f:1.f,grain?6.f:0.f,frame,
                mixed?150.f-frame*1.2f:0.f,mixed?100.f+frame*.6f:0.f,mixed?120.f:0.f,
                scene<4?scene:0,mixed?2:0,.8f,.1f);
            if(!flow.Dispatch(gpu.list.Get(),picture.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,
                mixed?depth.Get():nullptr,mixed?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_UNKNOWN,true))return 5;
            gpu.Submit();
            if(!flow.FlowValid())continue;
            if(!Snapshot(gpu,file,flow.Flow(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)||
               !Snapshot(gpu,file,flow.FusionInfo(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)||
               !Snapshot(gpu,file,flow.GeometryModel(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))return 6;
            ++snapshots;
        }
    }
    printf("Captured %u complete vector/confidence/source/model snapshots at 641x361\n",snapshots);
    return FAILED(gpu.device->GetDeviceRemovedReason())?7:0;
}
