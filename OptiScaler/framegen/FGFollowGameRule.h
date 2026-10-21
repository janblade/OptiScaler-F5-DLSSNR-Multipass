#pragma once

#include <algorithm>

// With FG Input DLSSG and Frame Generation Enabled left on auto, OptiScaler's FG follows the game's own DLSS
// Frame Generation setting: on/off and the frame multiplier the game sends with slDLSSGSetOptions.
// Pure rules (no OptiScaler state), host-tested in tests/fg_follow_game_smoke.cpp.
// Idea from upstream OptiScaler 13db0069, rebuilt on our seams.
namespace FGFollowGame
{
// Highest frame count a game request is taken at; the output's own max clamps it further
constexpr int kMaxGameCount = 6;

// What the game asked for. Off is off; on, auto and dynamic are on. A count outside 1..6 is clamped (1 = 2x).
struct Request
{
    bool enabled = false;
    int count = 1;

    constexpr bool operator==(const Request&) const = default;
};

constexpr Request FromGame(bool modeOff, unsigned int numFramesToGenerate)
{
    const int count = numFramesToGenerate > static_cast<unsigned int>(kMaxGameCount)
                          ? kMaxGameCount
                          : std::max(static_cast<int>(numFramesToGenerate), 1);
    return { !modeOff, count };
}

// Streamline 1 has no slDLSSGSetOptions, so the game's choice is never seen there
constexpr bool CanFollow(bool dlssgInput, int streamlineMajor) { return dlssgInput && streamlineMajor >= 2; }

// Only an unset Enabled (auto) follows. A value from the ini, the menu, the hotkey, a failed precheck or
// External frame generation wins over the game.
constexpr bool Follows(bool enabledSet, bool canFollow) { return !enabledSet && canFollow; }

constexpr bool Enabled(bool follows, Request game, bool menuEnabled) { return follows ? game.enabled : menuEnabled; }

// Following the game must not start XeFG where XeFG's own checks refuse it (the menu runs those only while it is
// drawn): exclusive full screen, or a float (scRGB) swapchain, since XeFG only takes HDR10. Ignore Init Checks lifts
// it, as in the menu. Other outputs are not concerned.
constexpr bool FollowBlocked(bool xefgOutput, bool ignoreChecks, bool exclusiveFullscreen, bool floatSwapchain)
{
    return xefgOutput && !ignoreChecks && (exclusiveFullscreen || floatSwapchain);
}

// Frames to generate per real frame: the game's count when following, else the output's menu count (XeFG or
// DLSSG InterpolationCount). Never above the output's max; a max not known yet (below 1) allows 1.
// The game-side "Override DLSSG Ratio" is not part of this: it applies to the game's own DLSS-FG only.
constexpr int Count(bool follows, Request game, int menuCount, int outputMax)
{
    return std::clamp(follows ? game.count : menuCount, 1, std::max(outputMax, 1));
}

// numFramesToGenerateMax told to the game: the running output's max (0 before it exists). At least 1, so a game
// always offers 2x.
constexpr int ReportedMax(int outputMax) { return std::max(outputMax, 1); }

// numFramesActuallyPresented: the real frame plus the generated ones while OptiScaler's FG runs
constexpr int Presented(bool fgRunning, int generated) { return fgRunning ? std::max(generated, 0) + 1 : 1; }

// A new game request needs the FG to be (re)evaluated: it turned FG on, or changed the count of an output
// that only takes a new count on recreate (XeFG). Only while following; otherwise it is just recorded.
constexpr bool NeedsFGChange(bool follows, Request previous, Request next, bool countNeedsRecreate)
{
    if (!follows || !next.enabled)
        return false;

    return !previous.enabled || (countNeedsRecreate && previous.count != next.count);
}
} // namespace FGFollowGame
