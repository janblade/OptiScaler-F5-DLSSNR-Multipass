#pragma once

#include <cmath>

// Reuse detail between frames: run the model on one frame, and on the next add the saved detail of that frame
// (model answer minus model input), moved with the motion vectors, to the new input instead of running
// the model again. The method is reverse reprojection caching (Nehab et al. 2007) with TAA-style history
// rejection (see dlssnr_detail_reuse.hlsl).
//
// This header is the cadence only: which frame runs the model, which reuses, and what each has to record.
// It has no GPU or game dependency so tests/nr_detail_reuse_smoke.cpp can check it on the host.
//
// Every frame that is processed saves its detail (captureHistory), reuse frames included, so the saved history
// is always one frame old when the next frame reads it:
//   - a reuse frame moves it onto its input;
//   - a full frame may pull the model's new detail toward it (steadiness), so full and reuse frames next to each
//     other differ less.
// The model keeps its own history (its previous output). After a reuse frame that history is two frames old,
// so the next full frame hands the model motion composed over both frames (composeMotion), unless the model
// starts over anyway (reset).

namespace DlssNrDetailReuse
{
enum class Kind
{
    Full,  // run the model
    Reuse, // skip the model; input + reprojected saved detail
};

// What the caller knows about this frame.
struct FrameFacts
{
    bool enabled = false;            // option on and this route supports it
    bool reset = false;              // the model starts over this frame (camera cut, new feature, resize)
    bool blocked = false;            // a frame that must see the real model: calibration, frame hold
    unsigned long long frame = 0;    // frame number; a step above maxStep means NR did not run in between
    unsigned long long maxStep = 1;  // the largest step between two NR frames that is not a gap (frame generation
                                     // presents in between, so a present counter steps by more than one)
    unsigned long long revision = 0; // changes whenever anything that changes the model's answer changes
};

struct Decision
{
    Kind kind = Kind::Full;
    bool historyUsable = false;  // the saved detail is from the previous frame and may be reprojected
    bool captureHistory = false; // save answer - input, the input colour and the depth for the next frame
    bool composeMotion = false;  // full frame: give the model motion composed over the last two frames
    bool saveMotion = false;     // reuse frame: keep this frame's motion for the next full frame to compose
};

class Cadence
{
    bool historyValid = false; // the previous frame's detail is saved
    bool pending = false;      // Next asked for a capture; Captured has not confirmed it yet
    bool lastWasReuse = false;
    bool lastBlocked = false; // the history was saved on a blocked frame (a held frame saves the frozen picture)
    bool hasLast = false;
    unsigned long long lastFrame = 0;
    unsigned long long lastRevision = 0;
    unsigned long long full = 0, reused = 0, fallback = 0;

public:
    Decision Next(const FrameFacts& f)
    {
        Decision d;

        if (!f.enabled)
        {
            historyValid = false;
            pending = false;
            lastWasReuse = false;
            lastBlocked = false;
            hasLast = false;
            return d;
        }

        // A capture that was never confirmed leaves no usable history.
        if (pending)
        {
            historyValid = false;
            pending = false;
        }

        const bool gap = hasLast && (f.frame <= lastFrame || f.frame - lastFrame > f.maxStep);
        const bool changed = hasLast && f.revision != lastRevision;
        // The frame after a blocked one runs the model too: what was saved then may be a frozen picture.
        const bool invalid = f.reset || f.blocked || lastBlocked || gap || changed;
        const bool usable = historyValid && !invalid;
        const bool wouldReuse = historyValid && !lastWasReuse;

        if (wouldReuse && !invalid)
        {
            d.kind = Kind::Reuse;
            d.saveMotion = true;
            ++reused;
            lastWasReuse = true;
        }
        else
        {
            if (wouldReuse && invalid)
                ++fallback;

            d.kind = Kind::Full;
            // After a gap the model's history is older still; composing two frames would not cover it,
            // and the saved motion belongs to a frame before the gap.
            d.composeMotion = lastWasReuse && !f.reset && !gap;
            ++full;
            lastWasReuse = false;
        }

        d.historyUsable = usable;
        d.captureHistory = true;
        historyValid = false;
        pending = true;

        hasLast = true;
        lastBlocked = f.blocked;
        lastFrame = f.frame;
        lastRevision = f.revision;
        return d;
    }

    // The frame Next asked to capture was processed and its history saved (ok), or not.
    void Captured(bool ok)
    {
        if (!pending)
            return;
        pending = false;
        historyValid = ok;
    }

    // The frame could not be processed as decided (the caller ran the model instead of reusing, or a pass
    // failed to record). Next frame starts clean: full, nothing reprojected, no composed motion.
    void Drop()
    {
        historyValid = false;
        pending = false;
        lastWasReuse = false;
    }

    // The reuse frame Next asked for could not be recorded (the caller ran the model instead): counted as a fallback,
    // not a reuse, and the next frame starts clean.
    void ReuseFailed()
    {
        if (lastWasReuse && reused > 0)
        {
            --reused;
            ++fallback;
        }
        Drop();
    }

    unsigned long long Full() const { return full; }
    unsigned long long Reused() const { return reused; }
    unsigned long long Fallback() const { return fallback; }
};

// Whether the real frame rate is high enough for reuse. Moved detail errs by how far things move between two real
// frames, so at a low frame rate the trails around moving bodies grow. Fed the time between two NR frames (one per
// rendered frame, so frame generation's frames do not count). The rate is smoothed over about half a second; reuse
// stops below the minimum and comes back only 15% above it, since stopping it lowers the frame rate further.
class FrameRateGate
{
    double interval = 0.0; // smoothed seconds per rendered frame; 0 = no reading yet
    bool allowed = true;

public:
    static constexpr double kResumeFactor = 1.15;

    // minimumFps <= 0: no minimum. Returns whether reuse may run.
    bool Update(double seconds, double minimumFps)
    {
        if (seconds > 0.0 && seconds < 1.0) // longer is a pause (loading, a menu), not a frame rate
        {
            const double alpha = interval > 0.0 ? 1.0 - std::exp(-seconds / 0.5) : 1.0;
            interval += (seconds - interval) * alpha;
        }
        if (!(minimumFps > 0.0) || interval <= 0.0)
            allowed = true;
        else if (allowed && Fps() < minimumFps)
            allowed = false;
        else if (!allowed && Fps() >= minimumFps * kResumeFactor)
            allowed = true;
        return allowed;
    }

    double Fps() const { return interval > 0.0 ? 1.0 / interval : 0.0; }
};
} // namespace DlssNrDetailReuse
