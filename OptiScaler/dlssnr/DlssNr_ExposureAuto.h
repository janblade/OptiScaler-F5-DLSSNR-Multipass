#pragma once

// Where Auto's exposure comes from, and what the exposure scan does meanwhile (plan 2026-10-14-exposure-scan-all-apis,
// story 5). The source ladder, first that holds:
// 1. Handed: the game's upscaler call carries an exposure texture that reads clean (DlssNrExposureDefault::Offer).
// 2. PreExposure: no texture, but the call's pre-exposure moves with the light (Dawnwalker, UE5): the game's own
//    exposure, carried on the call.
// 3. Found: the scan's matcher found the game's exposure among its candidates (DlssNr_ExposureMatch.h). Automatic gives
//    the level, measured while the scan searched; the game's value gives the movement (user decision 2026-10-20: no
//    meter lag, no HUD or bloom in it, the game's own adaptation range and timing).
// 4. Baked: the game already exposed the colour (our meter flat while a candidate swings): nothing more is applied.
// 5. Automatic: OptiScaler's own meter, as before story 5 when nothing holds.
//
// The scan's lifecycle on Auto (user-agreed 2026-10-19). Candidates are caught only when the game makes them, mostly at
// load and before the first upscaler call tells whether a texture is handed, so the scan watches from launch:
// - Watching: before the first NR frame.
// - Done: a texture or a moving pre-exposure is handed over, so the scan is not needed: every candidate is let go at
//   once. Pinning the game's resources past its own teardown removed the device in Cyberpunk 2077, so letting go early
//   is safety, not tidiness.
// - Searching: nothing handed: copy every candidate each frame until the matcher's verdict.
// - Found: keep only the match (and the copies of it the game keeps), let go of the rest, read one value a frame.
// - Baked: let go of every candidate.
// - GaveUp: no verdict, or none found, within searchSeconds: let go of every candidate, Automatic. Candidates the game
//   makes after that (a loading screen, a recreated feature) start a new search.
// A match that goes away (the game destroyed it, or a feature release let go of it) searches again: nothing is kept by
// candidate number from one session or one search to the next.
//
// Header-only, no D3D or Vulkan types: tests/nr_exposure_auto_smoke.cpp.

#include <cstdint>

namespace DlssNrExposureAuto
{
enum class Rung
{
    Handed,
    PreExposure,
    Found,
    Baked,
    Automatic
};

enum class Phase
{
    Watching,
    Done,
    Searching,
    Found,
    Baked,
    GaveUp
};

// What the matcher said last (DlssNrExposureMatch::What, kept apart so this header needs no matcher types)
enum class Verdict
{
    NotSure,
    Found,
    Baked,
    None
};

struct Settings
{
    float searchSeconds = 300.0f; // NR time without a verdict, or with None, before giving up
};

// One NR frame's news, from the backend that runs NR. `handed` and `preExposure` are latched for the session by the
// caller (Offer::Usable is): a frame without them after frames with them means the texture was found unusable.
struct Frame
{
    float seconds = 0.0f;         // NR time since the session started
    bool handed = false;          // a usable exposure texture is handed over (Offer::Usable)
    bool preExposure = false;     // the call's pre-exposure moves with the light
    Verdict verdict = Verdict::NotSure;
    bool matchAlive = true;       // the found candidate is still there (only asked while Found)
    bool newCandidates = false;   // the scan took a candidate since the last frame
};

class Lifecycle
{
  public:
    explicit Lifecycle(Settings s = {}) : _s(s) {}

    void OnFrame(const Frame& f)
    {
        if (f.handed || f.preExposure)
        {
            _phase = Phase::Done;
            return;
        }

        switch (_phase)
        {
        case Phase::Watching:
        case Phase::Done: // what was handed stopped being usable: search what the game makes from here
            Search(f.seconds);
            break;

        case Phase::Searching:
            if (f.verdict == Verdict::Found)
                _phase = Phase::Found;
            else if (f.verdict == Verdict::Baked)
                _phase = Phase::Baked;
            else if (f.seconds - _since >= _s.searchSeconds)
                _phase = Phase::GaveUp;
            break;

        case Phase::Found:
            if (!f.matchAlive)
                Search(f.seconds);
            break;

        case Phase::GaveUp:
            if (f.newCandidates)
                Search(f.seconds);
            break;

        case Phase::Baked:
            break;
        }
    }

    Phase Now() const { return _phase; }

    // The scan copies every candidate (search) / only the match / nothing
    bool CopiesAll() const { return _phase == Phase::Watching || _phase == Phase::Searching; }
    bool CopiesMatchOnly() const { return _phase == Phase::Found; }

    // Every candidate is let go of (the match too, outside Found)
    bool LetsGo() const { return _phase == Phase::Done || _phase == Phase::Baked || _phase == Phase::GaveUp; }

    // A new search starts from no history: the backend clears what the matcher reads when this changes
    unsigned int SearchNumber() const { return _searches; }

  private:
    void Search(float seconds)
    {
        _phase = Phase::Searching;
        _since = seconds;
        ++_searches;
    }

    Settings _s;
    Phase _phase = Phase::Watching;
    float _since = 0.0f;
    unsigned int _searches = 0;
};

// The rung in force for Auto. `handed` and `preExposure` as in Frame; the lifecycle gives the scan's answer.
inline Rung Ladder(bool handed, bool preExposure, Phase phase)
{
    if (handed)
        return Rung::Handed;
    if (preExposure)
        return Rung::PreExposure;
    if (phase == Phase::Found)
        return Rung::Found;
    if (phase == Phase::Baked)
        return Rung::Baked;
    return Rung::Automatic;
}
} // namespace DlssNrExposureAuto
