#pragma once

// NR profiles: the Neural Rendering settings saved under a name in OptiScaler\NRProfiles\<name>.ini and loaded back,
// in this game or another. Plain data and rules, no config and no ImGui, so tests/nr_profiles_smoke.cpp runs them;
// Config.cpp writes and reads the files, dlssnr/DlssNr_Menu.cpp draws the NR Profiles section.
//
// A profile keeps how NR looks and how much it costs: what the preset rows set, plus passes, colour, output, input
// style, reuse and the LUT. It leaves out what belongs to the game, the display or the session (exposure, colour
// encoding, paper white, Optical F5Low, Finished Picture, edge compression, debug views, the toggle key), so a profile
// made in one game does not break another. Every key Config::SaveIni writes is in exactly one of the two lists; the
// test fails on a new key that is in neither.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace DlssNrProfiles
{
inline constexpr const wchar_t* kFolderName = L"NRProfiles"; // inside the OptiScaler folder, beside LUTs
inline constexpr std::size_t kMaxNameLength = 48;
inline constexpr int kFileVersion = 1; // [Profile] Version

// [DlssNr] keys a profile keeps. Pass4..Pass30 keys (written in a loop) are kept too, see IsKept.
inline constexpr std::string_view kKeptKeys[] = {
    // placement and model
    "RunBeforeSR", "DeferredDLSS", "Precision", "Preset", "WorkingScale", "ModelResolutionAuto", "ReducedUpscaleMethod",
    "Transfer", "SuperiorScalingPreset", "ScalingDownscaler",
    // reuse
    "VitEvery", "VitEveryPlain", "DetailReuse", "DetailReuseSteady", "DetailReuseFill", "DetailReuseInterval",
    "DetailReuseSpread", "DetailReuseDepthTolerance", "DetailReuseClipGamma", "DetailReuseClipFalloff",
    "DetailReuseSigmaFloor", "DetailReuseWithFG", "DetailReuseMinFps", "DetailReuseMaxDropped", "SceneCut", "EditCache",
    "EditCacheInterval", "EditCacheCrossfade",
    // look
    "TransferStrength", "ColourStrength", "ReplaceDetailStrength", "MaxRatio", "ReversibleMode", "Passes",
    "PassFeedback", "UnlockPasses", "Style", "Intensity", "LocalStructure", "LocalTone", "SkinStructure", "AutoMask",
    "Pass2Preset", "Pass2Style", "Pass2Intensity", "Pass2LocalStructure", "Pass2LocalTone", "Pass2SkinStructure",
    "Pass2AutoMask", "Pass3Preset", "Pass3Style", "Pass3Intensity", "Pass3LocalStructure", "Pass3LocalTone",
    "Pass3SkinStructure", "Pass3AutoMask", "SkinProtection", "SkinToneEnabled", "SkinDetail", "SkinColour",
    "EnvironmentDetail", "EnvironmentColour",
    // input style and grade
    "InputFlatten", "InputFlattenStrength", "InputStructureHint", "InputStructureStrength", "InputStyleFacesOnly",
    "LutFile", "LutStrength",
};

// [DlssNr] keys a profile leaves out.
inline constexpr std::string_view kLeftOutKeys[] = {
    // on/off and the game's own path
    "Enabled", "FinishedPicture", "ToggleKey", "NativeLowLatency",
    // the old single input style: written from the input style card for older builds, the card's keys are kept
    "InputStyle", "InputStyleStrength",
    // exposure, colour encoding, display
    "WhitePointFromExposure", "WhitePointSource", "GameExposureScale", "WhitePointTrim", "WhitePointScale",
    "AutoExposureTrim", "AutoExposureShadowProtection", "AutoExposureMeter", "AutoExposureMeterLowPercent",
    "AutoExposureMeterHighPercent", "AutoExposureAdaptBrighterSeconds", "AutoExposureAdaptDarkerSeconds",
    "AutoExposureFollowGame", "GameExposureTrimAnchors", "AutoExposureTrimAnchors", "ScanTrim", "ScanAnchorValue",
    "ScanAnchorWhitePoint", "ScanAnchors", "ScanInverted", "ScanMeter", "ScanExposure", "RecordExposureScan",
    "ColourEncoding", "DisplayPaperWhiteNits",
    // Optical F5Low and the native path
    "NativeDepthFinder", "NativeDepthVkCopyUsage", "NativeDepthResolveInList", "NativeDxvkVulkan",
    "NativeDepthWarmupFrames", "NativeDepthOverlay", "NativeDebugView", "NativeMotionProfile",
    "NativeMotionProfileCustom", "NativeMotion", "NativeInput", "NativeUpscaler", "NativeUpscalerPass",
    "NativeFrameGenerationOnly", "NativeLowLatencyBoost", "NativeLowLatencyDxvkGameThread",
    // edge compression: laid out for this game's picture
    "SpatialCompression", "SpatialCenterX", "SpatialCenterY", "SpatialWorkX", "SpatialWorkY", "SpatialOffsetX",
    "SpatialOffsetY", "SpatialShiftX", "SpatialShiftY",
    // views, diagnostics, testing
    "DebugView", "Compare", "CompareSplit", "CompareZoom", "CompareSwap", "CompareTags", "TagScale", "ShowSkinMask",
    "DetailReuseDebug", "EditCacheDebug", "ProbeD3D11", "FrameStats", "KernelProfile", "AutoCapture", "UseProxy",
    "ProxyProbe", "ApplyModel", "HoldFrame",
};

inline bool EqualNoCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// Pass4Style .. Pass30AutoMask (Config::DlssNrExtraPasses)
inline bool IsExtraPassKey(std::string_view key)
{
    if (key.size() < 5 || !EqualNoCase(key.substr(0, 4), "Pass") || key[4] < '1' || key[4] > '9')
        return false;

    std::size_t i = 4;
    int pass = 0;
    while (i < key.size() && i < 6 && std::isdigit(static_cast<unsigned char>(key[i])))
        pass = pass * 10 + (key[i++] - '0');

    if (pass < 4 || pass > 30)
        return false;

    const std::string_view rest = key.substr(i);
    for (std::string_view suffix : { "Style", "Intensity", "LocalStructure", "LocalTone", "SkinStructure", "AutoMask" })
    {
        if (EqualNoCase(rest, suffix))
            return true;
    }
    return false;
}

inline bool IsKept(std::string_view key)
{
    if (IsExtraPassKey(key))
        return true;
    return std::any_of(std::begin(kKeptKeys), std::end(kKeptKeys), [&](std::string_view k) { return EqualNoCase(k, key); });
}

inline bool IsLeftOut(std::string_view key)
{
    return std::any_of(std::begin(kLeftOutKeys), std::end(kLeftOutKeys),
                       [&](std::string_view k) { return EqualNoCase(k, key); });
}

// A profile name as typed: outer spaces trimmed; letters, digits, space, - _ . only; at most kMaxNameLength; not
// starting with '.', not ending with '.'; not a Windows device name (CON, NUL, COM1 ...) with or without an
// extension. Anything else is no name, so a name can never be a path.
inline std::optional<std::string> SanitiseName(std::string_view typed)
{
    const auto first = typed.find_first_not_of(' ');
    if (first == std::string_view::npos)
        return std::nullopt;
    const std::string name(typed.substr(first, typed.find_last_not_of(' ') - first + 1));

    if (name.size() > kMaxNameLength || name.front() == '.' || name.back() == '.')
        return std::nullopt;

    for (const char c : name)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != ' ' && c != '-' && c != '_' && c != '.')
            return std::nullopt;
    }

    std::string stem = name.substr(0, name.find('.'));
    while (!stem.empty() && stem.back() == ' ')
        stem.pop_back();
    for (std::string_view device : { "CON", "PRN", "AUX", "NUL" })
    {
        if (EqualNoCase(stem, device))
            return std::nullopt;
    }
    if (stem.size() == 4 && (EqualNoCase(stem.substr(0, 3), "COM") || EqualNoCase(stem.substr(0, 3), "LPT")) &&
        stem[3] >= '1' && stem[3] <= '9')
        return std::nullopt;

    return name;
}

