#pragma once

#include <cstdint>

// Which frame generation output OptiScaler makes (State::activeFgOutput). Its own header, so the pure HDR output rules
// (output/SdrToHdr_Create.h) can map it on the host.
enum class FGOutput : uint32_t
{
    NoFG,
    FSRFG,
    DLSSG,
    XeFG,
};

// The outputs whose game-facing swap chain gets the full screen answers of FGXeFGForceBorderless
// (FGHooks::hkGetFullscreenState and hkGetFullscreenDesc): the game is told full screen while the chain is windowed.
// XeFG's chain forwards to the real chain or answers itself; FSR FG's (FrameInterpolationSwapChainDX12) forwards both
// calls to the real chain, which is windowed. So FGXeFGForceBorderless governs FSR FG's full screen answers too.
inline bool FgAnswersFullscreenForGame(FGOutput output)
{
    return output == FGOutput::XeFG || output == FGOutput::FSRFG;
}
