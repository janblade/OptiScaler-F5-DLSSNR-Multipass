#pragma once

// Moving the white point between Automatic's own and Auto's found exposure (DlssNr_ExposureScan.h FoundExposure), on
// the host, in stops, over a second -- both ways. One helper for the D3D12 and Vulkan passes.
//
// Both ways because the found exposure goes away in ordinary play: a loading screen destroys the game's buffer, a
// feature recreate (resolution or quality change) lets go of every candidate, the scan searches again. Easing only the
// takeover stepped the picture back to Automatic in one frame, by as much as the takeover had eased away (0.84 EV in
// Cyberpunk 2077).
//
// While it gives a base (Step > 0) the host's white point is in force: the shader is not asked to recompute it from the
// meter's live texture. Automatic's own value (`own`) is the CPU readback, a few frames behind, and is 0 while the meter
// starts over (a change of source does that): taking over then waits for it, and leaving holds where the picture is,
// for up to kHoldSeconds, instead of falling to the paper-white slider for the frames it takes to arrive.
//
// Header-only: tests/nr_exposure_found_blend_smoke.cpp.

#include <algorithm>
#include <cmath>

namespace DlssNrExposureFoundBlend
{
inline constexpr double kSeconds = 1.0;     // the ease, both ways
inline constexpr double kHoldSeconds = 2.0; // leaving without a reading of Automatic's own, at most

inline bool Valid(float base) { return std::isfinite(base) && base > 1e-8f && base < 1e8f; }

// What a Step changed, for the log
enum class Event
{
    None,
    TookOver, // the found exposure starts moving the white point (from Automatic's own, or back from leaving)
    Leaving,  // the found exposure went away: easing back to Automatic's own
    Left      // Automatic's own is in force again (the shader's live value)
};

class Blend
{
  public:
    // `target`: the found exposure's base white point this frame (pre-exposure / found), 0 when there is none.
    // `own`: Automatic's own base white point from the readback, 0 when it has no reading. `now`: seconds.
    // Returns the base white point the host pins, or 0: none, Automatic's own (the shader's live value) is in force.
    float Step(float target, float own, double now)
    {
        _event = Event::None;
        const bool ownOk = Valid(own);

        if (Valid(target))
        {
            if (_shown <= 0.0f)
            {
                // Waits for Automatic's reading rather than stepping from nothing: the picture shows Automatic now
                if (!ownOk)
                    return 0.0f;

                Begin(own, now);
                _event = Event::TookOver;
            }
            else if (_leaving)
            {
                Begin(_shown, now);
                _event = Event::TookOver;
            }

            _shown = Lerp(_from, target, Progress(now));
            return _shown;
        }

        if (_shown <= 0.0f)
            return 0.0f;

        if (!_leaving)
        {
            Begin(_shown, now);
            _leaving = true;
            _leftAt = now;
            _event = Event::Leaving;
        }

        if (!ownOk)
        {
            if (now - _leftAt < kHoldSeconds)
            {
                _start = now; // the ease starts when Automatic's reading does
                return _shown;
            }

            return Finish();
        }

        const double t = Progress(now);

        if (t >= 1.0)
            return Finish();

        _shown = Lerp(_from, own, t);
        return _shown;
    }

    Event LastEvent() const { return _event; }
    float Shown() const { return _shown; }

    void Reset() { *this = Blend {}; }

  private:
    void Begin(float from, double now)
    {
        _from = from;
        _start = now;
        _leaving = false;
    }

    float Finish()
    {
        _shown = 0.0f;
        _leaving = false;
        _event = Event::Left;
        return 0.0f;
    }

    double Progress(double now) const { return std::clamp((now - _start) / kSeconds, 0.0, 1.0); }

    static float Lerp(float from, float to, double t)
    {
        return std::exp2(std::lerp(std::log2(from), std::log2(to), (float) t));
    }

    float _shown = 0.0f;
    float _from = 0.0f;
    double _start = 0.0;
    double _leftAt = 0.0;
    bool _leaving = false;
    Event _event = Event::None;
};
} // namespace DlssNrExposureFoundBlend
