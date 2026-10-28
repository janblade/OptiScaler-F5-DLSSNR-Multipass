#pragma once

// The Optical F5Low debug view controls, shared by the D3D12, D3D11 and Vulkan drivers: the view chooser, its key, the
// arrow settings, the picture on the page and the enlarged / full screen window. Each driver hands in how its menu
// shows the producer's D3D12 picture (`texture` below: an ImTextureID its ImGui backend can draw, or 0 when it cannot
// this frame). Nothing is drawn or recorded while NativeDebugView is off. From PR #232 (TechvibeCoder07).

#include "FlowDebugViews.h"
#include "NativeDriver.h"

#include <Config.h>
#include <menu/menu_overlay_base.h>

#include <imgui/imgui.h>

#include <algorithm>
#include <atomic>

namespace native
{

class FlowDiagnosticUi
{
  public:
    // For the frame step: whether the menu wants a picture this frame (asked once per menu frame, taken here), and
    // which.
    bool TakeRequest()
    {
        return Config::Instance()->DlssNrNativeDebugView.value_or_default() &&
               _wanted.exchange(false, std::memory_order_acq_rel);
    }
    int View() const { return _view.load(std::memory_order_acquire); }

    // The enlarged or full screen picture is up: the menu draws its frame for it even when the menu itself is hidden.
    bool Expanded() const
    {
        return _display.load(std::memory_order_acquire) != Display::Inline &&
               Config::Instance()->DlssNrNativeMotion.value_or_default() &&
               Config::Instance()->DlssNrNativeDebugView.value_or_default();
    }

    void Reset()
    {
        _wanted.store(false, std::memory_order_release);
        _display.store(Display::Inline, std::memory_order_release);
    }

