#pragma once

// The depth-tolerance default used by detail reuse's trust test (DlssNr_DetailReuseConstants.h) for a game
// measured to need more than the shipped default, while the user has not set DetailReuseDepthTolerance by hand.
//
// The shipped default (0.051, see DlssNr_DetailReuseConstants.h) was raised once already, from a Witcher 3
// measurement, on the premise that every game has the same problem to the same degree -- it does not. RDR2's own
// flicker was reported at the same time but never measured, and turned out to need far more: clean at 0.263,
// roughly 5x the global default, and above even the Witcher's own confirmed-clean ceiling of 0.25 (user,
// 2026-10-02). Raising the global default a second time on RDR2's number alone would repeat exactly that
// mistake -- generalising one game's measurement to every game, including ones (NBA 2K27) never pushed anywhere
// near this value. So this is a per-game default, not a second global one.
//
// Header-only and free of D3D/Vulkan types so it can be exercised on the host (a test mirrors
// tests/nr_auto_trim_smoke.cpp's shape).

#include <optional>
#include <string>

#include "DlssNr_ExeMatch.h"

namespace DlssNrReuseDepth
{
// Measured in-game (user, 2026-10-02): RDR2's distant-water flicker is clean at 0.263; the shipped global default
// (0.051) does not fix it there. Only RDR2 is listed -- raise a game onto this list with a measurement behind it,
// the same rule DlssNr_AutoTrimDefault.h's list already follows.
constexpr float kRdr2DepthTolerance = 0.263f;

constexpr const char* kRelaxedDepthGames[] = {
    "rdr2.exe",     // Red Dead Redemption 2: distant water flicker clean at 0.263 (2026-10-02)
    "playrdr2.exe", // its launcher-started exe
};

// Whether the game's exe (name or full path, any case) is on kRelaxedDepthGames.
inline bool IsKnownRelaxedDepthGame(const std::string& exe)
{
    return DlssNrExeMatch::IsKnownGame(exe, kRelaxedDepthGames);
}

// The depth tolerance in force: the user's own when they set one, otherwise the shipped default unless this is a
// known relaxed game, which gets its own measured value instead.
inline float Effective(const std::optional<float>& userValue, bool knownRelaxed, float shippedDefault)
{
    if (userValue.has_value())
        return *userValue;
    return knownRelaxed ? kRdr2DepthTolerance : shippedDefault;
}
} // namespace DlssNrReuseDepth
