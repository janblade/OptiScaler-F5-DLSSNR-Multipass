#include "pch.h"
#include "StatusPage.h"
#include "StatusPageRules.h"

#include "MenuCard.h"
#include "menu_common.h"

#include <Config.h>
#include <State.h>
#include <dlssnr/DlssNrFeature_Dx12.h>
#include <framegen/FGOutput.h>
#include <framegen/f5low/F5Low.h>
#include <misc/GpuMemory.h>
#include <misc/IdentifyGpu.h>
#include <output/SdrToHdr_Create.h>
#include <plugins/PluginRegistry.h>
#include <proxies/FfxApi_Proxy.h>
#include <proxies/XeSS_Proxy.h>
#include <upscalers/IFeature.h>

namespace
{
using namespace StatusPageRules;

// The rules mirror these enums by value, every value
static_assert((int) Api::NotSelected == (int) API::NotSelected && (int) Api::DX11 == (int) API::DX11 &&
              (int) Api::DX12 == (int) API::DX12 && (int) Api::Vulkan == (int) API::Vulkan);
static_assert((uint32_t) FgInput::NoFG == (uint32_t) FGInput::NoFG &&
              (uint32_t) FgInput::Upscaler == (uint32_t) FGInput::Upscaler &&
              (uint32_t) FgInput::DLSSG == (uint32_t) FGInput::DLSSG &&
              (uint32_t) FgInput::NvngxFG == (uint32_t) FGInput::NvngxFG &&
              (uint32_t) FgInput::FSRFG == (uint32_t) FGInput::FSRFG &&
              (uint32_t) FgInput::FSRFG30 == (uint32_t) FGInput::FSRFG30 &&
              (uint32_t) FgInput::XeFG == (uint32_t) FGInput::XeFG &&
              (uint32_t) FgInput::ForceXeLL == (uint32_t) FGInput::ForceXeLL);
static_assert((uint32_t) FgOutput::NoFG == (uint32_t) FGOutput::NoFG &&
              (uint32_t) FgOutput::FSRFG == (uint32_t) FGOutput::FSRFG &&
              (uint32_t) FgOutput::DLSSG == (uint32_t) FGOutput::DLSSG &&
              (uint32_t) FgOutput::XeFG == (uint32_t) FGOutput::XeFG &&
              (uint32_t) FgOutput::F5Low == (uint32_t) FGOutput::F5Low);
static_assert((uint32_t) FgNvngx::None == (uint32_t) FGNvngxReplacement::None &&
              (uint32_t) FgNvngx::Nukems == (uint32_t) FGNvngxReplacement::Nukems &&
              (uint32_t) FgNvngx::Arturs == (uint32_t) FGNvngxReplacement::Arturs &&
              (uint32_t) FgNvngx::FFX == (uint32_t) FGNvngxReplacement::FFX &&
              (uint32_t) FgNvngx::Combo == (uint32_t) FGNvngxReplacement::Combo &&
              (uint32_t) FgNvngx::Custom == (uint32_t) FGNvngxReplacement::Custom);
static_assert((int) NativeMode::Off == (int) DlssNrNativeMode::Shown::Off &&
              (int) NativeMode::NrOnly == (int) DlssNrNativeMode::Shown::NrOnly &&
              (int) NativeMode::NrAndFrameGeneration == (int) DlssNrNativeMode::Shown::NrAndFrameGeneration &&
              (int) NativeMode::MotionOnly == (int) DlssNrNativeMode::Shown::MotionOnly);

// The game's API: the upscaler input's, else the D3D11 bridge's, else the swap chain's (upstream's fallback), so
// games with an FSR or XeSS input, or none (Optical F5Low), show one too
API GameApi()
{
    const auto& state = State::Instance();

    if (state.api != NotSelected)
        return state.api;

    if (state.swapchainInteropApi == SwapchainInteropApi::Dx11wDx12)
        return DX11;

    return state.swapchainApi;
}

const ImVec4 kOk(0.45f, 0.85f, 0.45f, 1.0f);
const ImVec4 kWarn(1.0f, 0.85f, 0.3f, 1.0f);

std::string StrFmt(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int length = std::vsnprintf(nullptr, 0, fmt, args);
    va_end(args);

    if (length <= 0)
        return {};

    std::string out(length, '\0');
    va_start(args, fmt);
    std::vsnprintf(out.data(), length + 1, fmt, args);
    va_end(args);
    return out;
}

// Filled dot for on / found, hollow dot for off / missing, in the next table cell
void Flag(const char* label, bool on, bool warnWhenOn = false)
{
    ImGui::TableNextColumn();

    const float lineHeight = ImGui::GetTextLineHeight();
    const float radius = lineHeight * 0.28f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 center(pos.x + lineHeight * 0.5f, pos.y + lineHeight * 0.5f);
    auto drawList = ImGui::GetWindowDrawList();

    if (on)
        drawList->AddCircleFilled(center, radius, ImGui::GetColorU32(warnWhenOn ? kWarn : kOk));
    else
        drawList->AddCircle(center, radius, ImGui::GetColorU32(ImGuiCol_TextDisabled), 0, 1.5f);

    ImGui::Dummy(ImVec2(lineHeight, lineHeight));
    ImGui::SameLine(0.0f, 0.0f);

    if (on)
        ImGui::TextUnformatted(label);
    else
        ImGui::TextDisabled("%s", label);
}

// Label and value, two pairs per row
void Pair(const char* label, const std::string& value, bool dim = false)
{
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);

