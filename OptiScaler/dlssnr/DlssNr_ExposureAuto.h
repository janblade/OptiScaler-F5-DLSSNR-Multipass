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
// - Watching: before the first NR frame, and while a handed texture has not yet read clean (the scan keeps what it
//   holds: Game exposure may still give up on the texture, and the game made its candidates at load).
// - Done: a texture or a moving pre-exposure is handed over, so the scan is not needed: every candidate is let go at
//   once. Pinning the game's resources past its own teardown removed the device in Cyberpunk 2077, so letting go early
//   is safety, not tidiness.
// - NotHere: a game whose upscaler is not D3D12. Auto's scan runs on D3D12 only for now (stories 3 and 4 find the
//   candidates on D3D11 and Vulkan; using one there is not checked in a game yet). Every candidate let go.
// - Searching: nothing handed: copy every candidate each frame until the matcher's verdict.
// - Found: copy only the match, let go of the rest. Only the match: a ping-pong partner the game keeps (Cyberpunk's
//   c61/c62) is let go too; reading one image of the pair is at most a frame late, and a lag of a row is eased.
// - Baked: let go of every candidate.
// - GaveUp: no verdict, or none found, within searchSeconds: let go of every candidate, Automatic. Candidates the game
//   makes after that (a loading screen, a recreated feature) are held, not copied, and start a new search once
//   retrySeconds have passed, doubled with every search given up: a game that keeps making small buffers (Starfield
//   remakes its frame generation resources hundreds of times) must not keep the scan copying for the whole session.
// The backend lets go when a phase is entered; candidates taken after that, in Found or GaveUp, are held for the next
// search without being copied. A match that goes away (the game destroyed it, or a feature release let go of it)
// searches again: nothing is kept by candidate number from one session or one search to the next.
//
// Header-only, no D3D or Vulkan types: tests/nr_exposure_auto_smoke.cpp.

#include <algorithm>
#include <cmath>
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
    GaveUp,
    NotHere
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
    float retrySeconds = 60.0f;   // after giving up, before new candidates start a search; doubled each time
    float maxRetrySeconds = 1800.0f;
};

// One NR frame's news, from the backend that runs NR. `handed` and `preExposure` are latched for the session by the
// caller (Offer::Usable is): a frame without them after frames with them means the texture was found unusable.
struct Frame
{
    float seconds = 0.0f;         // NR time since the session started
    bool handed = false;          // an exposure texture is handed over and has read clean (Offer::Confirmed)
    bool handedPending = false;   // one is handed over, not yet read clean nor given up on
    bool preExposure = false;     // the call's pre-exposure moves with the light (PreExposureMotion)
    bool supported = true;        // the game's upscaler is D3D12
    Verdict verdict = Verdict::NotSure;
    bool matchAlive = true;       // the found candidate is still there (only asked while Found)
    bool newCandidates = false;   // the scan took a likely candidate since the last frame
};

class Lifecycle
{
  public:
    explicit Lifecycle(Settings s = {}) : _s(s) {}

    void OnFrame(const Frame& f)
    {
        if (!f.supported)
        {
            _phase = Phase::NotHere;
            return;
        }

        if (f.handed || f.preExposure)
        {
            _phase = Phase::Done;
            return;
        }

        switch (_phase)
        {
        case Phase::Watching:
            if (!f.handedPending)
                Search(f.seconds);
            break;

        case Phase::Done: // what was handed stopped being usable: search what the game makes from here
        case Phase::NotHere:
            Search(f.seconds);
            break;

        case Phase::Searching:
            if (f.verdict == Verdict::Found)
                _phase = Phase::Found;
            else if (f.verdict == Verdict::Baked)
                _phase = Phase::Baked;
            else if (f.seconds - _since >= _s.searchSeconds)
                GiveUp(f.seconds);
            break;

        case Phase::Found:
            if (!f.matchAlive)
                Search(f.seconds);
            break;

        case Phase::GaveUp:
            _newSince = _newSince || f.newCandidates;
            if (_newSince && f.seconds - _since >= _retry)
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

    // On entering this phase every candidate is let go of (in Found, all but the match: CopiesMatchOnly)
    bool LetsGo() const
    {
        return _phase == Phase::Done || _phase == Phase::Baked || _phase == Phase::GaveUp || _phase == Phase::NotHere;
    }

    // No candidate is taken any more (the backend's adopt): the scan is not needed while this holds
    bool Closed() const { return _phase == Phase::Done || _phase == Phase::Baked || _phase == Phase::NotHere; }

    // How long the current give-up waits before new candidates start a search
    float RetrySeconds() const { return _retry; }

    // A new search starts from no history: the backend clears what the matcher reads when this changes
    unsigned int SearchNumber() const { return _searches; }

  private:
    void Search(float seconds)
    {
        _phase = Phase::Searching;
        _since = seconds;
        ++_searches;
    }

    void GiveUp(float seconds)
    {
        _phase = Phase::GaveUp;
        _since = seconds;
        _newSince = false;
        _retry = _givenUp++ == 0 ? _s.retrySeconds : std::min(_retry * 2.0f, _s.maxRetrySeconds);
    }

    Settings _s;
    Phase _phase = Phase::Watching;
    float _since = 0.0f;
    unsigned int _searches = 0;
    unsigned int _givenUp = 0;
    float _retry = 0.0f;
    bool _newSince = false;
};

// Whether the upscaler call's pre-exposure moves with the light (audit HC16: Dawnwalker, UE5, 1.0 -> 0.59 -> 0.45 ->
// 0.40 within a second), from one value a frame. Moving: it changed by more than 1% at least kChanges times, over at
// least kLevels distinct values (0.05 EV apart), spanning at least a quarter. A game that switches between a few fixed
// values (gameplay and a cutscene, a photo mode) does not move with the light, and stopping Auto's search for the session
// on it would cost the scan its answer. Latched for the session once moving.
class PreExposureMotion
{
  public:
    static constexpr unsigned int kChanges = 10;
    static constexpr size_t kLevels = 6;

    void Note(float pre)
    {
        if (_moving || !std::isfinite(pre) || pre <= 0.0f)
            return;

        if (_last > 0.0f && std::fabs(std::log2(pre / _last)) > 0.0144f)
            ++_changes;

        const int level = (int) std::lround(std::log2(pre) * 20.0f);

        if (_levels < kLevels && std::find(_seen, _seen + _levels, level) == _seen + _levels)
            _seen[_levels++] = level;

        _last = pre;
        _low = _low > 0.0f ? std::min(_low, pre) : pre;
        _high = std::max(_high, pre);
        _moving = _changes >= kChanges && _levels >= kLevels && _high >= _low * 1.25f;
    }

    bool Moving() const { return _moving; }

  private:
    float _last = 0.0f, _low = 0.0f, _high = 0.0f;
    unsigned int _changes = 0;
    int _seen[kLevels] = {};
    size_t _levels = 0;
    bool _moving = false;
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
