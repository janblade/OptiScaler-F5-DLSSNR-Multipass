#pragma once

// [DlssNr] MotionSource=optical on a native D3D12 game that calls its own upscaler: the game's upscaler keeps running
// and Neural Rendering at the Finished Picture takes Optical F5Low's flow (measured on the finished picture by the
// native producer, NativeDriver) instead of the game's motion vectors; the depth stays the game's. These are the rules
// the two halves share, free of D3D12 so a host test can check them (tests/nr_optical_motion_smoke.cpp):
//
// - The producer runs before the Finished Picture pass of the same present, on the same queue
//   (wrapped_swapchain.cpp, FG_Hooks.cpp): it offers its flow for that present (the picture, the queue, the present
//   count) and the pass takes it only for exactly that one. Any other present (the producer skipped it, another
//   queue, a Streamline application picture, a size change) falls back to the game's motion.
// - NR's history must not reach across a change of motion source: the first picture on the other source starts over.

#include <cstdint>

namespace DlssNr::OpticalMotion
{

// Where the motion goes: Finished Picture with the model on the finished picture itself. "Run the model before Super
// Resolution" (or DeferredDLSS) needs frame N's flow before frame N's finished picture exists, so it keeps the game's.
struct Keys
{
    bool optical = false;         // [DlssNr] MotionSource=optical
    bool nrEnabled = false;       // [DlssNr] Enabled
    bool finishedPicture = false; // [DlssNr] FinishedPicture
    bool modelBeforeSr = false;   // [DlssNr] RunBeforeSR or DeferredDLSS
};

inline bool Wanted(const Keys& keys)
{
    return keys.optical && keys.nrEnabled && keys.finishedPicture && !keys.modelBeforeSr;
}

// What a present is: compared by identity only (never dereferenced here).
struct Present
{
    const void* queue = nullptr;
    const void* picture = nullptr;
    uint64_t epoch = 0; // State::frameCount (native::PresentEpoch for D3D12)
    uint32_t width = 0, height = 0;
};

inline bool SamePresent(const Present& offered, const Present& wanted)
{
    return offered.queue != nullptr && offered.picture != nullptr && offered.queue == wanted.queue &&
           offered.picture == wanted.picture && offered.epoch == wanted.epoch && offered.width == wanted.width &&
           offered.height == wanted.height;
}

struct Choice
{
    bool optical = false; // this present's NR takes the flow
    bool reset = false;   // the player switched the motion source: NR's history starts over
};

// `lastWanted` is the setting the last picture was made under (kept by the caller, updated here). Only a switch of the
// setting restarts NR's history. A present without a flow (the game skipped its upscaler for a frame, a frame
// generation pause; NBA 2K27's menus, 2026-11-28) takes the game's motion for that one picture and keeps the history:
// both describe the same movement, and a restart out and back again blinked the detail. A scene cut or queue change the
// producer saw is carried to the next picture that takes the flow (PublishOpticalMotion's `reset`); a gap without
// Neural Rendering is FinishedHistoryGap's.
inline Choice Choose(bool wanted, bool flowForThisPresent, bool& lastWanted)
{
    Choice choice;
    choice.optical = wanted && flowForThisPresent;
    choice.reset = wanted != lastWanted;
    lastWanted = wanted;
    return choice;
}

// The flow measures with the game's depth from the same upscaler call the Finished Picture pass copied it from (the
// copy keeps the game's allocation and its subrect where the game had it). The producer's depth alignment
// (native/DepthAlign_Dx12.h: depth uv = picture uv / scale + (-offsetX / width, offsetY / height)) maps the picture
// onto that subrect: scale = allocation / subrect, offsets = the subrect's corner in the picture's pixels. An empty
// subrect (the call gave none) is the whole allocation: the identity.
struct DepthRect
{
    uint32_t allocationWidth = 0, allocationHeight = 0; // the copy's size
    uint32_t x = 0, y = 0, width = 0, height = 0;       // where the game's call drew in it (the render subrect)
};

inline void DepthTransformForRect(const DepthRect& rect, uint32_t pictureWidth, uint32_t pictureHeight,
                                  float transform[4])
{
    transform[0] = 1.0f;
    transform[1] = 1.0f;
    transform[2] = 0.0f;
    transform[3] = 0.0f;

    // Inside the allocation only (a call may give a subrect that runs past its edge)
    if (rect.x >= rect.allocationWidth || rect.y >= rect.allocationHeight || rect.width == 0 || rect.height == 0)
        return;

    const uint32_t width = rect.width < rect.allocationWidth - rect.x ? rect.width : rect.allocationWidth - rect.x;
    const uint32_t height = rect.height < rect.allocationHeight - rect.y ? rect.height : rect.allocationHeight - rect.y;

    transform[0] = (float) rect.allocationWidth / (float) width;
    transform[1] = (float) rect.allocationHeight / (float) height;
    transform[2] = -(float) rect.x / (float) rect.allocationWidth * (float) pictureWidth;
    transform[3] = (float) rect.y / (float) rect.allocationHeight * (float) pictureHeight;
}

// [FrameGen] MotionSource=optical: frame generation takes the same flow instead of the game's motion vectors, a choice
// of its own (NBA 2K27, 2026-11-28: with good game vectors the flow gave a halo around players in generated frames,
// while Neural Rendering on it looked clean). Frame generation fed by OptiScaler's FG swap chain from the game's
// upscaler call (FGInput=upscaler) on a native D3D12 game; framegen/OpticalMotionFg_Dx12.h does the work.
struct FgKeys
{
    bool optical = false;       // [FrameGen] MotionSource=optical
    bool fgEnabled = false;     // frame generation on
    bool upscalerInput = false; // FGInput=upscaler
    // XeFG or F5Low output: they take the flow's conventions (display-size, unjittered, the context's init flags).
    // FSR frame generation and DLSS-G are refused (not checked with the flow).
    bool xefgApiOutput = false;
    bool nativeDx12 = false;   // a native D3D12 game, no D3D11 or Vulkan bridge
    bool resourceFlip = false; // [FrameGen] ResourceFlip flips the game's motion; the flow is never flipped
    // The FG swap chain reads display-size motion right: F5Low always (it takes the size from the tag), XeFG only when
    // it was made for it ([XeFG] HighResMV at its creation, so a switch on needs a restart there)
    bool contextFits = true;
};

enum class FgRefusal
{
    None,
    NotNativeDx12,
    NotUpscalerInput,
    Output,
    ResourceFlip,
    NeedsRestart
};

inline FgRefusal FgRefused(const FgKeys& keys)
{
    return !keys.nativeDx12      ? FgRefusal::NotNativeDx12
           : !keys.upscalerInput ? FgRefusal::NotUpscalerInput
           : !keys.xefgApiOutput ? FgRefusal::Output
           : keys.resourceFlip   ? FgRefusal::ResourceFlip
           : !keys.contextFits   ? FgRefusal::NeedsRestart
                                 : FgRefusal::None;
}

inline bool FgWanted(const FgKeys& keys)
{
    return keys.optical && keys.fgEnabled && FgRefused(keys) == FgRefusal::None;
}

// At the game's upscaler call: this frame's motion for frame generation is the flow. Not for OptiScaler's own virtual
// upscaler call (full Optical F5Low mode feeds the flow through it already), nor with several views packed together.
inline bool FgAtCall(bool wanted, bool virtualUpscalerCall, bool severalViews)
{
    return wanted && !virtualUpscalerCall && !severalViews;
}

enum class FgTag
{
    Nothing,     // the call tagged the game's motion as always
    Flow,        // this present's flow
    GameFallback // no flow for the frame being generated: the game's motion vectors, as the call kept them
};

// At the present, for the frame frame generation dispatches. The flow measures the picture being presented, which is
// the latest call's frame: a dispatch of an older frame (frame generation running behind) takes the game's. A flow of
// another size than the frame generation picture (the game renders at another size than its window) the same.
inline FgTag FgTagAtPresent(bool opticalAtCall, bool dispatchIsLatest, bool flowForThisPresent, bool flowFitsPicture)
{
    if (!opticalAtCall)
        return FgTag::Nothing;
    return dispatchIsLatest && flowForThisPresent && flowFitsPicture ? FgTag::Flow : FgTag::GameFallback;
}

// The fallback's own reset: the context was made for the flow's conventions (display-size, unjittered motion).
// Render-size game vectors would be read at the wrong scale, so frame generation does not interpolate across that
// frame. Jittered ones are off by under a pixel: no reset (NBA 2K27, 2026-11-28: a reset on every fallback frame
// stuttered badly).
inline bool FgFallbackResets(bool gameLowResMv, bool gameJitteredMv)
{
    (void) gameJitteredMv;
    return gameLowResMv;
}

} // namespace DlssNr::OpticalMotion
