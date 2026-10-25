#include "pch.h"
#include "PluginsPage.h"

#include "menu_common.h"
#include <Config.h>
#include <framegen/mfgunlock/Status.h>
#include <plugins/PluginsView.h>

namespace
{
const ImVec4 kGreen(0.45f, 0.85f, 0.45f, 1.0f);
const ImVec4 kAmber(1.0f, 0.85f, 0.3f, 1.0f);
const ImVec4 kRed(1.0f, 0.4f, 0.35f, 1.0f);

// The "(?)" marker every control carries, matching the rest of the menu.
void HelpMarker(const char* tip)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");

    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void StateText(const Plugins::RowView& row)
{
    ImGui::SameLine();

    switch (row.tone)
    {
    case Plugins::Tone::Green:
        ImGui::TextColored(kGreen, "%s", row.state.c_str());
        break;
    case Plugins::Tone::Amber:
        ImGui::TextColored(kAmber, "%s", row.state.c_str());
        break;
    case Plugins::Tone::Red:
        ImGui::TextColored(kRed, "%s", row.state.c_str());
        break;
    case Plugins::Tone::Grey:
        ImGui::TextDisabled("%s", row.state.c_str());
        break;
    default:
        ImGui::TextUnformatted(row.state.c_str());
        break;
    }

    if (row.pending)
    {
        ImGui::SameLine();
        ImGui::TextColored(kAmber, "restart to apply");
    }
}

int ModeIndex(MfgUnlockMode mode) { return mode == MfgUnlockMode::Off ? 0 : (mode == MfgUnlockMode::Detect ? 1 : 2); }

MfgUnlockMode ModeFromIndex(int index)
{
    return index == 0 ? MfgUnlockMode::Off : (index == 1 ? MfgUnlockMode::Detect : MfgUnlockMode::On);
}

void RenderBuiltin(Config* config, const std::vector<Plugins::Entry>& entries, bool& anyBuiltinOn)
{
    CustomOptional<MfgUnlockMode>* modes[] = { &config->MfgUnlockXeFG, &config->MfgUnlockRTX40,
                                               &config->MfgUnlockRTX2030 };
    static constexpr const char* kModeNames[] = { "Off", "Detect", "On" };

    for (const auto& entry : entries)
    {
        if (entry.kind != Plugins::Kind::Builtin || entry.family < 0 || entry.family > 2)
            continue;

        auto& option = *modes[entry.family];
        MfgUnlockMode mode = option.value_or_default();
        anyBuiltinOn = anyBuiltinOn || mode != MfgUnlockMode::Off;

        const auto live = MfgUnlock::StatusRegistry::Get(static_cast<MfgUnlock::Family>(entry.family));
        // The state text describes this session, so it follows the mode the process started with; the combo holds
        // the next start's value and "restart to apply" shows the difference
        const Plugins::RowView row = Plugins::BuiltinRow(entry, static_cast<MfgUnlockMode>(entry.startedWith), live);

        ImGui::PushID(entry.family);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("MFG unlock: %s", row.label.c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);

        int index = ModeIndex(mode);
        if (ImGui::Combo("##mode", &index, kModeNames, 3))
        {
            mode = ModeFromIndex(index);
            option = mode;
            Plugins::Registry::Global().SetDesired(Plugins::Kind::Builtin, entry.name, static_cast<int>(mode));
        }

        StateText(row);
        ImGui::PopID();
    }

    ImGui::TextDisabled("Off by default. Built in, no file needed.");
    HelpMarker("Off: never patches anything.\n"
               "Detect: only when this GPU needs the unlock.\n"
               "On: always.\n\n"
               "Replaces an external unlocker plugin.\n"
               "Save Settings and restart to apply. ini: [MFGUnlock]");
}

void RenderAsiFiles(Config* config, const std::vector<Plugins::Entry>& entries, bool masterNow)
{
    ImGui::BeginDisabled(!masterNow);

    size_t shown = 0;
    for (const auto& entry : entries)
    {
        if (entry.kind != Plugins::Kind::Asi)
            continue;

        const Plugins::RowView row = Plugins::AsiRow(entry, config->LateAsiPluginsDelay.value_or_default());

        ImGui::PushID(static_cast<int>(shown));
        bool on = row.checked;
        if (ImGui::Checkbox("##on", &on))
        {
            auto list = Plugins::ParseDisabledList(config->DisabledAsi.value_or(L""));
            Plugins::SetDisabled(list, entry.name, !on);

            // Back to unset (the ini says auto) when nothing is left
            if (list.empty())
                config->DisabledAsi = std::optional<std::wstring> {};
            else
                config->DisabledAsi = Plugins::JoinDisabledList(list);

            Plugins::Registry::Global().SetDesired(Plugins::Kind::Asi, entry.name, on ? 1 : 0);
        }

        ImGui::SameLine();
        ImGui::TextUnformatted(row.label.c_str());
        StateText(row);
        ImGui::PopID();
        ++shown;
    }

    if (shown == 0)
        ImGui::TextDisabled("No *.asi files in the folder");

    ImGui::EndDisabled();

    if (!masterNow)
        ImGui::TextDisabled("ASI plugins are off");
}

void RenderLoaders(Config* config, const std::vector<Plugins::Entry>& entries)
{
    struct Switch
    {
        const wchar_t* name;
        CustomOptional<bool>* option;
        const char* help;
    };

    const Switch switches[] = {
        { Plugins::kLoaderSpecialK, &config->LoadSpecialK,
          "Loads SpecialK64.dll from the game folder. Not loaded while the OptiFG, DLSSG via Streamline or FSR-FG "
          "inputs are on.\nSave Settings and restart to apply. ini: [Plugins] LoadSpecialK" },
        { Plugins::kLoaderReShade, &config->LoadReShade,
          "Loads ReShade next to OptiScaler.\nSave Settings and restart to apply. ini: [Plugins] LoadReShade" },
        { Plugins::kLoaderAmdxc64, &config->LoadCustomAmdxc64OnRdna2,
          "Uses the bundled amdxc64.dll on RDNA 2 GPUs only.\nSave Settings and restart to apply. ini: [Plugins] "
          "LoadCustomAmdxc64OnRdna2" },
    };

    for (size_t index = 0; index < std::size(switches); ++index)
    {
        const Plugins::Entry* found = nullptr;
        for (const auto& entry : entries)
        {
            if (entry.kind == Plugins::Kind::Loader && Plugins::EqualsNoCase(entry.name, switches[index].name))
                found = &entry;
        }
        if (found == nullptr)
            continue;

        const Plugins::RowView row = Plugins::LoaderRow(*found);

        ImGui::PushID(static_cast<int>(index));
        bool on = row.checked; // what the next start will do, not the session's volatile value
        if (ImGui::Checkbox(row.label.c_str(), &on))
        {
            *switches[index].option = on;
            Plugins::Registry::Global().SetDesired(Plugins::Kind::Loader, found->name, on ? 1 : 0);
        }
        HelpMarker(switches[index].help);
        StateText(row);
        ImGui::PopID();
    }
}
} // namespace