// <folder>\<name>.ini, only for a name SanitiseName keeps unchanged
inline std::optional<std::filesystem::path> ProfilePath(const std::filesystem::path& folder, std::string_view name)
{
    const auto clean = SanitiseName(name);
    if (!clean || *clean != name)
        return std::nullopt;

    auto path = folder / std::filesystem::path(*clean + ".ini");
    if (path.parent_path() != folder)
        return std::nullopt;
    return path;
}

// The profile name of a file in the folder: its stem, for a .ini whose stem is a valid name
inline std::optional<std::string> NameFromPath(const std::filesystem::path& path)
{
    if (!EqualNoCase(path.extension().string(), ".ini"))
        return std::nullopt;

    const std::string stem = path.stem().string();
    const auto clean = SanitiseName(stem);
    if (!clean || *clean != stem)
        return std::nullopt;
    return clean;
}

// A [DlssNr] section as key -> value text, the way SaveIni writes it ("auto" for a default)
struct NoCaseLess
{
    bool operator()(const std::string& a, const std::string& b) const
    {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
        });
    }
};
using Section = std::map<std::string, std::string, NoCaseLess>;

inline Section Filter(const Section& section)
{
    Section kept;
    for (const auto& [key, value] : section)
    {
        if (IsKept(key))
            kept.emplace(key, value);
    }
    return kept;
}

// Whether two sections hold the same kept settings. A missing key reads as "auto", as the ini reader has it.
inline bool Same(const Section& a, const Section& b)
{
    const auto valueIn = [](const Section& s, const std::string& key) -> std::string {
        const auto it = s.find(key);
        return it == s.end() ? std::string("auto") : it->second;
    };

    for (const Section* s : { &a, &b })
    {
        for (const auto& [key, value] : *s)
        {
            if (IsKept(key) && !EqualNoCase(valueIn(a, key), valueIn(b, key)))
                return false;
        }
    }
    return true;
}
} // namespace DlssNrProfiles
