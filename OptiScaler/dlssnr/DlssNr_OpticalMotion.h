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
// both describe the same movement, and a restart out and back again blinked the detail. A longer gap restarts anyway,
// through the producer's own reset when the flow comes back (PublishOpticalMotion's `reset`).
inline Choice Choose(bool wanted, bool flowForThisPresent, bool& lastWanted)
{
    Choice choice;
    choice.optical = wanted && flowForThisPresent;
    choice.reset = wanted != lastWanted;
    lastWanted = wanted;
    return choice;
}

} // namespace DlssNr::OpticalMotion
