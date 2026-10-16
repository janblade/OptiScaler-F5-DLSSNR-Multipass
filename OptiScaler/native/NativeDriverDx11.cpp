#include "pch.h"

#include "NativeDriverDx11.h"

#include <native/Dx11FrameSource.h>
#include <native/NativeDriver.h>
#include <native/DebugPreviewDx11.h>
#include <native/FlowDiagnosticUi.h>
#include <native/NativeDebugLease.h>
#include <native/VkPresentBridge.h>

#include <Config.h>
#include <misc/IdentifyGpu.h>
#include <resource_tracking/GenericDepth_Dx11.h>

#include <imgui/imgui.h>
#include <d3dcompiler.h>
#include <mutex>

// The D3D11 counterpart of native/NativeDriverDx12.cpp: the same shared frame step (native::NativeDriver), a different
// adapter (native::Dx11FrameSource) and a diagnostic-only shared-SRV return path.

namespace
{

native::NativeDriver g_driver("Native motion (D3D11)",
                              ", on a private D3D12 device paired with the game's D3D11 device");
native::Dx11FrameSource g_source;
std::mutex g_runMutex;
native::DebugPreviewDx11 g_preview;
native::FlowDiagnosticUi g_ui;
native::NativeDebugLease g_dx12Lease;
Microsoft::WRL::ComPtr<ID3D11Device> g_menuDevice;
Microsoft::WRL::ComPtr<ID3D11DeviceContext> g_menuContext;
Microsoft::WRL::ComPtr<ID3D11PixelShader> g_grayShader;

ImTextureID DiagnosticTexture(ID3D12Resource* texture,DXGI_FORMAT format,bool gray)
{
    if(native::DebugRenderer("imgui_impl_dx12"))return g_dx12Lease.Texture(texture,format,gray);
    if(!native::DebugRenderer("imgui_impl_dx11") || !g_menuDevice || !g_source.Queue())return 0;
    const auto state=gray?D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    auto* srv=g_preview.Export(g_menuDevice.Get(),g_source.Queue(),texture,format,state);
    if(srv && g_driver.Producer())g_driver.Producer()->PreviewReadSubmitted(g_preview.CopyFence(),g_preview.CopiedValue());
    return (ImTextureID)(uintptr_t)srv;
}
bool ShowDiagnostic(ID3D12Resource* texture,DXGI_FORMAT format,bool gray,float width,float height)
{
    auto id=DiagnosticTexture(texture,format,gray);if(!id)return false;
    const bool grayDx11=gray && native::DebugRenderer("imgui_impl_dx11");
    if(grayDx11 && !g_grayShader)
    {
        const char code[]="Texture2D image:register(t0);SamplerState linearSampler:register(s0);struct I{float4 pos:SV_POSITION;float4 col:COLOR0;float2 uv:TEXCOORD0;};float4 main(I i):SV_Target{float v=image.Sample(linearSampler,i.uv).r;return i.col*float4(v,v,v,1);}";
        Microsoft::WRL::ComPtr<ID3DBlob> compiled;
        if(SUCCEEDED(D3DCompile(code,sizeof(code),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&compiled,nullptr)))
            g_menuDevice->CreatePixelShader(compiled->GetBufferPointer(),compiled->GetBufferSize(),nullptr,&g_grayShader);
    }
    if(grayDx11 && g_grayShader)ImGui::GetWindowDrawList()->AddCallback([](const ImDrawList*,const ImDrawCmd*)
        {g_menuContext->PSSetShader(g_grayShader.Get(),nullptr,0);},nullptr);
    ImGui::Image(id,ImVec2(width,height));
    if(grayDx11 && g_grayShader)ImGui::GetWindowDrawList()->AddCallback(ImDrawCallback_ResetRenderState,nullptr);
    return true;
}

// Shared by OnPresent and OnFGPresent; the depth finder's frame close is the caller's job (see the two entry points
// below), since only one of them needs to do it. Returns the D3D12 picture the producer ended up with this frame, or
// null when nothing ran.
ID3D12Resource* RunFrame(IDXGISwapChain* swapChain, ID3D11Device* device, bool copyBack)
{
    std::unique_lock lock(g_runMutex,std::try_to_lock);
    if(!lock.owns_lock())return nullptr;
    if (!g_driver.Enabled())
        return nullptr;

    if (swapChain == nullptr || device == nullptr || g_driver.Failed())
        return nullptr;

    g_source.SetPresent(swapChain, device, copyBack);

    // While Dx11wDx12SC has replaced the game's swap chain for frame generation this runs from OnFGPresent only, and the
    // bridge copies the result into frame generation's back buffer.
    return g_driver.RunFrame(g_source,g_ui.TakeRequest(),g_ui.View()).submitted ? g_source.ProcessedPicture() : nullptr;
}

} // namespace

