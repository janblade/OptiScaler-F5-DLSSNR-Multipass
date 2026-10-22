#pragma once

#include <cstdint>
#include <cwctype>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "PluginRegistry.h"

// What each row of the Plugins page says and in which tone, built only from what the loader recorded: a row never
// claims "loaded" without a recorded Loaded. No ImGui and no Config, so tests/plugins_view_smoke.cpp covers it.
namespace Plugins
{
enum class Tone
{
    Normal,
    Grey,
    Green,
    Amber,
    Red
};

struct RowView
{
    std::string label;
    std::string state;
    Tone tone = Tone::Normal;
    bool checked = false;
    bool pending = false; // the value differs from what this session started with: restart to apply
};

// ImGui takes UTF-8; file names are wide.
inline std::string ToUtf8(std::wstring_view text)
{
    if (text.empty())
        return {};

    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};

    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

// The Windows error LoadLibrary left, in words where the cause is well known.
inline std::string ErrorText(uint32_t error)
{
    const char* reason = nullptr;
    switch (error)
    {
    case 2:
    case 3:
        reason = "file or path not found";
        break;
    case 5:
        reason = "access denied";
        break;
    case 0x7E:
        reason = "module not found";
        break;
    case 0x7F:
        reason = "procedure not found";
        break;
    case 0xC1:
        reason = "not a valid 64-bit DLL";
        break;
    case 0x45A:
        reason = "DLL initialisation failed";
        break;
    default:
        break;
    }

    return reason != nullptr ? std::format("0x{:X}, {}", error, reason) : std::format("0x{:X}", error);
}

// "failed to load (0x7E, module not found)"; without a code it is only "failed to load"
inline std::string FailedText(uint32_t error)
{
    return error == 0 ? std::string("failed to load") : "failed to load (" + ErrorText(error) + ")";
}

inline RowView AsiRow(const Entry& entry, int lateDelaySeconds)
{
    RowView row;
    row.label = ToUtf8(entry.name);
    if (!entry.version.empty() && entry.state != State::Disabled && entry.state != State::Skipped)
        row.label += " v" + ToUtf8(entry.version);
    row.checked = entry.desired == 1;
    row.pending = entry.Pending();

    switch (entry.state)
    {
    case State::Loaded:
        row.state = "loaded";
        row.tone = Tone::Green;
        break;
    case State::LoadedLate:
        row.state = "loaded late";
        row.tone = Tone::Green;
        break;
    case State::Waiting:
        row.state = std::format("waiting, loads after {} s", lateDelaySeconds);
        row.tone = Tone::Grey;
        break;
    case State::Disabled:
        row.state = "disabled";
        row.tone = Tone::Grey;
        break;
    case State::Skipped:
        row.state = "not loaded: ASI plugins are off";
        row.tone = Tone::Grey;
        break;
    case State::Failed:
        row.state = FailedText(entry.error);
        row.tone = Tone::Red;
        break;
    default:
        row.state = StateText(entry.state) != nullptr ? ToUtf8(StateText(entry.state)) : std::string();
        row.tone = Tone::Grey;
        break;
    }

    return row;
}

// configured: what the menu holds now; live: what the family actually did, read when drawing.
inline RowView BuiltinRow(const Entry& entry, MfgUnlockMode configured, const MfgUnlock::FamilyStatus& live)
{
    RowView row;
    row.label = ToUtf8(entry.name);
    row.pending = entry.Pending();

    if (configured == MfgUnlockMode::Off)
    {
        row.state = "off";
        row.tone = Tone::Grey;
    }
    else if (live.state == MfgUnlock::FamilyState::Off)
    {
        row.state = "waiting for a provider";
        row.tone = Tone::Grey;
    }
    else
    {
        row.state = MfgUnlock::FormatStatus(live);
        row.tone = live.state == MfgUnlock::FamilyState::Active       ? Tone::Green
                   : live.state == MfgUnlock::FamilyState::StoodAside ? Tone::Amber
                                                                      : Tone::Grey;
    }

    return row;
}

inline RowView LoaderRow(const Entry& entry)
{
    RowView row;
    row.label = ToUtf8(entry.name);
    row.checked = entry.desired == 1;
    row.pending = entry.Pending();

    switch (entry.state)
    {
    case State::Loaded:
        row.state = "loaded";
        row.tone = Tone::Green;
        break;
    case State::Failed:
        row.state = FailedText(entry.error);
        row.tone = Tone::Red;
        break;
    case State::Blocked:
        row.state = "on, not loaded: a frame generation input is on";
        row.tone = Tone::Amber;
        break;
    case State::On:
        row.state = "on, not loaded this session";
        row.tone = Tone::Grey;
        break;
    default:
        row.state = "off";
        row.tone = Tone::Grey;
        break;
    }

    return row;
}

// Generic word, never a product name: an asi whose file name says it is an unlocker.
inline constexpr std::wstring_view kUnlockWord = L"unlock";

inline bool NameHasWord(std::wstring_view name, std::wstring_view word)
{
    for (size_t start = 0; start + word.size() <= name.size(); ++start)
    {
        if (EqualsNoCase(name.substr(start, word.size()), word))
            return true;
    }
    return false;
}

// The built-in unlock is on while something else that unlocks frame generation is set up too: the "External frame
// generation / MFG unlocker" switch, or an asi that will load (or has) with 'unlock' in its name. The page does not
// choose a winner.
inline bool BothUnlocksWarning(const std::vector<Entry>& entries, bool anyBuiltinConfiguredOn, bool externalFgSwitch)
{
    if (!anyBuiltinConfiguredOn)
        return false;
    if (externalFgSwitch)
        return true;

    for (const auto& entry : entries)
    {
        if (entry.kind != Kind::Asi)
            continue;
        if (entry.state != State::Loaded && entry.state != State::LoadedLate && entry.state != State::Waiting)
            continue;
        if (NameHasWord(entry.name, kUnlockWord))
            return true;
    }

    return false;
}

inline bool AnyPending(const std::vector<Entry>& entries, bool masterNow, bool masterStarted)
{
    if (masterNow != masterStarted)
        return true;

    for (const auto& entry : entries)
    {
        if (entry.Pending())
            return true;
    }

    return false;
}
} // namespace Plugins
