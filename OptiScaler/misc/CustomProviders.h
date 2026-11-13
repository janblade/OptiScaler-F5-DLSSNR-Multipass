#pragma once

// Custom providers: dlls in OptiScaler's custom\<name>\<kind> folder that take the place of a built-in upscaler, frame
// generation or ray reconstruction, picked by <name> in the ini or the menu. Layout and kinds as upstream OptiScaler's
// CustomProviders (optiscaler/OptiScaler 941ce6a1, de2626f1, c2a40331, 74ae2c65), adapted here:
//   custom\<name>\ffx        DX12 FFX upscaler (also DX11 through DX12)
//   custom\<name>\ffx_vk     Vulkan FFX upscaler
//   custom\<name>\ffx_fg     DX12 FFX frame generation and its swapchain (an FSR FG provider)
//   custom\<name>\ffx_fg_vk  Vulkan FFX frame generation and its swapchain
//   custom\<name>\dlss       NGX runtime that runs DLSS (nvngx.dll)
//   custom\<name>\dlssg      nvngx_dlssg.dll replacement (an FG Nvngx entry)
//   custom\<name>\dlssd      nvngx_dlssd.dll replacement (ray reconstruction)
// The dll may have any name; when the folder has several, the kind's standard name wins, else the first dll. For the
// DLSS-G kind a dll directly in custom\<name> is also taken (the first layout this fork shipped).
// Dlls load by full path with LOAD_WITH_ALTERED_SEARCH_PATH, so files a provider loads itself are found next to it.

#include "Config.h"
#include "State.h"

