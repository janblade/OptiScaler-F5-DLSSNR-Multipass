#pragma once

#include <cwctype>
#include <string>
#include <string_view>

// Which driver-profile reads and NGX support checks OptiScaler answers differently for one caller. Pure, host-tested in
// tests/drs_caller_rule_smoke.cpp; NvApiHooks::hkNvAPI_DRS_GetSetting and NVSDK_NGX_D3D12_GetFeatureRequirements use
// it.
namespace drscaller
{
// Streamline's "use the NVIDIA app's override plugins" switch (the NVIDIA app's DLSS override sets it to 1). With it
// on, Streamline loads sl_*_override_0 plugins from ProgramData\NVIDIA\NGX\models instead of the files next to it.
constexpr unsigned kStreamlineOtaOverrideId = 0x10E41E06;

inline std::wstring Lower(std::wstring_view s)
{
    std::wstring out(s);
    for (auto& c : out)
        c = (wchar_t) std::towlower(c);
    return out;
}

// f5low's private Streamline (Frame Generator DLSS-G): unmodified NVIDIA files copied under other names into
// OptiScaler\plugins\f5low_streamline (f5low-fg generators/dlssg_files.h): xefg_slinterposer.dll and sl.xefg_*.dll.
inline bool IsF5LowStreamline(std::wstring_view fileName)
{
    const auto name = Lower(fileName);
    return name == L"xefg_slinterposer.dll" || name.rfind(L"sl.xefg_", 0) == 0;
}

// f5low's Streamline must load its own files: with the NVIDIA app override on it would load the same override plugins
// the game's Streamline already holds, and f5low's DLSS-G setup (its own nvngx_dlssg, unlocked on RTX 20/30/40) would
// run through NVIDIA's (Witcher 3, RTX 2070 SUPER, crash at launch, 2026-11-25). The game's Streamline keeps the value.
inline bool AnswerOtaOverrideOff(unsigned settingId, std::wstring_view callerFileName)
{
    return settingId == kStreamlineOtaOverrideId && IsF5LowStreamline(callerFileName);
}
} // namespace drscaller
