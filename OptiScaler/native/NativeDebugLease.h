#pragma once
#include "NativeDriver.h"
#include "NativeDebugDevice.h"
#include <Config.h>
#include <menu/menu_overlay_dx.h>
#include <imgui/imgui.h>
#include <mutex>
#include <cstring>

namespace native
{
inline bool DebugRenderer(const char* name)
{
    return ImGui::GetCurrentContext() && ImGui::GetIO().BackendRendererName &&
           std::strcmp(ImGui::GetIO().BackendRendererName,name)==0;
}
// While an interop game's menu is presented through a D3D12 FG swapchain, show the
// producer resource directly. Its CPU lease and GPU read fence span the menu submit.
class NativeDebugLease
{
    std::unique_lock<std::mutex> _lock;
    NativeProducer* _producer=nullptr;
    bool _sampled=false;
  public:
    void Begin(std::mutex& mutex,NativeDriver& driver,ID3D12CommandQueue* queue,ID3D12Fence* menuFence)
    {
        if(!Config::Instance()->DlssNrNativeDebugView.value_or_default() || !queue)return;
        _lock=std::unique_lock(mutex,std::try_to_lock);
        if(!_lock.owns_lock())return;
        _producer=driver.Producer();_sampled=false;
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        if(!_producer || FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
        {End();return;}
        const auto ready=_producer->LastSubmission();
        if(!SameDebugDevice(device.Get(),_producer->Device(),ready.fence,menuFence))
        {End();return;}
        if(ready.fence && FAILED(queue->Wait(ready.fence,ready.value)))End();
    }
    bool Held() const {return _lock.owns_lock();}
    ImTextureID Texture(ID3D12Resource* texture,DXGI_FORMAT format,bool gray)
    {
        if(!Held() || !texture)return 0;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format=format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=1;
        view.Shader4ComponentMapping=gray?D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0,0,0,D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1):D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
        if(!MenuOverlayDx::FrameTextureView(texture,view,gpu))return 0;
        _sampled=true;return (ImTextureID)gpu.ptr;
    }
    void End(ID3D12Fence* fence=nullptr,UINT64 value=0)
    {
        if(!Held())return;
        if(_sampled && fence && _producer)_producer->PreviewReadSubmitted(fence,value);
        _producer=nullptr;_sampled=false;_lock.unlock();
    }
};
}
