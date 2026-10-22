// Host test for plugins/PluginRegistry.h: the plugin list the Plugins page reads, the [Plugins] DisabledAsi list, and
// the built-in MFG unlock's default (off).
//
// Build (from the repo root): cl /nologo /std:c++20 /EHsc /W4 tests\plugin_registry_smoke.cpp
// Run: plugin_registry_smoke.exe [path to OptiScaler.ini, default OptiScaler.ini]
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "../OptiScaler/plugins/PluginRegistry.h"
#include "../OptiScaler/framegen/mfgunlock/Mode.h"

static int fails = 0;
static void Check(bool ok, const std::string& label)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", label.c_str());
        ++fails;
    }
}

static std::string Lower(std::string text)
{
    for (auto& c : text)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return text;
}

static std::string Trim(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// The value of [section] key in an ini text file; nullopt when the section or the key is missing.
static std::optional<std::string> IniValue(const std::string& path, const std::string& section, const std::string& key)
{
    std::ifstream file(path);
    std::string line;
    bool inSection = false;

    while (std::getline(file, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line[0] == '[')
        {
            inSection = Lower(line) == "[" + Lower(section) + "]";
            continue;
        }
        const auto equals = line.find('=');
        if (inSection && equals != std::string::npos && Lower(Trim(line.substr(0, equals))) == Lower(key))
            return Trim(line.substr(equals + 1));
    }

    return std::nullopt;
}

static MfgUnlockMode ModeFromText(const std::string& text)
{
    bool valid = true;
    return MfgUnlock::EffectiveMode(MfgUnlock::ParseMode(text, valid));
}

int main(int argc, char** argv)
{
    using namespace Plugins;

    // (a) add, update, find
    {
        Registry reg;
        reg.Upsert({ .name = L"a.asi", .kind = Kind::Asi, .state = State::Waiting });
        reg.Upsert({ .name = L"b.asi", .kind = Kind::Asi, .state = State::Waiting });
        Check(reg.SetState(Kind::Asi, L"A.ASI", State::Loaded, 0, L"1.2.0.0"), "SetState finds a name in any case");
        Check(!reg.SetState(Kind::Asi, L"missing.asi", State::Loaded), "SetState on an unknown name is false");
        reg.Upsert({ .name = L"a.asi", .kind = Kind::Asi, .state = State::Failed, .error = 0x7E });
        const auto snapshot = reg.Snapshot();
        Check(snapshot.size() == 2, "re-adding the same kind and name updates, it does not add");
        Check(snapshot[0].state == State::Failed && snapshot[0].error == 0x7E, "re-add replaced the entry");
        Check(snapshot[1].name == L"b.asi" && snapshot[1].state == State::Waiting, "other entries are unchanged");
        reg.Upsert({ .name = L"a.asi", .kind = Kind::Loader, .state = State::Off });
        Check(reg.Snapshot().size() == 3, "the same name under another kind is its own entry");
    }

    // (b) pending
    {
        Registry reg;
        reg.Upsert({ .name = L"x", .kind = Kind::Loader, .state = State::Off, .startedWith = 0, .desired = 0 });
        Check(!reg.Snapshot()[0].Pending(), "not pending at start");
        reg.SetDesired(Kind::Loader, L"x", 1);
        Check(reg.Snapshot()[0].Pending(), "pending once desired differs from started-with");
        reg.SetDesired(Kind::Loader, L"x", 0);
        Check(!reg.Snapshot()[0].Pending(), "flipping back clears pending");
    }

    // (c) parse
    {
        const auto list = ParseDisabledList(L"a.asi, B.ASI ,,c.asi");
        Check(list.size() == 3 && list[0] == L"a.asi" && list[1] == L"B.ASI" && list[2] == L"c.asi",
              "parse trims, drops empty items, keeps the written case");
        Check(ParseDisabledList(L"").empty(), "empty text is no list");
        Check(ParseDisabledList(L"\t a.asi \t,\ta.asi").size() == 2, "tabs are trimmed, duplicates kept");
    }

    // (d) round trip
    {
        const auto list = ParseDisabledList(L"a.asi, B.ASI ,,c.asi");
        Check(JoinDisabledList(list) == L"a.asi,B.ASI,c.asi", "join writes comma separated, no spaces");
        Check(ParseDisabledList(JoinDisabledList(list)) == list, "parse(join(x)) == x");
        Check(JoinDisabledList({}).empty(), "empty list joins to nothing");
    }

    // (e) matching
    {
        const auto list = ParseDisabledList(L"a.asi,B.ASI,\x00C4.asi");
        Check(IsDisabled(list, L"b.asi"), "match ignores case");
        Check(!IsDisabled(list, L"x\\b.asi"), "a path never matches a file name entry");
        Check(!IsDisabled(list, L"b.asi.off"), "the whole name must match");
        Check(IsDisabled(list, L"\x00E4.asi"), "non-ASCII letters match across case");
        Check(!IsDisabled({}, L"a.asi"), "an empty list disables nothing");
    }

    // (f) edit
    {
        auto list = ParseDisabledList(L"a.asi,A.ASI,b.asi");
        SetDisabled(list, L"a.ASI", false);
        Check(list.size() == 1 && list[0] == L"b.asi", "enabling removes every case-insensitive duplicate");
        SetDisabled(list, L"c.asi", true);
        SetDisabled(list, L"C.asi", true);
        Check(list.size() == 2 && IsDisabled(list, L"c.asi"), "disabling adds a name once");
    }

    // (g) the built-in MFG unlock defaults to off
    {
        Check(MfgUnlock::kDefaultMode == MfgUnlockMode::Off, "the default mode is off");
        Check(ModeFromText("") == MfgUnlockMode::Off, "an empty value is off");
        Check(ModeFromText("auto") == MfgUnlockMode::Off, "auto is off");
        Check(ModeFromText("AUTO") == MfgUnlockMode::Off, "AUTO is off");
        Check(ModeFromText("yes") == MfgUnlockMode::Off, "text that is not a mode is off");
        Check(ModeFromText("detect") == MfgUnlockMode::Detect, "detect stays detect");
        Check(ModeFromText("true") == MfgUnlockMode::On, "true stays on");
    }

    // (h) the shipped ini
    {
        const std::string path = argc > 1 ? argv[1] : "OptiScaler.ini";
        Check(std::ifstream(path).good(), "the ini can be read: " + path);

        for (const char* key : { "XeFG", "RTX40", "RTX2030" })
        {
            const auto value = IniValue(path, "MFGUnlock", key);
            Check(ModeFromText(value.value_or("auto")) == MfgUnlockMode::Off,
                  std::string("shipped ini: [MFGUnlock] ") + key + " is off");
        }

        const auto disabled = IniValue(path, "Plugins", "DisabledAsi");
        Check(disabled.has_value(), "shipped ini: [Plugins] has a DisabledAsi key");
        const std::string disabledText = Lower(disabled.value_or("x"));
        Check(disabledText.empty() || disabledText == "auto", "shipped ini: DisabledAsi lists nothing (auto)");
    }

    // (k) the session info round trips
    {
        Registry reg;
        Check(!reg.GetSession().asiEnabled && reg.GetSession().folder.empty() && !reg.GetSession().folderFound,
              "a new registry has an empty session");
        reg.SetSession({ .asiEnabled = true, .folder = L"x\\plugins", .folderFound = true });
        const auto session = reg.GetSession();
        Check(session.asiEnabled && session.folder == L"x\\plugins" && session.folderFound, "session round trip");
    }

    // (i) built-in MFG entries need no file
    {
        Registry reg;
        RegisterBuiltinMfg(reg, { MfgUnlockMode::Off, MfgUnlockMode::Off, MfgUnlockMode::Off });
        const auto snapshot = reg.Snapshot();
        Check(snapshot.size() == 3, "three built-in MFG families");
        for (const auto& entry : snapshot)
        {
            Check(entry.kind == Kind::Builtin, "MFG entry is built-in");
            Check(entry.path.empty(), "MFG entry has no file");
            Check(entry.state == State::Off, "MFG entry starts off");
        }
        Check(snapshot[0].family == 0 && snapshot[2].family == 2, "family index is kept");

        Registry on;
        RegisterBuiltinMfg(on, { MfgUnlockMode::On, MfgUnlockMode::Detect, MfgUnlockMode::Off });
        const auto onSnapshot = on.Snapshot();
        Check(onSnapshot[0].state == State::On && onSnapshot[1].state == State::On && onSnapshot[2].state == State::Off,
              "on and detect read as on, off as off");
        Check(onSnapshot[0].startedWith == static_cast<int>(MfgUnlockMode::On), "started-with is the mode");
    }

    // (j) the log summary
    {
        std::vector<Entry> entries;
        entries.push_back({ .name = L"a.asi", .kind = Kind::Asi, .version = L"1.2.0.0", .state = State::Loaded });
        entries.push_back({ .name = L"b.asi", .kind = Kind::Asi, .state = State::Disabled });
        entries.push_back({ .name = L"c.asi", .kind = Kind::Asi, .state = State::Failed, .error = 0x7E });
        entries.push_back({ .name = L"d-loadlate.asi", .kind = Kind::Asi, .state = State::Waiting });
        entries.push_back({ .name = L"XeFG", .kind = Kind::Builtin, .state = State::Off });
        Check(FormatSummary(entries) == L"[Plugins] 4 asi: a.asi loaded v1.2.0.0; b.asi disabled; c.asi failed 0x7E; "
                                        L"d-loadlate.asi waiting",
              "summary line");
        Check(FormatSummary({}) == L"[Plugins] 0 asi", "summary with no plugin");
    }

    if (fails == 0)
        std::printf("plugin_registry_smoke: all checks passed\n");
    return fails == 0 ? 0 : 1;
}
