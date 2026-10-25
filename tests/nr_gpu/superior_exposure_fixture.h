#pragma once
#include "superior_input_fixture.h"
#include "superior_numeric_metrics.h"
#include "../../OptiScaler/dlssnr/DlssNr_SuperiorScaling.h"
// Full-resolution mean -> protected white point -> unchanged encode and Area. All resources resident.
struct ExposureFixture {
 Runner& r;InputFixture input;ComPtr<ID3D12Resource> meter,white,meterCb,exposureCb;
 ExposureFixture(Runner& runner,UINT nw,UINT nh,UINT ww,UINT wh,bool halfSource,bool halfOutput,int stress,const DlssNrConstants& parameters):r(runner),input(runner,nw,nh,ww,wh,parameters,stress,halfOutput,halfSource){
  auto desc=input.exposure->GetDesc();desc.Width=desc.Height=64;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  meter=r.resource(desc,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  desc.Width=desc.Height=1;white=r.resource(desc,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  input.exposure=white;input.table(0,input.source.Get(),input.outputs[0].color.Get(),input.outputs[0].keep.Get());
  input.table(32,input.source.Get(),meter.Get(),meter.Get());input.table(40,meter.Get(),white.Get(),white.Get());
  auto c=parameters;c.DirectResolveFlags=halfSource?DlssNr::SuperiorScaling::ShaderFlag:0u;c.Mode=3;c.Width=c.Height=64;c.ExposureSourceWidth=nw;c.ExposureSourceHeight=nh;meterCb=input.cb(c);
  if(DlssNr::SuperiorScaling::ExposureStage(c,nw,nh)!=(halfSource?1u:0u))throw std::runtime_error("Meter selector failed");
  c.Mode=11;c.Width=c.Height=1;exposureCb=input.cb(c);
  if(DlssNr::SuperiorScaling::ExposureStage(c,64,64)!=(halfSource?2u:0u))throw std::runtime_error("Exposure selector failed");
  // Unused t3 in meter/reduction aliases their readable source, rather than the not-yet-written white UAV.
  auto cpu=r.heap->GetCPUDescriptorHandleForHeapStart();
  for(UINT offset : {32u,40u}) {
   auto* resource=offset==32?input.source.Get():meter.Get();D3D12_SHADER_RESOURCE_VIEW_DESC v{};
   v.Format=resource->GetDesc().Format;v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
   v.Texture2D.MipLevels=1;v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
   r.device->CreateShaderResourceView(resource,&v,{cpu.ptr+SIZE_T(offset+3)*r.stride});
  }
 }
 static DlssNrConstants settings(float protection){DlssNrConstants c{};c.WhitePoint=2;c.PreExposure=1;c.ExposureMode=3;c.ExposureTrim=1;c.ExposureProtection=protection;return c;}
 void prepare(ID3D12PipelineState* first,ID3D12PipelineState* second,ID3D12PipelineState* encode,ID3D12PipelineState* area){
  // Mode 3 dispatches 64x64 GROUPS, not 64x64 threads.
  input.dispatch(first,meterCb.Get(),32,512,512);
  r.barrier(meter.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  input.dispatch(second,exposureCb.Get(),40,1,1);
  r.barrier(white.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  input.prepare(0,encode,area,2);
 }
 void rest(){input.rest(0,2);r.barrier(meter.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);r.barrier(white.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);}
 std::vector<std::array<double,4>> batch(ID3D12PipelineState* a,ID3D12PipelineState* b,ID3D12PipelineState* c,ID3D12PipelineState* d,ID3D12PipelineState* encode,ID3D12PipelineState* area,bool reverse,UINT count){
  if(count>4)throw std::runtime_error("Timestamp capacity exceeded");
  r.begin();input.roots();for(UINT i=0;i<count;++i)for(UINT pos=0;pos<4;++pos){bool candidate=(pos==1||pos==2)!=reverse;UINT q=(i*4+pos)*2;
   r.list->EndQuery(input.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,q);
   for(UINT repeat=0;repeat<4;++repeat){prepare(candidate?c:a,candidate?d:b,encode,area);rest();}
   r.list->EndQuery(input.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,q+1);
  }r.list->ResolveQueryData(input.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,count*8,input.times.Get(),0);r.finish();
  UINT64* stamps;check(input.times->Map(0,nullptr,reinterpret_cast<void**>(&stamps)));std::vector<std::array<double,4>> out(count);
  for(UINT i=0;i<count;++i)for(UINT j=0;j<4;++j){UINT q=(i*4+j)*2;out[i][j]=double(stamps[q+1]-stamps[q])*1000/input.frequency/4;}
  input.times->Unmap(0,nullptr);return out;
 }
};