    ImGui::TableNextColumn();
    if (dim)
        ImGui::TextDisabled("%s", value.c_str());
    else
        ImGui::TextWrapped("%s", value.c_str());
}

// A long value on a row of its own
void Row(const char* label, const std::string& value)
{
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", value.c_str());
}

void SetupPairs()
{
    ImGui::TableSetupColumn("##l1", ImGuiTableColumnFlags_WidthStretch, 0.9f);
    ImGui::TableSetupColumn("##v1", ImGuiTableColumnFlags_WidthStretch, 1.1f);
    ImGui::TableSetupColumn("##l2", ImGuiTableColumnFlags_WidthStretch, 0.9f);
    ImGui::TableSetupColumn("##v2", ImGuiTableColumnFlags_WidthStretch, 1.1f);
}

bool BeginRows(const char* id)
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit))
        return false;

    ImGui::TableSetupColumn("##label");
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

std::string Join(const std::vector<std::string>& items)
{
    std::string text;

    for (const auto& item : items)
        text += text.empty() ? item : ", " + item;

    return text;
}

// The asi files the loader opened (the Plugins page has the rest)
std::string LoadedAsiPlugins()
{
    std::vector<std::string> names;

    for (const auto& entry : Plugins::Registry::Global().Snapshot())
    {
        if (entry.kind == Plugins::Kind::Asi &&
            (entry.state == Plugins::State::Loaded || entry.state == Plugins::State::LoadedLate))
        {
            names.push_back(wstring_to_string(entry.name));
        }
    }

    return Join(names);
}

