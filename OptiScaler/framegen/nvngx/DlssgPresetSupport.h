#pragma once

#include <misc/CustomProviders.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// Which DLSS-G presets ([DLSSG] Preset) a custom DLSS-G replacement (custom\<name>\dlssg) can take. NVIDIA's
// nvngx_dlssg reads the driver setting itself; Streamline's sl.dlss_g also reads it and passes the preset on as create
// parameters: B = DLSSG.UserInterfaceRecompositionEnabled, C = DLSSG.LiteCandidateBlendingEnabled (A = neither, the
// default model). A replacement takes a preset only if its dll knows that parameter (or reads the driver setting
// itself), so its file is scanned once for them. A plugin-registered provider has no file to scan.
namespace DlssgPresetSupport
{
struct Support
{
    bool known = false; // false: no file to scan (plugin-registered or missing), presets are passed on untested
    bool readsDriverSetting = false;
    bool uiRecomposition = false; // preset B
    bool lite = false;            // preset C
};

inline Support Scan(const std::filesystem::path& path)
{
    Support support;

    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(path, ec))
        return support;

    std::ifstream file(path, std::ios::binary);
    if (!file)
        return support;

    const std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    auto contains = [&data](const std::string& needle)
    { return std::search(data.begin(), data.end(), needle.begin(), needle.end()) != data.end(); };

    // The driver setting id (0x10E41DF1) as a little-endian constant, the way nvngx_dlssg reads it
    static const std::string drsId("\xF1\x1D\xE4\x10", 4);

    support.known = true;
    support.readsDriverSetting = contains(drsId);
    support.uiRecomposition = contains("DLSSG.UserInterfaceRecompositionEnabled");
    support.lite = contains("DLSSG.LiteCandidateBlendingEnabled");
    return support;
}

// Cached per provider name (a provider's dll doesn't change while the game runs)
inline Support ForCustom(const std::string& name)
{
    static std::mutex mutex;
    static std::map<std::string, Support> cache;

    std::scoped_lock lock(mutex);

    if (auto it = cache.find(name); it != cache.end())
        return it->second;

    auto support = Scan(CustomProviders::DllPath(name, CustomProviders::Kind::Dlssg));
    cache[name] = support;
    return support;
}

// preset: 1 = A, 2 = B, 3 = C. Unknown providers are given the preset (nothing to check against).
inline bool Takes(const Support& support, int preset)
{
    if (!support.known || support.readsDriverSetting || preset <= 1)
        return true;

    return preset == 2 ? support.uiRecomposition : support.lite;
}
} // namespace DlssgPresetSupport