namespace NativeMotionDx11
{

void OnPresent(IDXGISwapChain* swapChain, ID3D11Device* device)
{
    // DlssNrNativeDxvkVulkan: on a dxvk game this driver's shared-texture bridge stands aside for the Vulkan one
    // (NativeDriverVk.cpp), which reads dxvk's own Vulkan calls instead. OnFGPresent needs no such check: with the key on
    // a dxvk game never gets Dx11wDx12SC (hooks/DxgiFactory_Hooks.cpp); its frame generation is the Vulkan present bridge.
    if (VkPresentBridge::DxvkThroughVulkan())
        return;

    RunFrame(swapChain, device, true);
}

ID3D12Resource* OnFGPresent(IDXGISwapChain* real, ID3D11Device* device)
{
    // The one call site that closes the D3D11 depth finder's frame on this path; MenuOverlayDx::Present, which frame
    // generation's present still reaches, skips its own close under Dx11wDx12SC so this frame is not closed twice.
    GenericDepthDx11::OnPresent(real);
    // The bridge copies the D3D12 result into frame generation's back buffer itself: no copy back to D3D11
    return RunFrame(real, device, false);
}

void DrawStatus()
{
    std::unique_lock lock(g_runMutex,std::defer_lock);
    if(!g_dx12Lease.Held() && !lock.try_lock()){ImGui::TextDisabled("Motion processing in progress.");return;}
    g_driver.DrawStatus();
}

bool NrOnlyRunning()
{
    if(g_dx12Lease.Held())return g_driver.NrOnlyRunning();
    std::unique_lock lock(g_runMutex,std::try_to_lock);return lock.owns_lock() && g_driver.NrOnlyRunning();
}

void DrawAdvancedUi()
{
    std::unique_lock lock(g_runMutex,std::defer_lock);
    if(!g_dx12Lease.Held() && !lock.try_lock()){ImGui::TextDisabled("Motion processing in progress.");return;}
    g_driver.DrawFlowTuning();
    g_ui.DrawAdvanced(g_driver,ShowDiagnostic);
    if(!g_preview.Error().empty())ImGui::TextWrapped("Diagnostic: %s",g_preview.Error().c_str());
}

void BeginDebugFrame(ID3D11Device* device)
{
    std::lock_guard lock(g_runMutex);
    if(g_menuDevice && g_menuDevice.Get()!=device){g_preview.Reset();g_grayShader.Reset();}
    g_menuDevice=device;if(device)device->GetImmediateContext(&g_menuContext);g_preview.BeginFrame();
}
void EndDebugFrame(){std::lock_guard lock(g_runMutex);g_preview.EndFrame();}
void ReleaseDebugViews(){std::lock_guard lock(g_runMutex);g_preview.Reset();g_grayShader.Reset();}
void BeginDx12DebugFrame(ID3D12CommandQueue* queue){g_dx12Lease.Begin(g_runMutex,g_driver,queue);}
void EndDx12DebugFrame(ID3D12Fence* fence,UINT64 value){g_dx12Lease.End(fence,value);}
bool ExpandedPreviewVisible(){return g_ui.Expanded();}
void DrawExpandedPreview()
{
    std::unique_lock lock(g_runMutex,std::defer_lock);
    if(!g_dx12Lease.Held() && !lock.try_lock())return;
    g_ui.DrawExpanded(g_driver,DiagnosticTexture);
}

} // namespace NativeMotionDx11
