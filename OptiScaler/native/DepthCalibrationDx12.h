#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include "DepthCalibrationBytecode.h"
namespace native {
// Point-sample raw depth into picture coordinates before any motion/trust decision.
// The producer owns queue/fence ordering; descriptors use its retired ring slot.
class DepthCalibrationDx12 {
 template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
 Ptr<ID3D12RootSignature> root;Ptr<ID3D12PipelineState> pipeline;
 Ptr<ID3D12DescriptorHeap> heap;Ptr<ID3D12Resource> target;
 UINT increment=0,w=0,h=0;bool readable=false;
public:
 bool Init(ID3D12Device* device){
  if(pipeline)return true;
  D3D12_DESCRIPTOR_RANGE ranges[2]{};ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1};
  D3D12_ROOT_PARAMETER p[2]{};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;p[0].DescriptorTable={2,ranges};p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[1].Constants={0,0,8};
  D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=2;desc.pParameters=p;Ptr<ID3DBlob> code,error;
  if(FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&code,&error)) || FAILED(device->CreateRootSignature(0,code->GetBufferPointer(),code->GetBufferSize(),IID_PPV_ARGS(&root))))return false;
  D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=root.Get();ps.CS={DepthCalibrationBytecode::code,sizeof(DepthCalibrationBytecode::code)};
  if(FAILED(device->CreateComputePipelineState(&ps,IID_PPV_ARGS(&pipeline))))return false;
  D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=6;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)))){pipeline.Reset();return false;}
  increment=device->GetDescriptorHandleIncrementSize(hd.Type);return true;
 }
 ID3D12Resource* Run(ID3D12Device* device,ID3D12GraphicsCommandList* list,ID3D12Resource* source,DXGI_FORMAT view,UINT width,UINT height,UINT slot,const float* transform){
  if(!source || !width || !height || slot>=3 || !Init(device))return nullptr;
  if(!target || width!=w || height!=h){
   D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;d.DepthOrArraySize=1;d.MipLevels=1;d.Format=DXGI_FORMAT_R32_FLOAT;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
   D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;Ptr<ID3D12Resource> fresh;
   if(FAILED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&fresh))))return nullptr;
   target=std::move(fresh);w=width;h=height;readable=false;target->SetName(L"Aligned native raw depth");
  }
  auto cpu=heap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=SIZE_T(slot)*2*increment;
  D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=view;sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sv.Texture2D.MipLevels=1;device->CreateShaderResourceView(source,&sv,cpu);
  D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};uv.Format=DXGI_FORMAT_R32_FLOAT;uv.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;auto ucpu=cpu;ucpu.ptr+=increment;device->CreateUnorderedAccessView(target.Get(),nullptr,&uv,ucpu);
  D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={target.Get(),0,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};if(readable)list->ResourceBarrier(1,&b);
  struct Constants{float scale[2],offset[2];UINT size[2],sourceSize[2];}c{};
  for(int i=0;i<2;++i)c.scale[i]=std::isfinite(transform[i])?(std::max)(transform[i],0.01f):1.f;
  c.offset[0]=std::isfinite(transform[2])?-transform[2]/width:0.f;c.offset[1]=std::isfinite(transform[3])?transform[3]/height:0.f;c.size[0]=width;c.size[1]=height;const auto sd=source->GetDesc();c.sourceSize[0]=UINT(sd.Width);c.sourceSize[1]=sd.Height;
  list->SetComputeRootSignature(root.Get());ID3D12DescriptorHeap* heaps[]={heap.Get()};list->SetDescriptorHeaps(1,heaps);list->SetPipelineState(pipeline.Get());auto gpu=heap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=UINT64(slot)*2*increment;
  list->SetComputeRootDescriptorTable(0,gpu);list->SetComputeRoot32BitConstants(1,8,&c,0);list->Dispatch((width+7)/8,(height+7)/8,1);
  b.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;b.Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;list->ResourceBarrier(1,&b);readable=true;return target.Get();
 }
};
}