void RenderSystem(Config* config, IFeature* feature, const GpuInformation& gpu, float menuResScale)
{
    auto& state = State::Instance();
    const bool active = feature != nullptr && feature->IsInited() && !feature->IsFrozen();

    if (MenuCard card("System", menuResScale); card.IsOpen())
    {
        if (BeginRows("statusSystem"))
        {
            Row("GPU", gpu.name);
            Row("Game", state.gameExe + (state.gameName.empty() ? "" : " (" + state.gameName + ")"));

            if (!state.detectedQuirks.empty())
                Row("Quirks", Join(state.detectedQuirks));

            if (const auto asi = LoadedAsiPlugins(); !asi.empty())
                Row("ASI", asi);

            ImGui::EndTable();
        }

        if (ImGui::BeginTable("statusRuntime", 4, ImGuiTableFlags_SizingStretchProp))
        {
            SetupPairs();

            Pair("API", GameApiText((Api) GameApi(), gpu.usesDxvk, gpu.usesVkd3dProton));
            Pair("Swap chain", ApiName((Api) state.swapchainApi));
            Pair("Input", ApiUpscalerInputName(state.currentInputApiName));

            if (active)
            {
                // ShortName: Name() already carries the version and " w/Dx12"
                auto version = feature->Version();
                Pair("Upscaler", StrFmt("%s %u.%u.%u%s", feature->ShortName().c_str(), version.major, version.minor,
                                        version.patch, feature->IsWithDx12() && state.api != DX12 ? " w/Dx12" : ""));

                const float renderWidth = (float) std::max(1u, (unsigned) feature->RenderWidth());
                Pair("Render", StrFmt("%dx%d", feature->RenderWidth(), feature->RenderHeight()));
                Pair("Output", StrFmt("%dx%d (%.2fx)", feature->TargetWidth(), feature->TargetHeight(),
                                      (float) feature->TargetWidth() / renderWidth));
                Pair("Display", StrFmt("%dx%d (%.2fx)", feature->DisplayWidth(), feature->DisplayHeight(),
                                       (float) feature->DisplayWidth() / renderWidth));
                Pair("Frames", std::to_string(feature->FrameCount()));
            }
            else
            {
                Pair("Upscaler", "None", true);
                Pair("Render", "-", true);
                Pair("Output", "-", true);
                Pair("Display", "-", true);
                Pair("Frames", "-", true);
            }

            Pair("FG Input", FgInputName((FgInput) state.activeFgInput), state.activeFgInput == FGInput::NoFG);
            Pair("FG Output", FgOutputName((FgOutput) state.activeFgOutput), state.activeFgOutput == FGOutput::NoFG);
            Pair("FG Nvngx", FgNvngxName((FgNvngx) state.activeFgNvngx, state.activeFgNvngxCustom),
                 state.activeFgNvngx == FGNvngxReplacement::None);

            const auto ratio = RatioOverrideText(config->UpscaleRatioOverrideEnabled.value_or_default(),
                                                 config->UpscaleRatioOverrideValue.value_or_default(),
                                                 config->QualityRatioOverrideEnabled.value_or_default());
            Pair("Ratio override", ratio, ratio == "Off");
            Pair("Mipmap bias",
                 config->MipmapBiasOverride.has_value() ? StrFmt("%.3f", config->MipmapBiasOverride.value()) : "Off",
                 !config->MipmapBiasOverride.has_value());
            Pair("Anisotropy",
                 config->AnisotropyOverride.has_value() ? std::to_string(config->AnisotropyOverride.value()) : "Auto",
                 !config->AnisotropyOverride.has_value());

            ImGui::EndTable();
        }
    }
}

void RenderDetected(const GpuInformation& gpu, float menuResScale)
{
    auto& state = State::Instance();

    if (MenuCard card("Detected", menuResScale); card.IsOpen())
    {
        if (ImGui::BeginTable("statusDetected", 4, ImGuiTableFlags_SizingStretchProp))
        {
            Flag("nvngx.dll", state.nvngxExists);
            Flag("nvngx_dlss", state.NVNGX_DLSS_Path.has_value());
            Flag("nvngx_dlssd", state.NVNGX_DLSSD_Path.has_value());
            Flag("nvngx replacement", state.nvngxReplacement.has_value());
            Flag("libxess", state.libxessExists || XeSSProxy::Module() != nullptr);
            Flag("FSR hooks", state.fsrHooks);
            Flag("FFX 3.1", FfxApiProxy::Dx12Module() != nullptr);
            Flag("FFX SR", FfxApiProxy::Dx12Module_SR() != nullptr);
            Flag("FFX FG", FfxApiProxy::Dx12Module_FG() != nullptr);
            Flag("FFX Vk", FfxApiProxy::VkModule() != nullptr);
            Flag("FFX Vk SR", FfxApiProxy::VkModule_SR() != nullptr);
            Flag("FFX Vk FG", FfxApiProxy::VkModule_FG() != nullptr);
            Flag("OptiPatcher", state.isOptiPatcherSucceed);
            Flag("Linux", state.isRunningOnLinux);
            Flag("DLSS capable GPU", gpu.dlssCapable);
            Flag("FSR4 capable GPU", gpu.fsr4Support != FSR4Support::None, IsFsr4Int8(gpu.fsr4Support));

            ImGui::EndTable();
        }
    }
}

