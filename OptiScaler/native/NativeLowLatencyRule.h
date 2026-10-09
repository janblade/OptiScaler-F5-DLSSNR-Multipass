#pragma once

// When Optical F5Low's own low latency (native/NativeLowLatency.cpp) runs, and what the menu says when it does not. No
// D3D, no ImGui, no Config: a host test can include it.

#include <cstdint>

namespace native::lowlatency
{
// [DlssNr] NativeLowLatency. Auto: with an Optical F5Low mode running. On: also without one, in any game that makes no
// Reflex call of its own. Off: never.
enum class Setting : uint32_t
{
    Auto,
    On,
    Off
};

struct Inputs
{
    Setting setting = Setting::Auto;
    bool f5lowRunning = false;              // an Optical F5Low mode is on and not standing aside for a game upscaler
    bool gameCallsReflex = false;           // the game has made a Reflex call (SetSleepMode, Sleep or a marker)
    bool forceReflexDisabled = false;       // fakenvapi's Force Reflex is Force Disable
    bool forceXell = false;                 // fakenvapi's Force XeLL (it takes the frame generation slot and runs XeLL)
    bool otherFrameGenerationOwner = false; // frame generation that runs Reflex itself (DLSS FG, an external owner)
    bool apiAvailable = true;               // the Reflex function table could be found
};

enum class Decision
{
    Run,
    SettingOff,
    NoF5Low,
    GameRunsReflex,
    ForceReflexDisabled,
    ForceXell,
    FrameGenerationOwnsReflex,
    NoApi
};

// Order matters: what the player can change comes first, then what stops us for another reason.
inline Decision Decide(const Inputs& in)
{
    if (in.setting == Setting::Off)
        return Decision::SettingOff;

    if (in.setting == Setting::Auto && !in.f5lowRunning)
        return Decision::NoF5Low;

    if (in.gameCallsReflex)
        return Decision::GameRunsReflex;

    if (in.forceReflexDisabled)
        return Decision::ForceReflexDisabled;

    if (in.forceXell)
        return Decision::ForceXell;

    if (in.otherFrameGenerationOwner)
        return Decision::FrameGenerationOwnsReflex;

    if (!in.apiAvailable)
        return Decision::NoApi;

    return Decision::Run;
}

// Which Reflex surface a call goes through: the D3D-flavoured NVAPI entry points (NvAPI_D3D_*, also what XeFG's routing
// to XeLL hangs off) or the Vulkan-flavoured ones (NvAPI_Vulkan_*). Each call carries its own: nothing is remembered
// between calls, so a D3D12 device pointer can never reach a Vulkan entry point or the other way round.
enum class Api
{
    D3D,
    Vulkan
};

struct Target
{
    Api api = Api::D3D;
    const void* device = nullptr; // null: nothing to call
};

// A Vulkan game's present while the frame-generation bridge is up is the bridge's D3D12 swapchain's, on the bridge's
// private D3D12 device: its low latency goes through the D3D surface on that device (so XeFG's XeLL routing reaches it,
// as for a D3D11 game's hidden present) and no Vulkan NVAPI call is made. Without the bridge a Vulkan game's own
// VkDevice goes through the Vulkan surface. A D3D game's present is always D3D.
inline Target PresentTarget(bool vulkanGame, bool bridged, const void* gameDevice, const void* bridgeDevice12)
{
    if (!vulkanGame)
        return { Api::D3D, gameDevice };

    if (bridged)
        return { Api::D3D, bridgeDevice12 };

    return { Api::Vulkan, gameDevice };
}

// Plain words for the menu when we do not run. Run has no text here: the path in use names it.
// Where a present was seen. With OptiScaler's frame generation on a D3D12 game, the game presents to frame
// generation's swap chain (FGHooks::FGPresent) and frame generation then presents every shown frame, real and
// generated, through the wrapped swap chain on its own thread: only the first is the game's frame.
enum class PresentSource
{
    SwapChain,      // the wrapped swap chain (no frame generation swap chain), or the D3D11 bridge's game present
    FrameGeneration // FGHooks::FGPresent, the game's present to frame generation
};

// How long a FrameGeneration present keeps SwapChain presents ignored: frame generation's swap chain can go away
// (turned off, swap chain recreated), and then the wrapped swap chain's presents are the game's again.
inline constexpr double kFrameGenerationPresentHoldMs = 1000.0;

// A SwapChain present is skipped while FrameGeneration presents come in. `lastFrameGenerationMs` is 0 when none came.
inline bool SwapChainPresentIgnored(double nowMs, double lastFrameGenerationMs)
{
    return lastFrameGenerationMs > 0.0 && nowMs - lastFrameGenerationMs < kFrameGenerationPresentHoldMs;
}

// The menu's status line: "Status: [<StatusLabel>]: <reason>". On: it runs (the reason is the method in use). Standing
// Aside: something else owns latency here (the game's Reflex, a fakenvapi setting, frame generation). Off: nothing to
// run on, or switched off.
inline const char* StatusLabel(Decision decision)
{
    switch (decision)
    {
    case Decision::Run:
        return "On";
    case Decision::GameRunsReflex:
    case Decision::ForceReflexDisabled:
    case Decision::ForceXell:
    case Decision::FrameGenerationOwnsReflex:
        return "Standing Aside";
    default:
        return "Off";
    }
}

// The reason when it does not run. Run has none here: the method in use names it.
inline const char* DecisionText(Decision decision)
{
    switch (decision)
    {
    case Decision::SettingOff:
        return "Switched off";
    case Decision::NoF5Low:
        return "No Optical F5Low mode is running";
    case Decision::GameRunsReflex:
        return "Please use the Game's Own Reflex Settings";
    case Decision::ForceReflexDisabled:
        return "Force Reflex is set to Force Disable in the fakenvapi settings";
    case Decision::ForceXell:
        return "Force XeLL is on in the fakenvapi settings and already runs XeLL";
    case Decision::FrameGenerationOwnsReflex:
        return "The frame generation in use runs Reflex itself";
    case Decision::NoApi:
        return "No Reflex or fakenvapi interface could be found";
    default:
        return "";
    }
}

}// namespace native::lowlatency
