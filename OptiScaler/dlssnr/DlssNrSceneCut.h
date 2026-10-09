#pragma once

// Scene cuts the game does not flag, on DLSS-NR's game input: what to make of a cut the detector found
// (motion/SceneCut_Dx12.h, run by shaders/dlssnr/DlssNr_SceneCut.inl). No D3D here, so a host test includes it.
//
// The detector's answer arrives a few evaluates after the frame it judged (a readback, read once the GPU is past it).
// By then the game may have flagged a reset itself: a cut it flagged on the frame before, on, or after the cut frame
// counts as flagged by the game, and is not acted on. One it did not flag resets the model, in the Act mode only, and
// at most once in kQuiet evaluates (a white flash is two cuts: into the white and back out).

#include <cstdint>

namespace DlssNrSceneCut
{
// [DlssNr] SceneCut. Count is the default: the detector runs and the log and the menu count what it finds, while the
// picture is left as it is.
enum class Mode : uint32_t
{
    Off = 0,
    Count = 1,
    Act = 2,
};

constexpr Mode ModeFrom(uint32_t setting) { return setting == 0 ? Mode::Off : setting >= 2 ? Mode::Act : Mode::Count; }

constexpr unsigned long long kQuiet = 6; // evaluates after a reset of ours in which no other is made

// The divergence past which a frame is a cut: Optical F5Low's (OpticalFlowDx12::Settings::sceneCutThreshold).
constexpr float kThreshold = 0.45f;

struct Outcome
{
    bool gameFlagged = false; // the game reset within a frame of the cut
    bool reset = false;       // reset the model now
};

class Policy
{
  public:
    // The game asked for a reset on this evaluate.
    void GameReset(unsigned long long evaluate) { _gameResets[_next++ % kKept] = evaluate + 1; }

    // The detector found a cut on evaluate `cut`; this is evaluate `now`.
    Outcome Found(unsigned long long cut, unsigned long long now, Mode mode)
    {
        Outcome o;
        o.gameFlagged = GameResetBetween(cut == 0 ? 0 : cut - 1, cut + 1);
        ++found;
        ++(o.gameFlagged ? flagged : silent);

        // Not when the game reset since the cut anyway, nor right after a reset of ours.
        if (mode == Mode::Act && !o.gameFlagged && !GameResetBetween(cut, now) && now >= _quietUntil)
        {
            o.reset = true;
            ++resets;
            _quietUntil = now + kQuiet;
        }

        return o;
    }

    void Forget()
    {
        for (unsigned long long& r : _gameResets)
            r = 0;
        _quietUntil = 0;
    }

    unsigned long long found = 0;   // cuts the detector found
    unsigned long long flagged = 0; // ... that the game flagged too
    unsigned long long silent = 0;  // ... that it did not
    unsigned long long resets = 0;  // model resets made for them (Act)

  private:
    static constexpr unsigned kKept = 16;

    bool GameResetBetween(unsigned long long from, unsigned long long to) const
    {
        for (const unsigned long long r : _gameResets)
            if (r != 0 && r - 1 >= from && r - 1 <= to)
                return true;
        return false;
    }

    unsigned long long _gameResets[kKept] = {}; // evaluate + 1 of the latest game resets; 0 = none
    unsigned _next = 0;
    unsigned long long _quietUntil = 0;
};
} // namespace DlssNrSceneCut
