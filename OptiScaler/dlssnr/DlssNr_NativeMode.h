#pragma once

// Modes of the menu's Optical F5Low page (NR without a game upscaler); no D3D/ImGui, so a host test can include it.

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
    std::optional<Keys> keys;            // nullopt: the keys are left as they are
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
        change.keys = Keys {};
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

inline Shown ShownFor(Mode mode)
{
    switch (mode)
    {
    case Mode::NrOnly:
        return Shown::NrOnly;
    case Mode::NrAndFrameGeneration:
        return Shown::NrAndFrameGeneration;
    default:
        return Shown::Off;
    }
}

// A click on the mode already in effect keeps the keys (an unticked depth stays so) but still puts NR Pass at: right.
inline Change ForClick(Mode clicked, Shown shown, bool finishedPictureNow)
{
    Change change = ForMode(clicked, finishedPictureNow);

    if (ShownFor(clicked) == shown)
        change.keys.reset();

    return change;
}

// The depth finder at this start: Installed whatever the key says now, since its hooks stay until a restart.
enum class Finder
{
    Off,
    NeedsRestart,
    CouldNotStart,
    Installed
};

inline Finder FinderFor(bool wanted, bool installed, bool installFailed)
{
    if (installed)
        return Finder::Installed;

    if (!wanted)
        return Finder::Off;

    return installFailed ? Finder::CouldNotStart : Finder::NeedsRestart;
}

inline bool DepthRestartWarning(Shown shown, Finder finder)
{
    return shown != Shown::Off && finder == Finder::NeedsRestart;
}

// Which API the game itself uses, for the Optical F5Low page's drivers, banner and depth status. State::swapchainApi
// (what the last present was) mirrors State's API and SwapchainInteropApi without including them: a host test can.
// A Vulkan game on the present bridge also presents through XeFG's D3D12 swapchain, and the wrapped swapchain's present
// writes D3D12 into swapchainApi every frame; the interop says what the game is. A dxvk game presents D3D frames
// through Vulkan, but its D3D drivers are the ones that run (swapchainApi is then not Vulkan).
enum class GameApi
{
    Dx12,
    Dx11,
    Vulkan
};

enum class PresentApi
{
    NotSelected,
    Dx11,
    Dx12,
    Vulkan
};

enum class Interop
{
    None,
    Dx11wDx12,
    VkwDx12
};

inline GameApi GameApiFor(PresentApi present, Interop interop, bool d3d11DevicePresent)
{
    if (interop == Interop::VkwDx12 || present == PresentApi::Vulkan)
        return GameApi::Vulkan;

    return d3d11DevicePresent ? GameApi::Dx11 : GameApi::Dx12;
}

// The modes are for a game with no upscaler call of its own; the frame sources stand aside while the game makes one
// (gameUpscaler: GenericDepthDx12/Dx11::GameCallsUpscaler). Then only Off can be chosen: a click on another mode would
// change NR Pass at: under the game's own upscaler for nothing.
inline bool Selectable(Mode mode, bool gameUpscaler)
{
    if (mode == Mode::Off)
        return true;

    return !gameUpscaler;
}

enum class Warning
{
    None,
    GameUpscaler,        // a mode is on while the game calls an upscaler of its own: it stands aside
    Dx11FrameGeneration, // NR only, while frame generation replaces a D3D11 game's swap chain
    VulkanNeedsRestart,  // NR + upscaler & frame generation in a Vulkan game whose swapchain was made without the bridge
    NrDisabled,          // Enable Neural Rendering is off
    NeedsFinishedPicture // NR only, with NR Pass at: not on Finished Picture
};

// nativeInputBlocked: DlssNr::NativeInputBlockedBySwapChainInterop(), the condition native input itself refuses on.
// gameUpscaler: as for Selectable; it comes first, since nothing else matters while the mode stands aside.
// vulkanNoBridge: a Vulkan game whose swapchain was made without the present bridge (native/VkPresentBridge.h): frame
// generation starts with the next start of the game, once chosen.
inline Warning WarningFor(Shown shown, bool nrEnabled, bool finishedPicture, bool nativeInputBlocked, bool gameUpscaler,
                          bool vulkanNoBridge = false)
{
    if (shown != Shown::Off && gameUpscaler)
        return Warning::GameUpscaler;

    if (shown == Shown::NrOnly)
    {
        if (nativeInputBlocked)
            return Warning::Dx11FrameGeneration;

        if (!nrEnabled)
            return Warning::NrDisabled;

        if (!finishedPicture)
            return Warning::NeedsFinishedPicture;
    }
    else if (shown == Shown::NrAndFrameGeneration && vulkanNoBridge)
    {
        return Warning::VulkanNeedsRestart;
    }
    else if (shown == Shown::NrAndFrameGeneration && !nrEnabled)
    {
        return Warning::NrDisabled;
    }

    return Warning::None;
}
} // namespace DlssNrNativeMode
