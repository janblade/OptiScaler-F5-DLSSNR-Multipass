// Host test for plugins/PluginsView.h: the text, tone and warnings of the Plugins page rows, built from what the loader
// recorded.
//
// Build (from the repo root): cl /nologo /std:c++20 /EHsc /W4 tests\plugins_view_smoke.cpp
#include <cstdio>
#include <string>

#include "../OptiScaler/plugins/PluginsView.h"

static int fails = 0;
static void Check(bool ok, const std::string& label)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", label.c_str());
        ++fails;
    }
}

using namespace Plugins;

static Entry Asi(const wchar_t* name, State state, uint32_t error = 0, const wchar_t* version = L"", int on = 1)
{
    Entry entry;
    entry.name = name;
    entry.kind = Kind::Asi;
    entry.version = version;
    entry.state = state;
    entry.error = error;
    entry.startedWith = entry.desired = on;
    return entry;
}

int main()
{
    // Every asi state has its own text and tone
    {
        const RowView loaded = AsiRow(Asi(L"a.asi", State::Loaded, 0, L"1.2.0.0"), 30);
        Check(loaded.state == "loaded" && loaded.tone == Tone::Green && loaded.checked, "loaded is green and ticked");
        Check(loaded.label == "a.asi v1.2.0.0", "the version follows the name");

        const RowView late = AsiRow(Asi(L"b-loadlate.asi", State::LoadedLate), 30);
        Check(late.state == "loaded late" && late.tone == Tone::Green, "loaded late is green");

        const RowView waiting = AsiRow(Asi(L"b-loadlate.asi", State::Waiting), 12);
        Check(waiting.state == "waiting, loads after 12 s" && waiting.tone == Tone::Grey, "waiting names the delay");

        const RowView disabled = AsiRow(Asi(L"c.asi", State::Disabled, 0, L"", 0), 30);
        Check(disabled.state == "disabled" && disabled.tone == Tone::Grey && !disabled.checked,
              "disabled is grey and unticked");

        const RowView skipped = AsiRow(Asi(L"d.asi", State::Skipped), 30);
        Check(skipped.state == "not loaded: ASI plugins are off" && skipped.tone == Tone::Grey, "skipped says why");

        const RowView failed = AsiRow(Asi(L"e.asi", State::Failed, 0xC1), 30);
        Check(failed.tone == Tone::Red && failed.state.find("failed to load (") == 0 &&
                  failed.state.find("0xC1") != std::string::npos,
              "failed is red and carries the Windows error");
    }

    // Pending comes only from desired against started-with
    {
        Entry entry = Asi(L"a.asi", State::Loaded);
        Check(!AsiRow(entry, 30).pending, "not pending at start");
        entry.desired = 0;
        Check(AsiRow(entry, 30).pending && !AsiRow(entry, 30).checked, "unticked after start is pending");
        entry.desired = 1;
        Check(!AsiRow(entry, 30).pending, "ticked again is not pending");
    }

    // Built-in rows
    {
        Entry entry;
        entry.name = L"XeFG";
        entry.kind = Kind::Builtin;
        entry.state = State::Off;
        entry.family = 0;
        MfgUnlock::FamilyStatus live;

        RowView row = BuiltinRow(entry, MfgUnlockMode::Off, live);
        Check(row.state == "off" && row.tone == Tone::Grey, "configured off reads off");

        row = BuiltinRow(entry, MfgUnlockMode::On, live);
        Check(row.state == "waiting for a provider" && row.tone == Tone::Grey, "on with nothing live waits");

        live.state = MfgUnlock::FamilyState::Active;
        live.patches = { { "a", "found" }, { "b", "found" } };
        row = BuiltinRow(entry, MfgUnlockMode::On, live);
        Check(row.state == "active: 2 of 2 patches" && row.tone == Tone::Green, "active is green");

        live.state = MfgUnlock::FamilyState::StoodAside;
        live.failure = "patch x not found";
        row = BuiltinRow(entry, MfgUnlockMode::Detect, live);
        Check(row.tone == Tone::Amber && row.state.find("stood aside") == 0, "stood aside is amber");

        entry.desired = 2;
        entry.startedWith = 1;
        Check(BuiltinRow(entry, MfgUnlockMode::On, live).pending, "a changed mode is pending");
    }

    // Loader rows
    {
        Entry entry;
        entry.name = kLoaderReShade;
        entry.kind = Kind::Loader;
        entry.state = State::Off;
        Check(LoaderRow(entry).state == "off" && LoaderRow(entry).tone == Tone::Grey, "loader off");
        entry.state = State::On;
        Check(LoaderRow(entry).state == "on, not loaded this session",
              "on with nothing recorded does not claim loaded");
        entry.state = State::Loaded;
        Check(LoaderRow(entry).state == "loaded" && LoaderRow(entry).tone == Tone::Green, "loader loaded");
        entry.state = State::Failed;
        entry.error = 2;
        Check(LoaderRow(entry).tone == Tone::Red && LoaderRow(entry).state.find("not found") != std::string::npos,
              "loader failed is red with the reason");
    }

    // The both-unlocks warning names no product: any loaded file with the word 'unlock', or the external FG switch
    {
        std::vector<Entry> entries = { Asi(L"x-Unlock.asi", State::Loaded) };
        Check(!BothUnlocksWarning(entries, false, false), "built-in off: no warning");
        Check(BothUnlocksWarning(entries, true, false), "built-in on and an unlock file loaded: warn");
        entries[0].state = State::Disabled;
        Check(!BothUnlocksWarning(entries, true, false), "the same file disabled: no warning");
        entries[0].state = State::Waiting;
        Check(BothUnlocksWarning(entries, true, false), "waiting counts: it will load");
        entries = { Asi(L"other.asi", State::Loaded) };
        Check(!BothUnlocksWarning(entries, true, false), "an unrelated file: no warning");
        Check(BothUnlocksWarning(entries, true, true), "built-in on and the external FG switch: warn");
        Check(!BothUnlocksWarning(entries, false, true), "built-in off and the external FG switch: no warning");
    }

    // Anything pending, the master switch included
    {
        std::vector<Entry> entries = { Asi(L"a.asi", State::Loaded) };
        Check(!AnyPending(entries, true, true), "nothing changed");
        Check(AnyPending(entries, false, true), "the master switch changed");
        entries[0].desired = 0;
        Check(AnyPending(entries, true, true), "a row changed");
    }

    // Text helpers
    {
        Check(ErrorText(0x7E).find("0x7E") != std::string::npos, "0x7E keeps its code");
        Check(ErrorText(0xC1).find("0xC1") != std::string::npos, "0xC1 keeps its code");
        Check(ErrorText(0x1234) == "0x1234", "an unknown code is just the code");
        Check(ToUtf8(L"a\x00E4.asi") == "a\xC3\xA4.asi", "wide text goes out as UTF-8");
        Check(ToUtf8(L"").empty(), "empty stays empty");
    }

    if (fails == 0)
        std::printf("plugins_view_smoke: all checks passed\n");
    return fails == 0 ? 0 : 1;
}