void RenderHooks(Config* config, float menuResScale)
{
    if (MenuCard card("Hooks", menuResScale); card.IsOpen())
    {
        if (ImGui::BeginTable("statusHooks", 4, ImGuiTableFlags_SizingStretchSame))
        {
            Flag("DXGI spoofing", config->DxgiSpoofing.value_or_default());
            Flag("VLK spoofing", config->VulkanSpoofing.value_or_default());
            Flag("VLK ext. spoofing", config->VulkanExtensionSpoofing.value_or_default());
            Flag("Ntdll hooks", config->UseNtdllHooks.value_or_default());
            Flag("Disable overlays", config->DisableOverlays.value_or_default(), true);
            Flag("ASI plugins", config->LoadAsiPlugins.value_or_default());
            Flag("Precomp. shaders", config->UsePrecompiledShaders.value_or_default());
            Flag("Extended limits", config->ExtendedLimits.value_or_default(), true);
            Flag("DRS min override", config->DrsMinOverrideEnabled.value_or_default());
            Flag("DRS max override", config->DrsMaxOverrideEnabled.value_or_default());
            Flag("Compute root sig.", config->RestoreComputeSignature.value_or_default());
            Flag("Graphic root sig.", config->RestoreGraphicSignature.value_or_default());

            ImGui::EndTable();
        }

        // Only shown when something was overridden
        std::vector<std::string> barriers;
        auto addBarrier = [&](const char* name, bool set)
        {
            if (set)
                barriers.push_back(name);
        };
        addBarrier("Color", config->ColorResourceBarrier.has_value());
        addBarrier("Depth", config->DepthResourceBarrier.has_value());
        addBarrier("Motion", config->MVResourceBarrier.has_value());
        addBarrier("Exposure", config->ExposureResourceBarrier.has_value());
        addBarrier("Mask", config->MaskResourceBarrier.has_value());
        addBarrier("Output", config->OutputResourceBarrier.has_value());

        if (!barriers.empty())
        {
            ImGui::TextDisabled("Barriers overridden");
            ImGui::SameLine();
            ImGui::TextUnformatted(Join(barriers).c_str());
        }
    }
}

void RenderInitFlags(Config* config, IFeature* feature, float menuResScale)
{
    if (feature == nullptr || !feature->IsInited() || feature->IsFrozen())
        return;

    if (MenuCard card("Init Flags", menuResScale); card.IsOpen())
    {
        if (ImGui::BeginTable("statusInit", 4, ImGuiTableFlags_SizingStretchSame))
        {
            Flag("Auto exposure", feature->AutoExposure());
            Flag("Depth inverted", feature->DepthInverted());
            Flag("HDR", feature->IsHdr());
            Flag("Display res. MV", !feature->LowResMV());
            Flag("Jitter cancel.", feature->JitteredMV());
            Flag("Dynamic res.", feature->DynamicResolution());
            Flag("Output scaling", config->OutputScalingEnabled.value_or_default());
            Flag("Sharp. override", config->OverrideSharpness.value_or_default());
            Flag("V-Sync override", config->OverrideVsync.value_or_default());

            ImGui::EndTable();
        }
    }
}

