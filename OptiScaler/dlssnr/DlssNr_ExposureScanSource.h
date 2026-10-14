#pragma once

// Whose resource the exposure scan is looking at. Only the game's own resources can hold the game's exposure: what
// OptiScaler makes (Automatic exposure's own 1x1 textures, the first NBA 2K27 recording's "best mover") and what
// upscaler and frame generation middleware make are someone else's numbers. Decided by the module that called the
// create function, by file name.
//
// Header-only, no Windows types: tests/nr_exposure_scan_record_smoke.cpp.

#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace DlssNrExposureScanSource
{
// File-name prefixes of middleware that creates its own small UAV resources (lower case)
inline constexpr std::array<std::string_view, 9> kMiddleware = {
    "nvngx",          // DLSS, DLSS-G, DLSS-NR and NGX core
    "sl.",            // Streamline plugins
    "libxess",        // XeSS, XeSS FG
    "libxell",        // XeLL
    "amd_fidelityfx", // FidelityFX SDK
    "ffx_",           // FidelityFX 2/3 DLLs games ship
    "fsr",            // FSR 4 and older loose DLLs
    "dlss",           // dlss loader shims some games ship
    "nvlowlatency",   // Reflex
};

// `moduleName`: the calling module's file name (any case). `ours`: it is OptiScaler's own module.
inline bool FromGame(std::string_view moduleName, bool ours)
{
    if (ours)
        return false;

    std::string lower(moduleName);

    for (char& c : lower)
        c = (char) std::tolower((unsigned char) c);

    for (std::string_view prefix : kMiddleware)
    {
        if (lower.starts_with(prefix))
            return false;
    }

    return true;
}
} // namespace DlssNrExposureScanSource
