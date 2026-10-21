#pragma once

// Where DLSS-NR's white point comes from while the user has not chosen ([DlssNr] WhitePointSource and
// GameExposureScale = auto), and the scale Game exposure uses then.
//
// Source: the game's own exposure when the game hands one over, else OptiScaler's Automatic exposure (user decision
// 2026-10-14). "Hands one over" is what the game's upscaler call carries (an exposure texture), latched for the
// session (Offer below): the game's exposure is only read while Game exposure is the source (the backends bind the
// game's texture on no other condition), so the source cannot wait for a reading to choose it. A texture that gives
// no clean reading within kGiveUpFrames frames of being read gives Automatic back for the session. A game that never
// hands one over (RE2 on the Optical F5Low path) stays on Automatic.
//
// Scale: "Game's setting" (the game's PreExposure / exposure) unless the game is measured to look right "as is"
// (Native look, the base games with built-in DLSS-NR feed the model). Measured: NBA 2K27 is far too dark on Game's
// setting (model-input median 0.07-0.32, 2026-09-25) and right as is (-0.4 EV, 2026-10-07); RDR2 is the reverse.
//
// "auto" has its own value (kSourceAuto, kScaleAuto), not a real choice's: the config saves a value equal to the
// default as auto, so a default of Automatic would have turned a player's own choice of Automatic into auto.
// Header-only and free of D3D/Vulkan types (tests/nr_exposure_default_smoke.cpp).

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>

#include "DlssNr_ExeMatch.h"

namespace DlssNrExposureDefault
{
inline constexpr uint32_t kSourcePaperWhite = 0;
inline constexpr uint32_t kSourceGame = 1;
inline constexpr uint32_t kSourceScan = 2;
inline constexpr uint32_t kSourceAutomatic = 3;
inline constexpr uint32_t kSourceAuto = 4; // the default: Game exposure when the game hands one over, else Automatic

inline constexpr uint32_t kScaleGameSetting = 0; // DlssNrGameScale::kWithExposure
inline constexpr uint32_t kScaleAsIs = 1;        // DlssNrGameScale::kAsIs
inline constexpr uint32_t kScaleAuto = 2;        // the default: as is for a known game, else the game's setting

// Games measured to look right with the game's colour as is (Native look), by exe name (lower case). Kept here, as
// DlssNr_AutoTrimDefault.h's list is, rather than as a GameQuirk upstream owns. Add a game only with a measurement.
constexpr const char* kAsIsGames[] = {
    "nba2k27.exe", // NBA 2K27: Game's setting far too dark (2026-09-25), as is -0.4 EV the user's pick (2026-10-07)
};

inline bool IsKnownAsIsGame(const std::string& exe) { return DlssNrExeMatch::IsKnownGame(exe, kAsIsGames); }

// Frames Game exposure may read the game's texture without a clean value before auto gives Automatic back
inline constexpr uint32_t kGiveUpFrames = 240;

// Whether the game hands over a usable exposure, learned frame by frame on the upscaler's thread and read by the
// menu's: atomics.
class Offer
{
  public:
    // `offered`: this frame's upscaler call carries an exposure texture. `reading`: a clean value has been read from
    // it. `reading`'s frames only count while Game exposure is the source (`metering`): nothing reads it otherwise.
    void Update(bool offered, bool reading, bool metering)
    {
        if (offered)
            _offered = true;

        if (reading)
            _read = true;

        if (metering && _offered && !_read && !_unusable && ++_framesWithout >= kGiveUpFrames)
            _unusable = true;
    }

    bool Usable() const { return _offered && !_unusable; }
    bool Unusable() const { return _unusable; }

    // Offered and read clean at least once: what Auto's scan stops for (DlssNr_ExposureAuto.h)
    bool Confirmed() const { return _offered && _read && !_unusable; }

    // Offered, but neither read clean nor given up on yet
    bool Pending() const { return _offered && !_read && !_unusable; }

  private:
    std::atomic<bool> _offered { false };
    std::atomic<bool> _read { false };
    std::atomic<bool> _unusable { false };
    std::atomic<uint32_t> _framesWithout { 0 };
};

// The source in force. `gameExposureUsable`: Offer::Usable().
inline uint32_t Source(uint32_t configured, bool gameExposureUsable)
{
    if (configured == kSourceAuto)
        return gameExposureUsable ? kSourceGame : kSourceAutomatic;

    return std::min(configured, kSourceAutomatic);
}

// The Game exposure scale in force.
inline uint32_t Scale(uint32_t configured, bool knownAsIsGame)
{
    if (configured == kScaleAuto)
        return knownAsIsGame ? kScaleAsIs : kScaleGameSetting;

    return std::min(configured, kScaleAsIs);
}
} // namespace DlssNrExposureDefault
