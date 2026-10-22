#pragma once

#include <Windows.h>

#include <string_view>

namespace MfgUnlock
{
// True when any family is not off. With every family off nothing is hooked, tracked or patched.
bool AnyFamilyEnabled();

// True for libxess_fg.dll and for any nvngx_dlssg provider path (including the driver store .bin copies).
bool IsProviderModuleName(std::wstring_view name);

// The validated XeFG default-ceiling locator matches the stock `ebx = 3` ceiling.
inline constexpr int kXeFGOriginalDefaultMaxInterpolations = 3;

void ObserveProviderModule(HMODULE module);
bool PrepareXeFG();
void RefreshXeFGStatus();
bool IsXeFGActive();
bool IsTrackedProviderModule(HMODULE module);
bool BeforeModuleUnload(HMODULE module);
void AfterModuleUnload(HMODULE module);
void OnModuleUnloaded(HMODULE module);
} // namespace MfgUnlock
