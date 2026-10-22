#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "../framegen/mfgunlock/Mode.h"
#include "../framegen/mfgunlock/Status.h"

// What the Plugins page lists: the *.asi files the loader saw, the built-in MFG unlock, and the optional loaders. Pure
// data, no ImGui and no Config, so it can be unit tested on the host (tests/plugin_registry_smoke.cpp). The loader
// records what it did; the menu only reads.
namespace Plugins
{
enum class Kind
{
    Asi,
    Builtin,
    Loader
};

enum class State
{
    Waiting, // a -loadlate file, before its delay is over
    Loaded,
    LoadedLate,
    Disabled, // named in [Plugins] DisabledAsi: never opened
    Failed,   // LoadLibrary said no, see Entry::error
    Skipped,  // [Plugins] LoadAsiPlugins is off: seen, not opened
    Off,
    On
};

struct Entry
{
    std::wstring name; // file name for an asi, display name otherwise
    Kind kind = Kind::Asi;
    std::wstring path;    // empty for the built-in entries: they need no file
    std::wstring version; // "1.2.3.4", only for files that were opened
    State state = State::Waiting;
    uint32_t error = 0;
    int startedWith = 0; // what the running process started with, so a changed row can say "restart to apply"
    int desired = 0;     // what the menu wants now
    int family = -1;     // MfgUnlock::Family for the built-in MFG entries

    bool Pending() const { return desired != startedWith; }
};

inline bool EqualsNoCase(std::wstring_view a, std::wstring_view b)
{
    if (a.size() != b.size())
        return false;
    if (a.empty())
        return true;
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

// "a.asi, B.ASI ,,c.asi" -> {a.asi, B.ASI, c.asi}: trimmed, empty items dropped, written case kept.
inline std::vector<std::wstring> ParseDisabledList(std::wstring_view text)
{
    std::vector<std::wstring> list;
    size_t start = 0;

    while (start <= text.size())
    {
        size_t end = text.find(L',', start);
        if (end == std::wstring_view::npos)
            end = text.size();

        std::wstring_view item = text.substr(start, end - start);
        while (!item.empty() && (item.front() == L' ' || item.front() == L'\t'))
            item.remove_prefix(1);
        while (!item.empty() && (item.back() == L' ' || item.back() == L'\t'))
            item.remove_suffix(1);

        if (!item.empty())
            list.emplace_back(item);

        start = end + 1;
    }

    return list;
}

inline std::wstring JoinDisabledList(const std::vector<std::wstring>& list)
{
    std::wstring text;
    for (const auto& item : list)
    {
        if (!text.empty())
            text += L',';
        text += item;
    }
    return text;
}

// Whole file name, any case. The list is only ever compared, never used as a path.
inline bool IsDisabled(const std::vector<std::wstring>& list, std::wstring_view fileName)
{
    for (const auto& item : list)
    {
        if (EqualsNoCase(item, fileName))
            return true;
    }
    return false;
}

inline void SetDisabled(std::vector<std::wstring>& list, std::wstring_view fileName, bool disabled)
{
    for (auto it = list.begin(); it != list.end();)
    {
        if (EqualsNoCase(*it, fileName))
            it = list.erase(it);
        else
            ++it;
    }

    if (disabled)
        list.emplace_back(fileName);
}

class Registry
{
  public:
    // Same kind and name (any case) replaces the entry.
    void Upsert(Entry entry)
    {
        std::lock_guard lock(_mutex);
        for (auto& existing : _entries)
        {
            if (existing.kind == entry.kind && EqualsNoCase(existing.name, entry.name))
            {
                existing = std::move(entry);
                return;
            }
        }
        _entries.push_back(std::move(entry));
    }

    // False when no such entry. The late-load thread calls only this.
    bool SetState(Kind kind, std::wstring_view name, State state, uint32_t error = 0, std::wstring version = {})
    {
        std::lock_guard lock(_mutex);
        for (auto& entry : _entries)
        {
            if (entry.kind == kind && EqualsNoCase(entry.name, name))
            {
                entry.state = state;
                entry.error = error;
                if (!version.empty())
                    entry.version = std::move(version);
                return true;
            }
        }
        return false;
    }

    void SetDesired(Kind kind, std::wstring_view name, int desired)
    {
        std::lock_guard lock(_mutex);
        for (auto& entry : _entries)
        {
            if (entry.kind == kind && EqualsNoCase(entry.name, name))
                entry.desired = desired;
        }
    }

    // A copy: the menu draws from it without holding the lock.
    std::vector<Entry> Snapshot() const
    {
        std::lock_guard lock(_mutex);
        return _entries;
    }

    static Registry& Global()
    {
        static Registry registry;
        return registry;
    }

  private:
    mutable std::mutex _mutex;
    std::vector<Entry> _entries;
};

inline std::wstring Widen(std::string_view text) { return std::wstring(text.begin(), text.end()); }

// The three families are listed whether or not any provider file is around. Detect reads as on: the family is armed,
// the GPU check decides later. The menu reads the live family status from MfgUnlock::StatusRegistry when it draws.
inline void RegisterBuiltinMfg(Registry& registry, const std::array<MfgUnlockMode, 3>& modes)
{
    for (size_t index = 0; index < modes.size(); ++index)
    {
        Entry entry;
        entry.name = Widen(MfgUnlock::kFamilyNames[index]);
        entry.kind = Kind::Builtin;
        entry.state = modes[index] == MfgUnlockMode::Off ? State::Off : State::On;
        entry.startedWith = static_cast<int>(modes[index]);
        entry.desired = entry.startedWith;
        entry.family = static_cast<int>(index);
        registry.Upsert(std::move(entry));
    }
}

inline void RegisterLoaders(Registry& registry, bool specialK, bool reShade, bool amdxc64Rdna2)
{
    const std::array<std::pair<const wchar_t*, bool>, 3> loaders = { {
        { L"Special K", specialK },
        { L"ReShade", reShade },
        { L"Custom amdxc64 on RDNA2", amdxc64Rdna2 },
    } };

    for (const auto& [name, on] : loaders)
    {
        Entry entry;
        entry.name = name;
        entry.kind = Kind::Loader;
        entry.state = on ? State::On : State::Off;
        entry.startedWith = on ? 1 : 0;
        entry.desired = entry.startedWith;
        registry.Upsert(std::move(entry));
    }
}

inline const wchar_t* StateText(State state)
{
    switch (state)
    {
    case State::Waiting:
        return L"waiting";
    case State::Loaded:
        return L"loaded";
    case State::LoadedLate:
        return L"loaded late";
    case State::Disabled:
        return L"disabled";
    case State::Failed:
        return L"failed";
    case State::Skipped:
        return L"skipped";
    case State::Off:
        return L"off";
    case State::On:
        return L"on";
    }
    return L"";
}

// One log line for support: "[Plugins] 4 asi: a.asi loaded v1.2.0.0; b.asi disabled; c.asi failed 0x7E; ..."
inline std::wstring FormatSummary(const std::vector<Entry>& entries)
{
    size_t count = 0;
    std::wstring items;

    for (const auto& entry : entries)
    {
        if (entry.kind != Kind::Asi)
            continue;

        ++count;
        items += count == 1 ? L" " : L"; ";
        items += std::format(L"{} {}", entry.name, StateText(entry.state));
        if (entry.state == State::Failed)
            items += std::format(L" 0x{:X}", entry.error);
        else if (!entry.version.empty())
            items += std::format(L" v{}", entry.version);
    }

    return std::format(L"[Plugins] {} asi{}", count, count == 0 ? L"" : L":" + items);
}
} // namespace Plugins