void PluginsPage::Render(Config* config)
{
    auto& registry = Plugins::Registry::Global();
    const auto entries = registry.Snapshot(); // drawn from a copy: the late-load thread may write at any time
    const auto session = registry.GetSession();

    // Master switch and folder
    bool master = config->LoadAsiPlugins.value_or_default();
    if (ImGui::Checkbox("Load ASI plugins", &master))
        config->LoadAsiPlugins = master;
    HelpMarker("Lets OptiScaler load *.asi files from the plugins folder. When off, the list below still shows what "
               "would load.\nSave Settings and restart to apply. ini: [Plugins] LoadAsiPlugins");

    if (master != session.asiEnabled)
    {
        ImGui::SameLine();
        ImGui::TextColored(kAmber, "restart to apply");
    }

    const std::string folder = Plugins::ToUtf8(session.folder);
    ImGui::TextDisabled("Folder: %s%s", folder.empty() ? "(not read yet)" : folder.c_str(),
                        !folder.empty() && !session.folderFound ? " (not found)" : "");

    // Built-in
    bool anyBuiltinOn = false;
    if (auto ch = ScopedCollapsingHeader("Built-in", ImGuiTreeNodeFlags_DefaultOpen); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        RenderBuiltin(config, entries, anyBuiltinOn);
    }

    if (Plugins::BothUnlocksWarning(entries, anyBuiltinOn, config->ExternalFrameGeneration.value_or_default()))
    {
        ImGui::TextColored(kAmber, "The built-in MFG unlock and an external unlocker would both patch the same "
                                   "provider. Turn one of them off.");
    }

    // Files in the plugins folder
    if (auto ch = ScopedCollapsingHeader("Loaded from plugins folder", ImGuiTreeNodeFlags_DefaultOpen);
        ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::TextDisabled("Unticked files are not loaded at the next start.");
        HelpMarker("The file itself is not changed. Tick it again to load it.\nini: [Plugins] DisabledAsi");
        RenderAsiFiles(config, entries, master);
    }

    // Optional loaders
    if (auto ch = ScopedCollapsingHeader("Loaders", ImGuiTreeNodeFlags_DefaultOpen); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        RenderLoaders(config, entries);
    }

    if (Plugins::AnyPending(entries, master, session.asiEnabled))
    {
        ImGui::Spacing();
        ImGui::TextColored(kAmber, "Save Settings and restart to apply the changes above.");
    }
}
