// Real diagnostic transport readbacks, D3D12 -> D3D11 SRVs and Vulkan sampled images.
// Reuses the existing standalone adapter/fence test setup, never launches a game.
#define main ExistingSharedTransportMain
#include "nr_shared_frame_vk_gpu.cpp"
#undef main
#include "../OptiScaler/native/DebugPreviewDx11.h"
#include "../OptiScaler/native/DebugPreviewVk.h"

namespace
{
struct Source
{
    ComPtr<ID3D12Resource> texture, upload;
    D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    uint32_t width=0,height=0,bytes=0,seed=0;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    bool Create(Dx& dx,uint32_t w,uint32_t h,uint32_t b,uint32_t s)
    {
        width=w;height=h;bytes=b;seed=s;format=b==1?DXGI_FORMAT_R8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
        if(b==1)state|=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;
        desc.DepthOrArraySize=desc.MipLevels=1;desc.Format=format;desc.SampleDesc.Count=1;
        if(FAILED(dx.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&texture))))return false;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 size=0;
        dx.device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&size);
        upload=dx.Buffer(D3D12_HEAP_TYPE_UPLOAD,size);if(!upload)return false;
        unsigned char* mapped=nullptr;D3D12_RANGE empty{0,0};upload->Map(0,&empty,(void**)&mapped);
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x)for(uint32_t c=0;c<b;++c)
            mapped[y*fp.Footprint.RowPitch+x*b+c]=Pattern(x,y,c,s);
        upload->Unmap(0,nullptr);dx.Begin();
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
        dst.pResource=texture.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dx.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        dx.Transition(texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,state);dx.Execute();dx.WaitIdle();
        return true;
    }
};
bool Same(const Source& s,const unsigned char* data,uint32_t rowPitch)
{
    for(uint32_t y=0;y<s.height;++y)for(uint32_t x=0;x<s.width;++x)for(uint32_t c=0;c<s.bytes;++c)
        if(data[y*rowPitch+x*s.bytes+c]!=Pattern(x,y,c,s.seed))return false;
    return true;
}
bool Dx11RoundTrip(Dx& dx)
{
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    if(FAILED(D3D11CreateDevice(dx.adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)))return false;
    native::DebugPreviewDx11 preview;bool ok=true;
    const uint32_t sizes[][2]={{641,361},{320,180},{960,540},{641,361}};
    for(uint32_t frame=0;frame<12;++frame)
    {
        const auto& size=sizes[frame%4];preview.BeginFrame();
        for(uint32_t b:{4u,1u})
        {
            Source s;if(!s.Create(dx,size[0],size[1],b,frame*31+b))return false;
            auto* srv=preview.Export(device.Get(),dx.queue.Get(),s.texture.Get(),s.format,s.state);
            if(!srv){printf("D3D11 export: %s\n",preview.Error().c_str());return false;}
            ComPtr<ID3D11Resource> resource;srv->GetResource(&resource);ComPtr<ID3D11Texture2D> texture;resource.As(&texture);
            D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> readback;if(FAILED(device->CreateTexture2D(&desc,nullptr,&readback)))return false;
            context->CopyResource(readback.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE map{};
            if(FAILED(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&map)))return false;
            ok&=Same(s,(unsigned char*)map.pData,map.RowPitch);context->Unmap(readback.Get(),0);
        }
        preview.EndFrame();
    }
    ok&=preview.Reset();
    return Check("D3D12 -> D3D11: RGBA arrows + R8 mask, 12 frames, odd sizes and repeated resize",ok) &&
        Check("D3D11/D3D12 devices healthy",SUCCEEDED(device->GetDeviceRemovedReason())&&SUCCEEDED(dx.device->GetDeviceRemovedReason()));
}
bool VkRoundTrip(Vk& vk,Dx& dx)
{
    native::DebugPreviewVk preview;bool ok=true;
    const uint32_t sizes[][2]={{641,361},{320,180},{960,540},{641,361}};
    for(uint32_t frame=0;frame<12;++frame)
    {
        const auto& size=sizes[frame%4];
        if(!preview.BeginFrame(vk.device,vk.physical,vk.family,frame%3))return false;
        Source source[2];native::DebugPreviewVk::Image* image[2]{};GameImage out[2];
        for(int i=0;i<2;++i)
        {
            const uint32_t bytes=i?1:4;
            if(!source[i].Create(dx,size[0],size[1],bytes,frame*31+bytes))return false;
            image[i]=preview.Export(dx.queue.Get(),source[i].texture.Get(),source[i].format,source[i].state,i!=0);
            if(!image[i]){printf("Vulkan export: %s\n",preview.Error().c_str());return false;}
            if(!out[i].Create(vk,i?VK_FORMAT_R8_UNORM:VK_FORMAT_R8G8B8A8_UNORM,size[0],size[1],bytes))return false;
        }
        vk.Begin();preview.RecordTransfers(vk.cmd);
        for(int i=0;i<2;++i)
        {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            barrier.image=image[i]->image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdPipelineBarrier(vk.cmd,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={size[0],size[1],1};
            vkCmdCopyImageToBuffer(vk.cmd,image[i]->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,out[i].buffer,1,&copy);
            std::swap(barrier.oldLayout,barrier.newLayout);std::swap(barrier.srcAccessMask,barrier.dstAccessMask);
            vkCmdPipelineBarrier(vk.cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        }
        if(!vk.Submit(preview.WaitSemaphore(),preview.WaitValue(),0,true))return false;
        for(int i=0;i<2;++i){ok&=Same(source[i],(unsigned char*)out[i].mapped,size[0]*source[i].bytes);out[i].Destroy();}
    }
    preview.Reset();
    return Check("D3D12 -> Vulkan sampled images: RGBA arrows + grayscale view, 12 frames, resize",ok) &&
        Check("D3D12 device healthy after Vulkan diagnostics",SUCCEEDED(dx.device->GetDeviceRemovedReason()));
}
}
int main()
{
    Vk vk;if(!vk.Init())return 2;LUID luid{};if(!native::VkPhysicalDeviceLuid(vk.physical,luid))return 2;
    Dx dx;if(!dx.Init(luid))return 2;
    bool ok=Dx11RoundTrip(dx);ok&=VkRoundTrip(vk,dx);dx.WaitIdle();
    ok&=Check("no reported D3D12 validation errors",dx.DebugErrors()==0);
    vkDeviceWaitIdle(vk.device);vkDestroyCommandPool(vk.device,vk.pool,nullptr);vkDestroyDevice(vk.device,nullptr);vkDestroyInstance(vk.instance,nullptr);
    printf("Cross-API F5Low diagnostic GPU transport: %s\n",ok?"PASS":"FAIL");return ok?0:1;
}
