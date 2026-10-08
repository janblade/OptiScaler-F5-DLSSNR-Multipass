#pragma once

// Which line the main menu's header shows about the upscaler, and which button goes with it. Pure, so a host test can
// run every row; the drawing (colours, the upscaler names, the file checks) stays in
// MenuCommon::RenderMainMenuHeaderMessages. "Optical F5Low" is the player-facing name of the native modes
// (dlssnr/DlssNr_NativeMode.h): NR for a game that makes no upscaler call of its own.

#include "../dlssnr/DlssNr_NativeMode.h"

namespace HeaderBanner
{
// What the upscaler state is.
enum class Feature
{
    None,   // no feature, or one that is not initialised
    Game,   // a game's feature, running
    Frozen, // a game's feature, not currently used by the game
    F5Low   // Optical F5Low's own virtual upscaler (NR + upscaler & frame generation)
};

struct Inputs
{
    Feature feature = Feature::None;
    bool upscalerFiles = false;     // nvngx, libxess or FSR inputs found
    bool gameCallsUpscaler = false; // GenericDepthDx12/Dx11::GameCallsUpscaler()
    DlssNrNativeMode::Shown mode = DlssNrNativeMode::Shown::Off;
    bool nrAvailable = false;        // NR can run here, so offering Optical F5Low makes sense
    bool f5lowNrOnlyRunning = false; // the native driver has run NR on the finished picture
    bool nrEnabled = false;          // Enable Neural Rendering is on and NR has not failed (any API)
    bool frameGeneration = false;    // OptiScaler's frame generation is running
};

enum class Line
{
    Blank,              // a game's feature is running: nothing to say
    Frozen,             // "<name> is active, but not currently used by the game." (unchanged)
    SelectUpscaler,     // "Select <upscalers> as the game's upscaler ..." (unchanged)
    NoFiles,            // "Can't find nvngx.dll, libxess.dll ..." (unchanged)
    OfferWithFiles,     // no upscaler call: pick one, or use Optical F5Low
    OfferNoFiles,       // no upscaler files: Optical F5Low can still run NR
    F5LowNrAndFrameGen, // Optical F5Low's virtual upscaler is running, with NR and frame generation
    F5LowNrNoFrameGen,  // ... with NR, frame generation off
    F5LowFrameGenNoNr,  // ... with frame generation, NR off
    F5LowStabiliser,    // ... with neither: only the upscaler runs
    F5LowNrOnly,        // Optical F5Low runs NR on the finished picture
    F5LowStandsAside,   // a mode is on, the game calls an upscaler of its own
    F5LowStatus         // a mode is on and not yet running: the driver's own status text
};

enum class Action
{
    None,
    UseF5Low,     // opens the Optical F5Low page
    F5LowSettings // opens the Optical F5Low page
};

struct Banner
{
    Line line = Line::Blank;
    Action action = Action::None;
};

inline Banner Decide(const Inputs& in)
{
    using DlssNrNativeMode::Shown;

    // A game's own feature decides first; Optical F5Low has nothing to add to it.
    if (in.feature == Feature::Frozen)
        return { Line::Frozen, Action::None };

    if (in.feature == Feature::Game)
        return { Line::Blank, Action::None };

    // The virtual upscaler runs whatever NR and frame generation do, so the line says which of them run with it.
    if (in.feature == Feature::F5Low)
    {
        const Line line = in.nrEnabled ? (in.frameGeneration ? Line::F5LowNrAndFrameGen : Line::F5LowNrNoFrameGen)
                                       : (in.frameGeneration ? Line::F5LowFrameGenNoNr : Line::F5LowStabiliser);
        return { line, Action::F5LowSettings };
    }

    // No feature at all.
    if (in.mode != Shown::Off)
    {
        if (in.gameCallsUpscaler)
            return { Line::F5LowStandsAside, Action::None };

        if (in.mode == Shown::NrOnly && in.f5lowNrOnlyRunning)
            return { Line::F5LowNrOnly, Action::F5LowSettings };

        return { Line::F5LowStatus, Action::F5LowSettings };
    }

    // Mode off: offer Optical F5Low only where it can help. While the game calls an upscaler, or NR cannot run, the old
    // lines stand.
    const Line today = in.upscalerFiles ? Line::SelectUpscaler : Line::NoFiles;

    if (in.gameCallsUpscaler || !in.nrAvailable)
        return { today, Action::None };

    return { in.upscalerFiles ? Line::OfferWithFiles : Line::OfferNoFiles, Action::UseF5Low };
}

// The one-time notice ([Menu] F5LowHint) comes after this much real time with the header's offer standing: long
// enough that a game with an upscaler is past its title screen and menus, where it makes no upscaler call either.
inline constexpr double kHintQuietMs = 60000.0;
// Per present at most this much counts, so a loading stall adds little.
inline constexpr double kHintStepCapMs = 100.0;

// The offer's standing time after one more present `stepMs` after the last; 0 when the offer is gone.
inline double HintQuietAfter(double quietMs, bool offerStands, double stepMs)
{
    if (!offerStands)
        return 0.0;

    if (stepMs < 0.0)
        stepMs = 0.0;

    return quietMs + (stepMs < kHintStepCapMs ? stepMs : kHintStepCapMs);
}

inline bool HintDue(double quietMs) { return quietMs >= kHintQuietMs; }
} // namespace HeaderBanner