void RenderFork(Config* config, float menuResScale)
{
    auto& state = State::Instance();

    if (MenuCard card("Neural Rendering & F5Low", menuResScale); card.IsOpen())
    {
        if (BeginRows("statusFork"))
        {
            const auto summary = DlssNr::CurrentStatusSummary(config);
            const bool nrUsable = summary.nrEnabled && summary.failure.empty();

            Row("Neural Rendering", NeuralRenderingText(summary.nrEnabled, summary.failure, summary.backend));

            // Its main settings, for a bug report (the Neural Rendering pages have all of them; profiles are files, so
            // which one was loaded last is not shown)
            if (summary.nrEnabled)
            {
                Row("  Model",
                    NrModelText(config->DlssNrPreset.value_or_default(), config->DlssNrWeightsSetA.value_or_default(),
                                config->DlssNrWeightsSetB.value_or_default()));
                Row("  Passes", NrPassesText(config->DlssNrPasses.value_or_default(),
                                             config->DlssNrWorkingScale.value_or_default()));
                Row("  Pass at", NrPlacementText(config->DlssNrFinishedPicture.value_or_default(),
                                                 config->DlssNrRunBeforeSr.value_or_default(),
                                                 config->DlssNrDeferredDlss.value_or_default()));
                // [DlssNr] MotionSource, which the Finished Picture reads (the line under Neural Rendering's placement
                // says whether it runs)
                if (config->DlssNrFinishedPicture.value_or_default())
                    Row("  Motion", config->DlssNrMotionSourceOptical.value_or_default() ? "Optical F5Low" : "Game");
                Row("  Detail reuse", NrDetailReuseText(config->DlssNrDetailReuse.value_or_default(),
                                                        config->DlssNrDetailReuseInterval.value_or_default()));
                Row("  F5LUT", config->DlssNrPostLutStackEnabled.value_or_default() ? "On" : "Off");
            }
            if (!summary.vulkanRoute.empty())
                Row("Vulkan route", summary.vulkanRoute);

            Row("Optical F5Low", OpticalF5LowText((NativeMode) summary.mode, summary.gameCallsUpscaler, nrUsable));

            // The FG page's words, which follow the generator f5low.dll was asked for
            if (state.activeFgOutput == FGOutput::F5Low)
            {
                const auto f5low = F5Low::QueryStatus();
                auto text = f5low.state == F5Low::RunState::Undecided && !f5low.reason.empty()
                                ? "starting: " + f5low.reason
                                : std::string(F5Low::RunStateLabel(f5low.state, F5Low::LatchedGenerator()));

                Row("F5Low FG", text);
            }

            // What the swap chain runs, not only the setting: it applies at the next swap chain and stands aside in
            // some games (the Image page has the same line)
            SdrToHdr::StatusSettings hdrSettings;
            hdrSettings.enabled = config->HdrOutputEnabled.value_or_default();
            hdrSettings.hdr10 = config->UseHDR10.value_or_default();
            hdrSettings.gameNits = config->HdrOutputGameNits.value_or_default();
            hdrSettings.configuredPeak = config->HdrOutputPeakNits.value_or_default();
            hdrSettings.nonDxgiApi = state.swapchainApi == Vulkan;

            const auto hdrLine = SdrToHdr::StatusLine(SdrToHdr::Status(), hdrSettings);
            Row("HDR output", HdrOutputText(hdrLine.colour != SdrToHdr::StatusColour::None, hdrLine.text));

            Row("Protect memory", ProtectMemoryText(config->ProtectMemory.value_or_default(), GpuMemory::ProtectOn()));

            ImGui::EndTable();
        }
    }
}
} // namespace

void StatusPage::Render(Config* config, IFeature* currentFeature, float menuResScale)
{
    const auto gpu = IdentifyGpu::getPrimaryGpu();
    const auto& style = ImGui::GetStyle();

    // Dense on purpose, the whole page is meant to fit in one screenshot
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, style.ItemSpacing.y * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(style.CellPadding.x, 1.0f * menuResScale));

    ImGui::TextDisabled("What OptiScaler found and runs here. A screenshot of this page helps with a bug report.");

    // This fork's card right after System, so the first screenshot has what its bug reports need most
    RenderSystem(config, currentFeature, gpu, menuResScale);
    RenderFork(config, menuResScale);
    RenderDetected(gpu, menuResScale);
    RenderHooks(config, menuResScale);
    RenderInitFlags(config, currentFeature, menuResScale);

    ImGui::PopStyleVar(2);
}