    // On the Optical F5Low page, inside the Advanced section, after the flow tuning.
    template <class Texture>
    void DrawAdvanced(NativeDriver& driver, Texture&& texture, const char* unavailable = nullptr)
    {
        if (!Config::Instance()->DlssNrNativeDebugView.value_or_default())
        {
            Reset();
            return;
        }

        int view = View();
        ImGui::SetNextItemWidth(300.0f);

        if (ImGui::Combo("Debug view##f5flowdebug", &view, FlowDebugViews::kNames, FlowDebugViews::kCount))
            _view.store(FlowDebugViews::Clamp(view), std::memory_order_release);

        ImGui::TextWrapped("%s", FlowDebugViews::Legend(view));

        // Where this menu cannot show the picture it is not asked for, and there is nothing to enlarge.
        if (unavailable != nullptr)
        {
            Reset();
            ImGui::TextDisabled("%s", unavailable);
            return;
        }

        if (ImGui::Button("Enlarge##f5flowenlarge"))
            _display.store(Display::Enlarged, std::memory_order_release);

        ImGui::SameLine();

        if (ImGui::Button("Full screen##f5flowfull"))
            _display.store(Display::FullScreen, std::memory_order_release);

        auto* producer = driver.Producer();

        if (view == 0 && producer != nullptr && ImGui::TreeNode("Arrow appearance##f5lowarrows"))
        {
            auto& arrows = producer->Flow()->DebugTuning();
            ImGui::SliderFloat("Grid spacing (px)##arrowgrid", &arrows.spacing, 16.0f, 128.0f, "%.0f");
            ImGui::SliderFloat("Length gain##arrowgain", &arrows.gain, 0.25f, 8.0f, "%.2f");
            ImGui::SliderFloat("Minimum motion (px/frame)##arrowminimum", &arrows.minimumMotion, 0.05f, 4.0f, "%.2f");
            ImGui::SliderFloat("Line width##arrowwidth", &arrows.thickness, 0.5f, 3.0f, "%.2f");
            ImGui::SliderFloat("Arrow opacity##arrowopacity", &arrows.opacity, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Grid opacity##arrowgridopacity", &arrows.gridOpacity, 0.0f, 0.5f, "%.2f");
            ImGui::Checkbox("Show the game's picture##arrowbackground", &arrows.background);

            if (arrows.background)
                ImGui::TextDisabled(
                    "The picture as the motion estimate saw it, before Neural Rendering. Off, the arrows "
                    "draw over the finished picture.");

            if (ImGui::Button("Reset arrow appearance##arrowdefaults"))
            {
                const auto look = arrows;
                arrows = OpticalFlowDx12::DebugSettings {};
                arrows.depthLinearize = look.depthLinearize; // the depth view's settings are the config's
                arrows.depthMultiplier = look.depthMultiplier;
                arrows.depthFarPlane = look.depthFarPlane;
            }

            ImGui::TreePop();
        }

        _wanted.store(true, std::memory_order_release);
        constexpr float width = 360.0f;
        ImTextureID id = 0;
        float height = width * 9.0f / 16.0f;

        if (ID3D12Resource* picture = Ready(driver))
        {
            const D3D12_RESOURCE_DESC desc = picture->GetDesc();
            height = desc.Width > 0 ? width * (float) desc.Height / (float) desc.Width : height;
            id = texture(picture);
        }

        if (id != 0)
            ImGui::Image(id, ImVec2(width, height));
        else
        {
            ImGui::Dummy(ImVec2(width, height));
            ImGui::TextDisabled("Waiting for the debug view's picture.");
        }
    }

    // Once per menu frame, after the main menu window, whether the menu is visible or not (see Expanded). `available`:
    // this menu can show the picture at all (as DrawAdvanced's `unavailable`).
    template <class Texture> void DrawExpanded(NativeDriver& driver, Texture&& texture, bool available = true)
    {
        if (!available || !Expanded())
        {
            _display.store(Display::Inline, std::memory_order_release);
            return;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            _display.store(Display::Inline, std::memory_order_release);
            return;
        }

        _wanted.store(true, std::memory_order_release);
        ID3D12Resource* picture = Ready(driver);
        const ImTextureID id = picture != nullptr ? texture(picture) : 0;
        const int view = View();
        ImGuiViewport* viewport = ImGui::GetMainViewport();

        if (_display.load(std::memory_order_acquire) == Display::FullScreen)
        {
            // Over the whole game picture, so the arrows sit on what they describe.
            if (id != 0)
                ImGui::GetBackgroundDrawList(viewport)->AddImage(
                    id, viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y));

            if (MenuOverlayBase::IsVisible())
            {
                ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 16.0f, viewport->WorkPos.y + 16.0f),
                                        ImGuiCond_Always);

                if (ImGui::Begin("Optical F5Low debug view##f5flowfullscreen", nullptr,
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration |
                                     ImGuiWindowFlags_AlwaysAutoResize))
                {
                    ImGui::TextUnformatted(FlowDebugViews::Name(view));
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
                    ImGui::TextUnformatted(FlowDebugViews::Legend(view));
                    ImGui::PopTextWrapPos();

                    if (ImGui::Button("Leave full screen (Esc)##f5flowleave"))
                        _display.store(Display::Inline, std::memory_order_release);
                }

                ImGui::End();
            }

            return;
        }

        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.82f, viewport->WorkSize.y * 0.82f),
                                 ImGuiCond_Appearing);
        bool open = true;

        if (ImGui::Begin("Optical F5Low debug view##f5flowexpanded", &open, ImGuiWindowFlags_NoSavedSettings))
        {
            ImGui::TextUnformatted(FlowDebugViews::Name(view));
            ImGui::TextWrapped("%s", FlowDebugViews::Legend(view));

            if (id != 0)
            {
                const D3D12_RESOURCE_DESC desc = picture->GetDesc();
                const float aspect = desc.Height > 0 ? (float) desc.Width / (float) desc.Height : 16.0f / 9.0f;
                const ImVec2 space = ImGui::GetContentRegionAvail();
                const float width = (std::min) (space.x, space.y * aspect);

                if (width > 0.0f)
                    ImGui::Image(id, ImVec2(width, width / aspect));
            }
            else
                ImGui::TextDisabled("Waiting for the debug view's picture.");
        }

        ImGui::End();

        if (!open)
            _display.store(Display::Inline, std::memory_order_release);
    }

  private:
    enum class Display
    {
        Inline,
        Enlarged,
        FullScreen
    };

    // The producer's picture, once it drew one.
    static ID3D12Resource* Ready(NativeDriver& driver)
    {
        auto* producer = driver.Producer();
        return driver.GetStatus() == NativeDriver::Status::Running && producer != nullptr && producer->PreviewReady()
                   ? producer->Flow()->Preview()
                   : nullptr;
    }

    std::atomic<bool> _wanted { false };
    std::atomic<int> _view { 0 };
    std::atomic<Display> _display { Display::Inline };
};

} // namespace native
