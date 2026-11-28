#pragma once

// What the menu says about a Vulkan game's route: the native swapchain (the game's own device, FSR FG or DLSS-G) or the
// bridge (a D3D12 swapchain on screen, a D3D12 device and copies; Optical F5Low's NR + frame generation or the Vulkan
// route "Bridge, game's upscaler"). Pure, so a host test runs every value (tests/vulkan_route_rule_smoke.cpp); the menu
// (dlssnr/DlssNr_Menu.cpp, menu/menu_common.cpp, menu/StatusPage.cpp) reads the state and draws.

#include <cstdint>
#include <string>

namespace DlssNrVulkanRoute
{
// Mirror of framegen/FGOutput.h, by value (menu_common.cpp checks it with static_asserts).
enum class Output : uint32_t
{
    NoFG,
    FSRFG,
    DLSSG,
    XeFG,
    F5Low
};

// XeFG is a D3D12 library (Intel ships no Vulkan frame generation): on a Vulkan game it needs the bridge. FSR FG and
// DLSS-G have a Vulkan swapchain of their own, and F5Low (XeFG Tensor) presents on the game's own Vulkan swapchain
// (framegen/f5low/F5Low_Vk: it generates on a private D3D12 device with shared images, no bridge).
inline bool OutputNeedsBridge(Output output) { return output == Output::XeFG; }

inline const char* OutputName(Output output)
{
    switch (output)
    {
    case Output::FSRFG:
        return "FSR FG";
    case Output::DLSSG:
        return "DLSSG";
    case Output::XeFG:
        return "XeFG";
    case Output::F5Low:
        return "F5Low";
    default:
        return "None";
    }
}

// The FG Output combo's entry: the name, and where the route is the native swapchain a short note on the two that
// need the bridge, so it is read before the entry is chosen (short: the combo's preview shows it too).
inline std::string OutputLabel(Output output, bool nativeRoute)
{
    std::string label = OutputName(output);

    if (nativeRoute && OutputNeedsBridge(output))
        label += " (bridge)";

    return label;
}

// The route is the native swapchain: neither the game's-upscaler route nor Optical F5Low's NR + frame generation is on.
inline bool NativeRoute(bool routeOffered, bool routeOn, bool f5lowFrameGeneration)
{
    return routeOffered && !routeOn && !f5lowFrameGeneration;
}

// What a chosen output on the native swapchain offers: the same switch as the route card's radio, or, where the
// game makes no upscaler call (the route needs one), Optical F5Low's NR + frame generation instead.
enum class Offer
{
    None,
    UseBridge,
    UseF5Low
};

inline Offer BridgeOffer(Output chosen, bool nativeRoute, bool gameCallsUpscaler)
{
    if (!nativeRoute || !OutputNeedsBridge(chosen))
        return Offer::None;

    return gameCallsUpscaler ? Offer::UseBridge : Offer::UseF5Low;
}

inline std::string BridgeNeededLine(Output chosen)
{
    return std::string(OutputName(chosen)) + " runs on D3D12: it needs the Vulkan bridge.";
}

// Where Neural Rendering runs on a Vulkan game. The native seam runs it on Vulkan; every other way runs it on a D3D12
// device of its own: the game's upscaler on its D3D12 copy (the route, or a *_12 upscaler chosen as the Vulkan one),
// Optical F5Low's NR only / NR + frame generation (private device, or the bridge's, with copies), and Finished Picture.
enum class RunsOn
{
    NrOff,
    Vulkan,
    D3D12GameCopy, // the game's upscaler on its D3D12 copy, the route is off or the bridge is not up yet
    D3D12Bridge,   // the same, the route on and the bridge up
    D3D12F5Low,    // Optical F5Low's NR only / NR + frame generation
    D3D12FinishedPicture,
    D3D12,                    // running on D3D12, which no setting explains
    FinishedPictureNotRunning // Finished Picture is on but not running (refused: frame generation, format, ...)
};

// Whether Finished Picture runs on a native Vulkan game (it is made there by a Vulkan frame source on a private D3D12
// device, #353); false only for a build without it: the rule below is the only place.
inline constexpr bool kFinishedPictureRunsOnVulkan = true;

struct NrInputs
{
    bool nrEnabled = false;
    bool routeOn = false;
    bool bridgeUp = false;
    bool upscalerOnD3D12 = false; // the Vulkan upscaler setting is a *_12 one
    bool motionModeOn = false;    // Optical F5Low NR only or NR + frame generation
    bool finishedPicture = false; // the setting (the route turns it off)
    bool finishedPictureRunsOnVulkan = kFinishedPictureRunsOnVulkan;
    bool finishedPictureRan = false; // it ran on the last present (NativeMotionVk::FinishedPictureRan): Neural
                                     // Rendering is its, not Optical F5Low's, whatever the Optical F5Low setting says
    bool runningVk = false;          // DlssNr::IsRunningVk(): the native Vulkan seam is running Neural Rendering
    bool runningD3D12 = false;       // DlssNr::IsRunning(): the D3D12 side is
};

inline RunsOn NrRunsOn(const NrInputs& in)
{
    if (!in.nrEnabled)
        return RunsOn::NrOff;

    // What is running wins over what the settings suggest
    if (in.runningVk)
        return RunsOn::Vulkan;

    RunsOn byDesign = RunsOn::Vulkan;

    if (in.routeOn)
        byDesign = in.bridgeUp ? RunsOn::D3D12Bridge : RunsOn::D3D12GameCopy;
    else if (in.finishedPicture && in.finishedPictureRunsOnVulkan && in.finishedPictureRan && in.runningD3D12)
        byDesign = RunsOn::D3D12FinishedPicture; // also with Optical F5Low's motion on: it makes the picture, Finished
                                                 // Picture runs NR
    else if (in.motionModeOn)
        byDesign = RunsOn::D3D12F5Low;
    else if (in.finishedPicture)
        // It runs on the private D3D12 device; while it is not running there (refused or waiting) say so, its status
        // line says why (IsRunningVk() is false while Finished Picture is what runs on a Vulkan game)
        byDesign = in.finishedPictureRunsOnVulkan && in.runningD3D12 ? RunsOn::D3D12FinishedPicture
                                                                     : RunsOn::FinishedPictureNotRunning;
    else if (in.upscalerOnD3D12)
        byDesign = RunsOn::D3D12GameCopy;

    if (in.runningD3D12 && (byDesign == RunsOn::Vulkan || byDesign == RunsOn::FinishedPictureNotRunning))
        return RunsOn::D3D12;

    return byDesign;
}

// The route card's line, and the Neural Rendering pages' (which already have the NR off line above it: nothing here).
inline std::string RunsOnText(RunsOn runsOn)
{
    switch (runsOn)
    {
    case RunsOn::Vulkan:
        return "Vulkan (native swapchain)";
    case RunsOn::D3D12GameCopy:
        return "D3D12 (game's upscaler copy)";
    case RunsOn::D3D12Bridge:
        return "D3D12 (Vulkan bridge)";
    case RunsOn::D3D12F5Low:
        return "D3D12 (Optical F5Low, copies)";
    case RunsOn::D3D12FinishedPicture:
        return "D3D12 (Finished Picture, copies)";
    case RunsOn::D3D12:
        return "D3D12";
    case RunsOn::FinishedPictureNotRunning:
        return "none (Finished Picture is not running, see its status)";
    default:
        return "off";
    }
}

// The route card's "Now:" line, in full words (the short forms above are for the Status page and the pages' lines)
inline std::string CardRunsOnLine(RunsOn runsOn)
{
    switch (runsOn)
    {
    case RunsOn::NrOff:
        return "Now: Neural Rendering is off.";
    case RunsOn::Vulkan:
        return "Now: Neural Rendering runs on Vulkan, in the game's own device.";
    case RunsOn::D3D12Bridge:
        return "Now: Neural Rendering runs on D3D12, through the bridge.";
    case RunsOn::D3D12F5Low:
        return "Now: Neural Rendering runs on D3D12, because Optical F5Low copies the picture there.";
    case RunsOn::D3D12FinishedPicture:
        return "Now: Neural Rendering runs on D3D12, because Finished Picture copies the picture there.";
    case RunsOn::D3D12GameCopy:
        return "Now: Neural Rendering runs on D3D12, because the game's upscaler still runs on its D3D12 copy.";
    case RunsOn::FinishedPictureNotRunning:
        return "Now: Neural Rendering is not running (Finished Picture is waiting, see its status).";
    default:
        return "Now: Neural Rendering runs on D3D12.";
    }
}

// The page line is not drawn while Neural Rendering is off
inline std::string PageRunsOnLine(RunsOn runsOn) { return "Runs on: " + RunsOnText(runsOn); }

// The route is off, no Optical F5Low mode is on (it makes the upscaler call itself), and the Vulkan upscaler setting is
// DLSS on D3D12: only the route sets that one (the others, ffx_12 and fsr21_12, are choices of the Upscaler page), so
// Neural Rendering silently stays on D3D12.
inline bool LeftoverD3D12Upscaler(bool routeOffered, bool routeOn, bool motionModeOn, bool dlssOn12Backend)
{
    return routeOffered && !routeOn && !motionModeOn && dlssOn12Backend;
}

inline std::string LeftoverLine(const std::string& backendCode)
{
    return "The game's upscaler runs on its D3D12 copy (" + backendCode + "), so Neural Rendering runs on D3D12.";
}

// The route card's two rows, always shown.
inline const char* NativeSummary()
{
    return "Native swapchain: frame generation with FSR FG, DLSS-G or F5Low. Lighter than the bridge.";
}
inline const char* BridgeSummary() { return "Bridge: adds XeFG. Costs a second (D3D12) device and picture copies."; }

// The Status page's "Vulkan route" row: the route, then where Neural Rendering runs.
inline std::string StatusText(bool routeOn, bool f5lowFrameGeneration, bool leftover, bool bridgeUp,
                              const std::string& backendCode, RunsOn runsOn)
{
    std::string text;

    if (routeOn)
        text = "Bridge, game's upscaler";
    else if (f5lowFrameGeneration)
        text = "Bridge, Optical F5Low NR + frame generation";
    else if (leftover)
        text = "Native swapchain, but the upscaler is still " + backendCode;
    else if (bridgeUp)
        text = "Native swapchain after a restart (the bridge is still up)";
    else
        text = "Native swapchain";

    switch (runsOn)
    {
    case RunsOn::NrOff:
        return text + " (Neural Rendering off)";
    case RunsOn::Vulkan:
        return text + " (Neural Rendering on Vulkan)";
    case RunsOn::D3D12F5Low:
        return text + " (Neural Rendering on D3D12, Optical F5Low copies)";
    case RunsOn::D3D12FinishedPicture:
        return text + " (Neural Rendering on D3D12, Finished Picture copies)";
    case RunsOn::FinishedPictureNotRunning:
        return text + " (Finished Picture is not running)";
    default:
        return text + " (Neural Rendering on D3D12)";
    }
}
} // namespace DlssNrVulkanRoute