#include <proxies/KernelBase_Proxy.h>
#include <proxies/Ntdll_Proxy.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace CustomProviders
{
// Same values as the plugin API's OptiProviderKind
enum class Kind : uint32_t
{
    Ffx,
    FfxVk,
    FfxFg,
    FfxFgVk,
    Dlss,
    Dlssg,
    Dlssd,
    Count
};

// Where a plugin's provider resolves its exports (plugin API RegisterProvider)
using GetProcFn = void* (*) (void* userData, const char* exportName);

struct KindInfo
{
    const wchar_t* folder;
    const wchar_t* standardNames[2];
    const char* label; // for logs and menu tooltips
};

inline const KindInfo& Info(Kind kind)
{
    static const KindInfo infos[] = {
        { L"ffx", { L"amd_fidelityfx_upscaler_dx12.dll", L"amd_fidelityfx_dx12.dll" }, "FFX upscaler (DX12)" },
        { L"ffx_vk", { L"amd_fidelityfx_upscaler_vk.dll", L"amd_fidelityfx_vk.dll" }, "FFX upscaler (Vulkan)" },
        { L"ffx_fg", { L"amd_fidelityfx_framegeneration_dx12.dll", L"amd_fidelityfx_dx12.dll" }, "FSR FG (DX12)" },
        { L"ffx_fg_vk", { L"amd_fidelityfx_framegeneration_vk.dll", L"amd_fidelityfx_vk.dll" }, "FSR FG (Vulkan)" },
        { L"dlss", { L"nvngx.dll", nullptr }, "DLSS runtime" },
        { L"dlssg", { L"nvngx_dlssg.dll", nullptr }, "DLSS-G replacement" },
        { L"dlssd", { L"nvngx_dlssd.dll", nullptr }, "ray reconstruction" },
    };

    return infos[(size_t) kind];
}

// A folder name, never a path: what the ini and plugins may name
inline bool IsValidName(const std::string& name)
{
    return !name.empty() && name != "." && name != ".." && name.find_first_of("\\/:*?\"<>|") == std::string::npos;
}

inline std::filesystem::path Root()
{
    return std::filesystem::path(Config::Instance()->MainDllPath.value()) / L"custom";
}

inline std::filesystem::path Folder(const std::string& name) { return Root() / string_to_wstring(name); }

// The dll for a kind in custom\<name>, empty when there is none
inline std::filesystem::path DllPath(const std::string& name, Kind kind)
{
    if (!IsValidName(name))
        return {};

    std::error_code error;
    const auto& info = Info(kind);

    auto pick = [&info, &error](const std::filesystem::path& folder) -> std::filesystem::path
    {
        for (auto standardName : info.standardNames)
        {
            if (standardName != nullptr && std::filesystem::is_regular_file(folder / standardName, error))
                return folder / standardName;
        }

        for (auto& entry : std::filesystem::directory_iterator(folder, error))
        {
            if (entry.is_regular_file(error) && _wcsicmp(entry.path().extension().c_str(), L".dll") == 0)
                return entry.path();
        }

        return {};
    };

    if (auto path = pick(Folder(name) / info.folder); !path.empty())
        return path;

    // custom\<name>\nvngx_dlssg.dll, this fork's first layout; only the standard name, the folder holds other kinds
    if (kind == Kind::Dlssg && std::filesystem::is_regular_file(Folder(name) / L"nvngx_dlssg.dll", error))
        return Folder(name) / L"nvngx_dlssg.dll";

    return {};
}

// Where a custom provider's exports come from: its dll, or a plugin's resolver
struct Exports
{
    HMODULE dll = nullptr;
    std::filesystem::path path; // empty for a plugin
    GetProcFn getProc = nullptr;
    void* userData = nullptr;

    void* operator()(const char* exportName) const
    {
        if (getProc != nullptr)
            return getProc(userData, exportName);

        return dll != nullptr ? (void*) KernelBaseProxy::GetProcAddress_()(dll, exportName) : nullptr;
    }

    explicit operator bool() const { return dll != nullptr || getProc != nullptr; }

    // The FFX SDK 1.x Vulkan dll takes the old Vulkan backend description, the newer ones the V2 type
    bool IsLegacyFfxVk() const { return _wcsicmp(path.filename().c_str(), L"amd_fidelityfx_vk.dll") == 0; }
};

struct PluginProvider
{
    std::string name;
    Kind kind;
    GetProcFn getProc;
    void* userData;
};

inline std::mutex pluginMutex;
inline std::vector<PluginProvider> pluginProviders;

// Folder names are not case sensitive
inline bool Contains(const std::vector<std::string>& names, const std::string& name)
{
    return std::ranges::any_of(names, [&name](const std::string& n) { return _stricmp(n.c_str(), name.c_str()) == 0; });
}

// The names that have this kind: the folders (scanned once) and then the plugin providers registered so far
inline std::vector<std::string> List(Kind kind)
{
    static const auto folders = []
    {
        std::array<std::vector<std::string>, (size_t) Kind::Count> found;
        std::error_code error;

        for (auto& entry : std::filesystem::directory_iterator(Root(), error))
        {
            if (!entry.is_directory(error))
                continue;

            const auto name = wstring_to_string(entry.path().filename().wstring());

            for (size_t k = 0; k < found.size(); k++)
            {
                if (!DllPath(name, (Kind) k).empty())
                    found[k].push_back(name);
            }
        }

        return found;
    }();

    auto names = folders[(size_t) kind];

    std::scoped_lock lock(pluginMutex);
    for (auto& provider : pluginProviders)
    {
        if (provider.kind == kind && !Contains(names, provider.name))
            names.push_back(provider.name);
    }

    return names;
}

inline bool Has(const std::string& name, Kind kind) { return IsValidName(name) && Contains(List(kind), name); }

// Set by the plugin API (S6) so a custom dll that exports OptiPluginInit gets the API when it loads
using DllLoadedFn = void (*)(HMODULE dll, const std::string& name);
inline DllLoadedFn onDllLoaded = nullptr;

// Loads the dll found for a kind (DllPath); logs the path and the error when it fails
inline HMODULE LoadDll(const std::string& name, Kind kind, const std::filesystem::path& path)
{
    if (path.empty())
    {
        LOG_ERROR("Custom {} {}: no dll in {}", Info(kind).label, name,
                  wstring_to_string((Folder(name) / Info(kind).folder).wstring()));
        return nullptr;
    }

    auto dll = NtdllProxy::LoadLibraryExW_Ldr(path.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (dll == nullptr)
    {
        // ERROR_DLL_INIT_FAILED: the dll refused to load, e.g. it needs a module this process does not have
        const DWORD error = GetLastError();
        LOG_ERROR("Custom {} {}: {} did not load, error {}{}", Info(kind).label, name,
                  wstring_to_string(path.wstring()), error,
                  error == ERROR_DLL_INIT_FAILED ? " (the dll refused to load; it needs something this process lacks)"
                                                 : "");
        return nullptr;
    }

    LOG_INFO("Custom {} {}: loaded {}", Info(kind).label, name, wstring_to_string(path.wstring()));

    if (onDllLoaded != nullptr)
        onDllLoaded(dll, name);

    return dll;
}

// The FFX SDK 1.x Vulkan dll takes the old Vulkan backend description, the newer ones the V2 type
inline bool IsLegacyFfxVkDll(const std::filesystem::path& path)
{
    return _wcsicmp(path.filename().c_str(), L"amd_fidelityfx_vk.dll") == 0;
}

// The exports of name's kind: a plugin's first, then the folder's dll. Empty when there are none.
inline Exports Load(const std::string& name, Kind kind)
{
    {
        std::scoped_lock lock(pluginMutex);
        for (auto& provider : pluginProviders)
        {
            if (provider.kind == kind && _stricmp(provider.name.c_str(), name.c_str()) == 0)
            {
                LOG_INFO("Custom {} {}: from a plugin", Info(kind).label, name);
                return { .getProc = provider.getProc, .userData = provider.userData };
            }
        }
    }

    const auto path = DllPath(name, kind);
    auto dll = LoadDll(name, kind, path);
    if (dll == nullptr)
        return {};

    return { .dll = dll, .path = path };
}

inline void ResolveRegistered(const std::string& name);

// From the plugin API: a plugin's name takes the place of a folder with the same name
inline bool Register(const std::string& name, Kind kind, GetProcFn getProc, void* userData)
{
    if (!IsValidName(name) || kind >= Kind::Count || getProc == nullptr)
        return false;

    {
        std::scoped_lock lock(pluginMutex);
        if (std::ranges::any_of(pluginProviders, [&](const PluginProvider& p)
                                { return p.kind == kind && _stricmp(p.name.c_str(), name.c_str()) == 0; }))
        {
            LOG_WARN("Custom {} {} is already registered", Info(kind).label, name);
            return false;
        }

        pluginProviders.push_back({ name, kind, getProc, userData });
    }

    LOG_INFO("Custom {} {} registered by a plugin", Info(kind).label, name);

    // The ini can name it before it is registered
    ResolveRegistered(name);
    return true;
}
// The upscaler type through Config: Hudfix_Dx12.h's unscoped CaptureInfo::Upscaler hides ::Upscaler in files that
// include it first
using Upscaler = decltype(Config::Dx12Upscaler)::value_type;

// Per-API upscaler settings: [Upscalers] Dx11Upscaler / Dx12Upscaler / VulkanUpscaler may name a custom provider
// (upstream OptiScaler 74ae2c65)
inline CustomOptional<std::string, NoDefault>& NameSetting(API api)
{
    auto cfg = Config::Instance();
    return api == API::DX11     ? cfg->CustomUpscalerDx11
           : api == API::Vulkan ? cfg->CustomUpscalerVk
                                : cfg->CustomUpscaler;
}

inline CustomOptional<Upscaler, SoftDefault>& UpscalerSetting(API api)
{
    auto cfg = Config::Instance();
    return api == API::DX11 ? cfg->Dx11Upscaler : api == API::Vulkan ? cfg->VulkanUpscaler : cfg->Dx12Upscaler;
}

// Upscalers a custom name runs as on the API, the native one first (DX11 only through DX12, Vulkan also through it)
inline std::vector<Upscaler> Upscalers(const std::string& name, API api)
{
    std::vector<Upscaler> upscalers;

    auto add = [&](Upscaler upscaler, Kind kind)
    {
        if (Has(name, kind))
            upscalers.push_back(upscaler);
    };

    if (api == API::DX11)
    {
        add(Upscaler::FFX_on12, Kind::Ffx);
        add(Upscaler::DLSS_on12, Kind::Dlss);
    }
    else if (api == API::Vulkan)
    {
        add(Upscaler::FFX, Kind::FfxVk);
        add(Upscaler::FFX_on12, Kind::Ffx);
        add(Upscaler::DLSS, Kind::Dlss);
    }
    else
    {
        add(Upscaler::FFX, Kind::Ffx);
        add(Upscaler::DLSS, Kind::Dlss);
    }

    return upscalers;
}

inline bool Runs(const std::string& name, API api, Upscaler upscaler)
{
    return std::ranges::contains(Upscalers(name, api), upscaler);
}

// Custom name picked for the API when it runs as that upscaler, empty for the regular ones
inline std::string Selected(API api, Upscaler upscaler)
{
    auto name = NameSetting(api).value_or("");
    return Runs(name, api, upscaler) ? name : "";
}

// The API's upscaler setting from the ini as the upscaler a custom name runs as. Kept while an ASI plugin may still
// register it.
inline void ResolveUpscaler(API api)
{
    auto& setting = NameSetting(api);
    auto name = setting.value_or("");

    if (name.empty())
        return;

    if (auto upscalers = Upscalers(name, api); !upscalers.empty())
    {
        UpscalerSetting(api).set_from_config(upscalers[0]);
    }
    else if (!Config::Instance()->LoadAsiPlugins.value_or_default())
    {
        LOG_WARN("{} is not an upscaler or a custom upscaler for API {} in {}", name, (int) api,
                 wstring_to_string(Root().wstring()));
        setting.reset();
    }
}

// A custom name of FGNvngxReplacement, FSRFGProvider or RRUpscaler from the ini; kept while a plugin may register it
inline bool ResolveName(const std::string& name, std::initializer_list<Kind> kinds, const char* key)
{
    if (name.empty() || std::ranges::any_of(kinds, [&name](Kind kind) { return Has(name, kind); }) ||
        Config::Instance()->LoadAsiPlugins.value_or_default())
    {
        return true;
    }

    LOG_WARN("{}={} is not a replacement or a custom provider in {}", key, name, wstring_to_string(Root().wstring()));
    return false;
}

// Once OptiScaler's folder is known (dllmain)
inline void ResolveAll()
{
    for (auto api : { API::DX11, API::DX12, API::Vulkan })
        ResolveUpscaler(api);

    // FGNvngxReplacement=<name>: Nvngx_FG falls back by itself when the provider is missing, only warn here
    ResolveName(Config::Instance()->FGNvngxCustomName, { Kind::Dlssg }, "FGNvngxReplacement");

    if (!ResolveName(Config::Instance()->FSRFGProvider.value_or(""), { Kind::FfxFg, Kind::FfxFgVk }, "FSRFGProvider"))
        Config::Instance()->FSRFGProvider.reset();

    if (!ResolveName(Config::Instance()->CustomRRUpscaler.value_or(""), { Kind::Dlssd }, "RRUpscaler"))
        Config::Instance()->CustomRRUpscaler.reset();
}

// After a plugin registered a provider: the ini may name it already
inline void ResolveRegistered(const std::string& name)
{
    for (auto api : { API::DX11, API::DX12, API::Vulkan })
    {
        if (_stricmp(NameSetting(api).value_or("").c_str(), name.c_str()) == 0)
            ResolveUpscaler(api);
    }
}
} // namespace CustomProviders
