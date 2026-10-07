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

inline const char* DecisionText(Decision decision)
{
    switch (decision)
    {
    case Decision::SettingOff:
        return "Off: switched off";
    case Decision::NoF5Low:
        return "Off: no Optical F5Low mode is running";
    case Decision::GameRunsReflex:
        return "Off: the game runs its own Reflex";
    case Decision::ForceReflexDisabled:
        return "Off: Force Reflex is set to Force Disable in the fakenvapi settings";
    case Decision::ForceXell:
        return "Off: Force XeLL is on in the fakenvapi settings and already runs XeLL";
    case Decision::FrameGenerationOwnsReflex:
        return "Off: the frame generation in use runs Reflex itself";
    case Decision::NoApi:
        return "Off: no Reflex or fakenvapi interface could be found";
    default:
        return "";
    }
}

}// namespace native::lowlatency
