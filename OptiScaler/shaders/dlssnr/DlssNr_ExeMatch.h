#pragma once

// Shared exe-name matching for the small per-game default lists in shaders/dlssnr/ (DlssNr_AutoTrimDefault.h,
// DlssNr_ReuseDepthDefault.h). Factored out after a matcher copied per list let one of them stay unlowercased
// and silently never match its own game (NBA2K27.exe, found in a Review Pass, 2026-10-01) -- with a second real
// caller, sharing the comparison itself closes that bug class rather than relying on each copy staying correct.
//
// Header-only and free of D3D/Vulkan types, like the lists that use it, so it stays host-testable.

#include <algorithm>
#include <cctype>
#include <string>

namespace DlssNrExeMatch
{
// The exe's own filename (not its full path), lower-cased, for case- and path-insensitive comparison.
inline std::string BaseNameLower(const std::string& exe)
{
    const size_t slash = exe.find_last_of("\\/");
    std::string name = slash == std::string::npos ? exe : exe.substr(slash + 1);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return name;
}

// Whether the game's exe (name or full path, any case) is on a known-games list.
template <size_t N>
inline bool IsKnownGame(const std::string& exe, const char* const (&list)[N])
{
    const std::string name = BaseNameLower(exe);
    for (const char* known : list)
        if (name == known)
            return true;
    return false;
}
} // namespace DlssNrExeMatch
