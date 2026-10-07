#pragma once

// When F5Low's own low latency (native/NativeLowLatency.cpp) runs, and what the menu says when it does not. No D3D, no
// ImGui, no Config: a host test can include it.

#include <cstdint>

namespace native::lowlatency
{
// [DlssNr] NativeLowLatency. Auto: with an F5Low mode running. On: also without one, in any game that makes no Reflex
// call of its own. Off: never.
enum class Setting : uint32_t
{
    Auto,
    On,
    Off
};

struct Inputs
{
    Setting setting = Setting::Auto;
    bool f5lowRunning = false;              // an F5Low mode is on and not standing aside for a game upscaler
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
inline const char* DecisionText(Decision decision)
{
    switch (decision)
    {
    case Decision::SettingOff:
        return "Off: switched off";
    case Decision::NoF5Low:
        return "Off: no F5Low mode is running";
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
