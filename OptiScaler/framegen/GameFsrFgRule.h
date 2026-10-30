#pragma once

// XeFG on a swapchain the game makes through its own FSR Frame Generation library (amd_fidelityfx_framegeneration
// _dx12.dll). Some games always make it there, also while the player picked DLSS-G. Both generating crashes (Modern
// Warfare 4), so XeFG takes such a swapchain only when the game's FSR FG is known to be off: FG Input DLSSG (the
// game drives DLSS-G, not its FSR FG) and the game's FFX FG calls pass OptiScaler's hooks, which see it turn on.
// Pure rules (no OptiScaler state), host-tested in tests/game_fsrfg_rule_smoke.cpp.
namespace GameFsrFg
{
enum class Swapchain
{
    NotUnder, // XeFG is not the output, or the game's FSR FG library did not make it
    Take,     // XeFG takes it and stands aside while the game's FSR FG is on
    Refuse,   // left to the game
};

// seen: the game's FFX FG calls pass OptiScaler's hooks (FFX inputs on), so its FSR FG turning on is noticed
constexpr Swapchain Decide(bool xefgOutput, bool createdByGameFsrFg, bool dlssgInput, bool seen, bool gameFgOn)
{
    if (!xefgOutput || !createdByGameFsrFg)
        return Swapchain::NotUnder;

    return dlssgInput && seen && !gameFgOn ? Swapchain::Take : Swapchain::Refuse;
}

// XeFG under the game's FSR FG swapchain does not generate while the game's FSR FG is on
constexpr bool StandsAside(bool underGameFsrFg, bool gameFgOn) { return underGameFsrFg && gameFgOn; }
} // namespace GameFsrFg
