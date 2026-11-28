#pragma once

#include <filesystem>
#include <system_error>

// Where a relative path from OptiScaler.ini points. The base is OptiScaler's own folder (the folder with the proxy
// dll). A path that was written against the exe folder (OptiScaler\dlss31016 in a game whose proxy dll sits in a
// different folder than the exe) still works: when nothing is at the Opti-folder spelling but something is at the
// exe-folder one, that one is used. Absolute paths (drive or root) are returned as they are. Pure std, so it is unit
// tested on the host.
namespace RelativePathRule
{
inline std::filesystem::path Resolve(const std::filesystem::path& value, const std::filesystem::path& optiFolder,
                                     const std::filesystem::path& exeFolder)
{
    if (value.empty() || !value.is_relative())
        return value;

    std::error_code ec;
    const std::filesystem::path first = optiFolder / value;

    if (std::filesystem::exists(first, ec))
        return first;

    if (!exeFolder.empty() && exeFolder != optiFolder)
    {
        const std::filesystem::path second = exeFolder / value;

        if (std::filesystem::exists(second, ec))
            return second;
    }

    // Nothing there (yet): the documented base, so the log and the menu name a place that makes sense
    return first;
}
} // namespace RelativePathRule
