#pragma once
// Test-only readbacks; include after nr_optical_flow_gpu.cpp's fixture definitions.
static std::vector<unsigned char> ReadMotionBytes(Gpu& gpu,ID3D12Resource* out,unsigned stride,D3D12_RESOURCE_STATES state){
 auto d=out->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 total=0;
 gpu.device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&total);
 auto read=gpu.Buffer(total,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
 D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={out,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,state,D3D12_RESOURCE_STATE_COPY_SOURCE};
 gpu.list->ResourceBarrier(1,&b);D3D12_TEXTURE_COPY_LOCATION src{},dst{};
 src.pResource=out;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.pResource=read.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
 gpu.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);gpu.list->ResourceBarrier(1,&b);gpu.Submit();
 unsigned char* p=nullptr;read->Map(0,nullptr,(void**)&p);std::vector<unsigned char> result(size_t(d.Width)*d.Height*stride);
 for(unsigned y=0;y<d.Height;++y)memcpy(result.data()+size_t(y)*d.Width*stride,p+size_t(y)*fp.Footprint.RowPitch,size_t(d.Width)*stride);
 read->Unmap(0,nullptr);return result;
}
static std::vector<unsigned char> ReadPreview(Gpu& gpu,ID3D12Resource* out){return ReadMotionBytes(gpu,out,4,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);}
static std::vector<float> ReadFlow(Gpu& gpu,ID3D12Resource* out,D3D12_RESOURCE_STATES state){
 auto b=ReadMotionBytes(gpu,out,8,state);std::vector<float> result(b.size()/4);
 for(size_t i=0;i<b.size()/8;++i){uint16_t h[4];memcpy(h,b.data()+i*8,8);result[2*i]=Half(h[0]);result[2*i+1]=Half(h[1]);}return result;
}
static std::vector<float> ReadConfidence(Gpu& gpu,ID3D12Resource* out){
 auto b=ReadMotionBytes(gpu,out,8,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);std::vector<float> result(b.size()/8);
 for(size_t i=0;i<result.size();++i){uint16_t h;memcpy(&h,b.data()+i*8+4,2);result[i]=Half(h);}return result;
}
