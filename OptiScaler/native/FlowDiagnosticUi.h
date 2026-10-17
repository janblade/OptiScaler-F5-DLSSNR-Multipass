#pragma once

#include "NativeDriver.h"
#include <Config.h>
#include <menu/menu_overlay_base.h>
#include <imgui/imgui.h>
#include <algorithm>
#include <atomic>
#include <iterator>

// Shared controls for the D3D11 and Vulkan diagnostic transports. Texture IDs are
// supplied by the active renderer; a D3D12 descriptor is never sent to another API.
namespace native
{
class FlowDiagnosticUi
{
    inline static constexpr const char* views[] = {
        "Final optical flow (fused direction and speed)", "Local texture confidence", "Camera homography motion",
        "Selective geometric fusion source", "Photometric support", "Homography fit quality", "Raw flow before fusion",
        "Motion arrows (colored grid)", "Selective geometric fusion rejection only", "Geometric model only (warped grid)", "SGF rejection reason"};
    std::atomic<bool> _wanted {false};
    std::atomic<int> _requested {7};
    int _view = 7;
    std::atomic<int> _display {0};
  public:
    int View() const { return _requested.load(std::memory_order_acquire); }
    bool Expanded() const { return _display != 0 && Config::Instance()->DlssNrNativeMotion.value_or_default() &&
                                   Config::Instance()->DlssNrNativeDebugView.value_or_default(); }
    bool TakeRequest()
    {
        return Config::Instance()->DlssNrNativeDebugView.value_or_default() &&
               (_wanted.exchange(false,std::memory_order_acq_rel) || Expanded());
    }
    void Reset() { _wanted=false; _display=0; }

    template<class Show> void DrawAdvanced(NativeDriver& driver, Show&& show)
    {
        if (!Config::Instance()->DlssNrNativeDebugView.value_or_default()) { Reset(); return; }
        ImGui::SetNextItemWidth(300.0f);
        ImGui::Combo("F5Low diagnostic view##f5flowdebug",&_view,views,(int)std::size(views));
        _requested.store(_view,std::memory_order_release);
        if (_view==3) ImGui::TextDisabled("Black: invalid. Blue: local flow. Orange: homography. Green: blend.");
        if (_view==10) ImGui::TextWrapped("Black: geometry used. Magenta: global model invalid. Orange: projection invalid/outside image. Yellow: photometric mismatch. Cyan: local flow retained. Red: no selected hypothesis.");
        if (_view==8) ImGui::TextWrapped("White: geometry not used (local-only or invalid). Black: geometry accepted, including blends. No model means all white. This is not an occlusion mask.");
        if (_view==9) ImGui::TextWrapped("Cyan: fitted projective grid. Dark gray: fixed reference grid. Black if no valid model. No optical-flow or game-picture background.");
        if (ImGui::Button("Enlarge preview")) _display=1;
        ImGui::SameLine();
        if (ImGui::Button("Fullscreen preview")) _display=2;
        auto* producer=driver.Producer();
        if (_view==7 && producer)
        {
            ImGui::TextWrapped("Arrows follow each region's visible motion. Length is scaled in game pixels/frame.");
            if (ImGui::TreeNode("Arrow appearance##f5lowarrows"))
            {
                auto& arrows=producer->Flow()->DebugTuning();
                ImGui::SliderFloat("Grid spacing (px)##arrowgrid",&arrows.spacing,16.f,128.f,"%.0f");
                ImGui::SliderFloat("Length gain##arrowgain",&arrows.gain,.25f,8.f,"%.2f");
                ImGui::SliderFloat("Minimum motion (px/frame)##arrowminimum",&arrows.minimumMotion,.05f,4.f,"%.2f");
                ImGui::SliderFloat("Line width##arrowwidth",&arrows.thickness,.5f,3.f,"%.2f");
                ImGui::SliderFloat("Arrow opacity##arrowopacity",&arrows.opacity,0.f,1.f,"%.2f");
                ImGui::SliderFloat("Grid opacity##arrowgridopacity",&arrows.gridOpacity,0.f,.5f,"%.2f");
                ImGui::Checkbox("Show captured game picture##arrowbackground",&arrows.background);
                if (ImGui::Button("Reset arrow appearance##arrowdefaults")) arrows=OpticalFlowDx12::DebugSettings{};
                ImGui::TreePop();
            }
        }
        constexpr float width=360.f,height=202.5f;
        const bool running=driver.GetStatus()==NativeDriver::Status::Running;
        _wanted.store(true,std::memory_order_release);
        bool drawn=running && producer && producer->PreviewReady() &&
            show(producer->Flow()->Preview(),DXGI_FORMAT_R8G8B8A8_UNORM,false,width,height);
        if (!drawn) { ImGui::Dummy(ImVec2(width,height)); ImGui::TextDisabled("Waiting for the F5Low diagnostic image."); }
        ImGui::TextDisabled("Trust: white means the previous frame is distrusted (%llu cuts).",(unsigned long long)driver.Cuts());
        driver.DrawTrustViewCombo();
        if (!(running && driver.TrustRecent() && producer &&
              show(producer->Trust()->Mask(),DXGI_FORMAT_R8_UNORM,true,width,height))) ImGui::Dummy(ImVec2(width,height));
    }

    template<class Get> void DrawExpanded(NativeDriver& driver, Get&& get)
    {
        if (!Expanded()) { _display=0; return; }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { _display=0; return; }
        _wanted.store(true,std::memory_order_release);
        auto* producer=driver.Producer();
        const bool ready=driver.GetStatus()==NativeDriver::Status::Running && producer && producer->PreviewReady();
        auto texture=ready?get(producer->Flow()->Preview(),DXGI_FORMAT_R8G8B8A8_UNORM,false):ImTextureID(0);
        auto* viewport=ImGui::GetMainViewport();
        if (_display==2)
        {
            if (texture) ImGui::GetBackgroundDrawList(viewport)->AddImage(texture,viewport->Pos,
                ImVec2(viewport->Pos.x+viewport->Size.x,viewport->Pos.y+viewport->Size.y));
            if (MenuOverlayBase::IsVisible())
            {
                ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x+16,viewport->WorkPos.y+16),ImGuiCond_Always);
                if (ImGui::Begin("F5Low preview controls##f5flowfullscreen",nullptr,
                    ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize))
                {
                    ImGui::TextUnformatted(views[std::clamp(_view,0,(int)std::size(views)-1)]);
                    if (ImGui::Button("Exit fullscreen preview")) _display=0;
                }
                ImGui::End();
            }
            return;
        }
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x*.82f,viewport->WorkSize.y*.82f),ImGuiCond_Appearing);
        bool open=true;
        if (ImGui::Begin("F5Low enlarged diagnostic##f5flowexpanded",&open,ImGuiWindowFlags_NoSavedSettings))
        {
            ImGui::TextUnformatted(views[std::clamp(_view,0,(int)std::size(views)-1)]);
            if (texture)
            {
                const auto desc=producer->Flow()->Preview()->GetDesc();
                const float aspect=(float)desc.Width/desc.Height;
                const auto space=ImGui::GetContentRegionAvail();
                const float width=std::min(space.x,space.y*aspect);
                if(width>0)ImGui::Image(texture,ImVec2(width,width/aspect));
            }
            else ImGui::TextDisabled("Waiting for the F5Low diagnostic image.");
        }
        ImGui::End(); if(!open)_display=0;
    }
};
}
