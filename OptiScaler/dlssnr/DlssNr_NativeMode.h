#pragma once

// Modes of the menu's "NR without a game upscaler" section; no D3D/ImGui, so a host test can include it.

#include <optional>

namespace DlssNrNativeMode
{
enum class Mode
{
    Off,
    NrOnly,
    NrAndFrameGeneration
};

// What the keys run. MotionOnly is not a mode: motion is estimated and nothing uses it.
enum class Shown
{
    Off,
    NrOnly,
    NrAndFrameGeneration,
    MotionOnly
};

struct Keys
{
    bool depthFinder = false; // NativeDepthFinder
    bool motion = false;      // NativeMotion
    bool input = false;       // NativeInput
    bool upscaler = false;    // NativeUpscaler
};

struct Change
{
    Keys keys;
    std::optional<bool> finishedPicture; // set only when it changes
    bool retryAfterFailure = false; // the NR Pass at: combo's rule: changing Finished Picture clears a session failure
};

inline Change ForMode(Mode mode, bool finishedPictureNow)
{
    Change change;
    std::optional<bool> finishedPicture;

    switch (mode)
    {
    case Mode::Off:
        break;
    case Mode::NrOnly:
        change.keys = { true, true, true, false };
        finishedPicture = true;
        break;
    case Mode::NrAndFrameGeneration:
        change.keys = { true, true, false, true };
        finishedPicture = false;
        break;
    }

    if (finishedPicture.has_value() && finishedPicture.value() != finishedPictureNow)
    {
        change.finishedPicture = finishedPicture;
        change.retryAfterFailure = true;
    }

    return change;
}

// Same precedence as the motion step's RunFrame: nothing runs without motion, and the upscaler wins over native input.
inline Shown FromKeys(const Keys& keys)
{
    if (!keys.motion)
        return Shown::Off;

    if (keys.upscaler)
        return Shown::NrAndFrameGeneration;

    return keys.input ? Shown::NrOnly : Shown::MotionOnly;
}

enum class Warning
{
    None,
    Dx11FrameGeneration, // NR only, while frame generation replaces a D3D11 game's swap chain
    NrDisabled,          // Enable Neural Rendering is off
    NeedsFinishedPicture // NR only, with NR Pass at: not on Finished Picture
};

inline Warning WarningFor(Shown shown, bool nrEnabled, bool finishedPicture, bool dx11SwapChainReplaced)
{
    if (shown == Shown::NrOnly)
    {
        if (dx11SwapChainReplaced)
            return Warning::Dx11FrameGeneration;

        if (!nrEnabled)
            return Warning::NrDisabled;

        if (!finishedPicture)
            return Warning::NeedsFinishedPicture;
    }
    else if (shown == Shown::NrAndFrameGeneration && !nrEnabled)
    {
        return Warning::NrDisabled;
    }

    return Warning::None;
}
} // namespace DlssNrNativeMode
