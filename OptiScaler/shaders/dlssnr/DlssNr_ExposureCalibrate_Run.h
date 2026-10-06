#pragma once

// "Tune for this scene": the run, shared by the D3D12 path (DlssNr_ExposureCalibrate_Dx12.inl) and the Vulkan path
// (dlssnr/DlssNr_ExposureCalibrate_Vk.inl). DlssNr_ExposureCalibrate.h has the sweep, the availability rules and the
// why. Here: the menu's requests, the NR-stopped check, the sweep's EV for each evaluation, the readback ring's
// bookkeeping and the text of the log. Each backend keeps its own GPU resources behind Backend, and its own copies and
// stats pass.
//
// Pure, like DlssNr_ExposureCalibrate.h: no clock, config, logger or D3D, so tests/nr_exposure_calibrate_smoke.cpp
// drives it with a fake Backend. DlssNr_ExposureCalibrate.cpp wraps it for the game (the clock, the sliders' EVs,
// logging, re-learning Follow) and holds the menu's API.
//
// Nothing costs anything until it is wanted: the resources are created when a run starts and released when it ends.
// While no run is on and the menu is not looking, an evaluation only stamps the time (IdleFrame); availability is
// worked out only while the menu polls or a run is on (Wanted).
//
// Per evaluation while it is wanted, the backend calls:
//   BeginFrame   before the white point is resolved: the NR-stopped check, the menu's start, readbacks that are due,
//                and the sweep's EV for this evaluation. The white point is pinned: the base white point frozen at the
//                start times the step's Trim (RunState::frameWhitePoint), used by the encode and resolve instead of the
//                live exposure, so Automatic and Game exposure hold still for the run; a drifting base aborts it.
//                Automatic is tuned against its own base even while following the game's exposure (the backends'
//                base), and Follow is learned again during the run (RelearnFollowOnStart).
//   after the encode: the untouched frame into this evaluation's input copy (RunState::current picks which of two)
//   after the resolve: the edited frame into this evaluation's output copy, and on a measured evaluation the stats pass
//                over output, previous output, input, previous input and the picture the model was shown, into a free
//                readback slot (FreeSlot, then Submitted)
// Input and output are each kept twice, alternating, so the previous evaluation's copy is still there to compare with.
//
// "Measure detail" (RequestMeasure) is the same run with MeasureSettings: every evaluation of it is copied and measured
// (Frame::capture) with the white point left live, and its result is kept as the latest measurement beside the one
// before it, so the menu can show an A/B.

#include "DlssNr_ExposureCalibrate.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace DlssNrExposureCalibrate
{
// Readbacks are read this many evaluations after they were recorded, so the GPU is certainly done with them. Frame
// generation keeps the GPU further behind than the 8 the frame statistics use (the retirement list waits 32). The ring
// holds every measured evaluation of that window. A measure measures every evaluation: on D3D12 16 are in flight (a
// slot is read back at the start of the evaluation it is due in, before that evaluation takes one); Vulkan stamps a
// submission with the next evaluation's count, so 17. A sweep has at most 8. A full ring drops samples (FreeSlot).
constexpr unsigned int kReadDelay = 16;
constexpr unsigned int kRing = 20;
static_assert(kRing > kReadDelay + 1, "a measure measures every evaluation: the ring must outlast the read delay");
// No evaluation for this long: NR stopped. A run is abandoned and a pending start dropped.
constexpr unsigned long long kStallMs = 2000;
// With Before SR set, a Tune runs after SR (TuneRunsAfterSr): this many evaluations after it was asked for, so NR has
// rebuilt at the new size and its history has settled, before the run starts (about half a second at 60 fps).
constexpr unsigned long long kAfterSrSettle = 30;
// The menu counts as looking for this long after it last polled; availability is only worked out meanwhile.
constexpr unsigned long long kMenuMs = 1000;
// Thresholds for damage, on the picture the model was shown (its peak channel above the shoulder, or below the floor),
// depend on the proxy curve: DlssNrProxyCurve::Thresholds, taken when the stats pass is dispatched.

// A backend's GPU side of a run: the grid, the readback ring and the copies.
class Backend
{
  public:
    virtual bool Held() const = 0;               // the resources exist
    virtual bool Create() = 0;                   // makes the grid and the readback ring; false leaves nothing behind
    virtual void Release() = 0;                  // gives everything back (once the GPU cannot be using it)
    virtual Stats Reduce(unsigned int slot) = 0; // a readback slot -> one Stats; an empty sample when unreadable

  protected:
    ~Backend() = default;
};

struct RunState
{
    // Menu thread and render thread, under the mutex.
    std::mutex mutex;
    Sweep sweep;
    bool startRequested = false;
    uint32_t source = 3;                  // the panel a run was started from: 3 Automatic, 1 Game exposure
    Blocker blocker = Blocker::NrStopped; // why a run cannot start or go on, as of the last wanted evaluation
    const char* startError = "";          // why the last start did not happen, "" if it did
    // The run (or the start asked for) is a Measure detail, not a Tune. Atomic: Follow's easing reads it lock-free
    // (HoldsFollow).
    std::atomic<bool> measuring { false };
    // With Before SR set: the evaluation a Tune asked for may start from (0 until the first one after the request).
    unsigned long long startAt = 0;
    // A Tune is asked for or running (not a measure, not readbacks draining after it). Read lock-free by Follow's
    // easing and the placement (HoldsFollow, TuneRunsAfterSr); cleared when the run ends, a start is refused, or NR
    // has stopped calling in.
    std::atomic<bool> tuneOn { false };
    // Measure detail's results: the latest and the one before it (samples 0 when there is none), how many, and the
    // scale each was measured at (its white point source and measuring white point): two results compare only at
    // the same scale.
    StepResult latest {};
    StepResult previous {};
    unsigned measurements = 0;
    struct Scale
    {
        uint32_t source = 0;
        float whitePoint = 0.0f;
    };
    Scale latestScale {};
    Scale previousScale {};

    // Either thread, lock-free: read every evaluation.
    std::atomic<bool> active { false }; // a start is pending, a run is on, or resources are held
    std::atomic<unsigned long long> lastEvaluationMs { 0 };
    std::atomic<unsigned long long> menuPolledMs { 0 };

    // Render thread only.
    Frame frame {};
    float frameWhitePoint = 0.0f; // the pinned white point of this evaluation, 0 when nothing runs
    bool measurePending = false;  // this evaluation should be measured and has not been yet
    float measureWhitePoint = 1.0f;
    bool logged = true;
    unsigned int current = 0;
    bool slotBusy[kRing] = {};
    Ticket slotTicket[kRing] = {};
    unsigned long long slotDue[kRing] = {};
};

// Where each slider stands when a run starts, in its own EV (EvForTrim with its neutral).
struct StartPoints
{
    float automatic = 0.0f;
    float gameExposure = 0.0f;
};

// What happened on an evaluation that the caller acts on: log, re-learn Follow.
struct FrameEvents
{
    bool started = false;       // a run started (RunState::sweep has its steps, RunState::measureWhitePoint its scale)
    bool relearnFollow = false; // ... and Follow the game's exposure is to be learned again (RelearnFollowOnStart)
    bool afterSr = false;       // ... and it runs after SR although Before SR is set (TuneRunsAfterSr)
    bool finished = false;      // a run ended, by a result or an abort: ResultLines is due in the log
};

inline std::string StopText(const Sweep& s)
{
    std::string text = AbortText(s.AbortReason());

    if (s.AbortReason() == Abort::Unavailable)
        text = text + ": " + BlockerText(s.StopBlocker());

    // Movement is tried again before it stops a run, so a stop says what the last try measured.
    if (s.AbortReason() == Abort::Motion)
    {
        if (s.LastSceneBand() >= 0.0f)
            text += std::format(" (the view changed again: input detail {:.5f}, the run started at {:.5f})",
                                s.LastSceneBand(), s.SceneBand());
        else
            text += std::format(" (input change {:.5f} after {} tries, limit {:.5f})", s.LastInputChange(),
                                s.Config().motionRetries + 1, s.Config().motionLimit);
    }

    return text;
}

// A note from a running sweep, for the log. A measure has one step, the current value, so it names no EV.
inline std::string NoteText(const Note& n, bool measure = false)
{
    if (measure && n.kind == Note::Kind::Retry)
        return std::format("DLSS-NR measure: the picture moved (input change {:.5f}, limit {:.5f}), measuring again (try {})",
                           n.value, n.against, n.count + 1);
    if (n.kind == Note::Kind::Retry)
        return std::format("DLSS-NR calibrate: {:+.1f} EV moved (input change {:.5f}, limit {:.5f}), measuring it again "
                           "(try {})",
                           Tidy(n.ev), n.value, n.against, n.count + 1);
    return std::format("DLSS-NR calibrate: the view changed at {:+.1f} EV (input detail {:.5f}, the run started at "
                       "{:.5f}), starting over (restart {})",
                       Tidy(n.ev), n.value, n.against, n.count);
}

// One measurement, for the log: the same numbers as a sweep step's line.
inline std::string MeasureText(const StepResult& r)
{
    return std::format("n {} | raw {:.5f} band {:.5f} (out {:.5f} in {:.5f}) | flicker {:.5f} (out {:.5f} in {:.5f}) | "
                       "shoulder {:.4f} floor {:.4f} | colour saturation {:+.1f}% (chroma out {:.4f} in {:.4f}) shift "
                       "{:.4f} warmth {:+.4f} | shadows {:.3f} of the picture, darkened {:+.1f}% (out {:.5f} in {:.5f}), "
                       "crushed {:.4f}",
                       r.samples, r.detailRaw, r.DetailOf(Detail::BandPass), r.detailBand, r.inputBand, r.Flicker(),
                       r.outputChange, r.inputChange, r.shoulder, r.floor, 100.0f * r.Saturation(), r.chromaOut,
                       r.chromaIn, r.colourShift, r.warmth, r.shadowShare, 100.0f * r.ShadowDarkening(), r.shadowOut,
                       r.shadowIn, r.crushed);
}

// "Measure detail" in words, for the menu: each measure against the game's own frame, then against the measurement
// before. Changes within kSameWithin (relative) or kSamePoints (percentage points) read as the same: a still NBA 2K27
// scene repeated detail to 0.3%, so this leaves room for a busier one; two measurements in a row show the real noise.
constexpr float kSameWithin = 0.03f;
constexpr float kSamePoints = 0.02f;

inline std::string DetailWords(const StepResult& r)
{
    const float added = r.DetailOf(Detail::BandPass);
    if (!(r.inputBand > 1e-6f))
        return std::format("Detail: {:+.5f} added (the game's frame has no detail to compare with)", added);
    const float share = added / r.inputBand;
    if (std::fabs(share) < kSamePoints) // a fraction of a percent reads as "0% more", which says nothing
        return "Detail: about as much as the game's own";
    return share >= 0.0f ? std::format("Detail: {:.0f}% more than the game's own", 100.0f * share)
                         : std::format("Detail: {:.0f}% less than the game's own (NR smooths here)", -100.0f * share);
}

// A still frame measures about 0.0002 of input change (NBA 2K27), so anything below kStillInput is "held still": a
// multiple taken against a few millionths reads as hundreds of times and means nothing.
constexpr float kStillInput = 1e-4f;

inline std::string FlickerWords(const StepResult& r)
{
    if (!(r.inputChange > kStillInput))
        return std::format("Flicker: {:.5f} of the picture changes between frames (the game's frame held still)",
                           r.Flicker());
    return std::format("Flicker: the output moves {:.1f}x as much as the game's frame between frames (1x = none added)",
                       r.outputChange / r.inputChange);
}

inline std::string ColourWords(const StepResult& r)
{
    if (!(r.chromaIn > 1e-4f))
        return "Colour: the picture is almost grey, nothing to compare";
    const float sat = r.Saturation();
    std::string text = std::fabs(sat) < kSamePoints
                           ? std::string("Colour: saturation as the game's")
                           : std::format("Colour: {:.0f}% {} saturated than the game's", 100.0f * std::fabs(sat),
                                         sat > 0.0f ? "more" : "less");
    const float warmth = r.warmth;
    if (std::fabs(warmth) >= 0.006f)
        text += warmth > 0.0f ? ", warmer" : ", cooler";
    else if (std::fabs(warmth) >= 0.002f)
        text += warmth > 0.0f ? ", slightly warmer" : ", slightly cooler";
    return text;
}

inline std::string ShadowWords(const StepResult& r)
{
    if (r.shadowShare < 0.005f)
        return "Shadows: hardly any in the picture";
    const float dark = r.ShadowDarkening();
    std::string text = std::format("Shadows ({:.0f}% of the picture): ", 100.0f * r.shadowShare);
    text += std::fabs(dark) < kSamePoints ? std::string("kept as the game's")
            : dark < 0.0f                 ? std::format("{:.0f}% lifted", -100.0f * dark)
                                          : std::format("{:.0f}% darker", 100.0f * dark);
    text += r.crushed < 0.0005f ? ", nothing crushed"
                                : std::format(", {:.1f}% of the picture crushed toward black", 100.0f * r.crushed);
    return text;
}

// The latest measurement against the one before (both at the same scale).
inline std::string CompareWords(const StepResult& now, const StepResult& before)
{
    std::vector<std::string> parts;
    const auto relative = [](float a, float b) { return std::fabs(b) > 1e-7f ? (a - b) / std::fabs(b) : 0.0f; };

    // A percentage against nothing is not a comparison: "was none, now X" says what happened, where relative() would
    // report "about the same" however large the new value is (Flicker() is exactly 0 whenever the output moved no more
    // than the game's frame, which a paused scene does reach).
    const auto fromZero = [](const char* name, float now, float scale)
    { return std::format("{} was none, now {:.5f}", name, now * scale); };

    const float nowDetail = now.DetailOf(Detail::BandPass), beforeDetail = before.DetailOf(Detail::BandPass);
    const float detail = relative(nowDetail, beforeDetail);
    if (std::fabs(beforeDetail) <= 1e-7f && std::fabs(nowDetail) > 1e-7f)
        parts.push_back(fromZero("detail", nowDetail, 1.0f));
    else
        parts.push_back(std::fabs(detail) < kSameWithin
                            ? std::string("detail about the same")
                            : std::format("{} detail ({:+.0f}%)", detail > 0.0f ? "more" : "less", 100.0f * detail));

    const float nowFlicker = now.Flicker(), beforeFlicker = before.Flicker();
    const float flicker = relative(nowFlicker, beforeFlicker);
    if (beforeFlicker <= 1e-7f && nowFlicker > 1e-7f)
        parts.push_back(fromZero("flicker", nowFlicker, 1.0f) + " (worse)");
    else
        parts.push_back(std::fabs(flicker) < kSameWithin
                            ? std::string("flicker about the same")
                            : std::format("{} flicker ({:+.0f}%, {})", flicker > 0.0f ? "more" : "less",
                                          100.0f * flicker, flicker > 0.0f ? "worse" : "better"));

    const float sat = now.Saturation() - before.Saturation();
    if (std::fabs(sat) >= kSamePoints)
        parts.push_back(std::format("{} saturated ({:+.0f} points)", sat > 0.0f ? "more" : "less", 100.0f * sat));

    const float dark = now.ShadowDarkening() - before.ShadowDarkening();
    if (std::fabs(dark) >= kSamePoints)
        parts.push_back(std::format("shadows {} ({:+.0f} points)", dark > 0.0f ? "darker" : "lighter", 100.0f * dark));

    const float crushed = now.crushed - before.crushed;
    if (std::fabs(crushed) >= 0.001f)
        parts.push_back(std::format("{} crushed ({:+.1f} points)", crushed > 0.0f ? "more" : "less", 100.0f * crushed));

    std::string text;
    for (size_t i = 0; i < parts.size(); ++i)
        text += (i == 0 ? "" : ", ") + parts[i];
    return text;
}

// The log of a run that ended: the abort, one line per measured step, the result.
// The Tune target a run had, for the log: Max detail, Natural with its floor, or Natural with nothing to put it on.
inline std::string TargetText(const Sweep& s)
{
    if (!s.NaturalWanted())
        return "DLSS-NR calibrate: target Max detail";
    if (!s.HasFloor())
        return "DLSS-NR calibrate: target Natural, but the game gives no exposure to set its floor by: tuned as Max "
               "detail";
    return std::format("DLSS-NR calibrate: target Natural, no darker than {:+.2f} EV ({:.1f} EV under the game's own "
                       "exposure)",
                       Tidy(s.FloorEv()), s.Config().floorBelowGameEv);
}

inline std::vector<std::string> ResultLines(const Sweep& s)
{
    std::vector<std::string> lines;

    if (s.Config().measureOnly)
    {
        if (s.AbortReason() != Abort::None)
            lines.push_back(std::format("DLSS-NR measure: stopped: {}", StopText(s)));
        else if (s.Finished())
            lines.push_back(std::format("DLSS-NR measure: {}", MeasureText(s.Steps().front())));
        return lines;
    }

    const auto& steps = s.Steps();
    const float neutral = s.Config().neutralTrim;
    const bool passes = s.Passes() > 1;
    // With several passes each line says which; one pass keeps the lines as they were.
    const auto passText = [passes](unsigned pass) { return passes ? std::format("pass {}: ", pass + 1) : std::string(); };

    if (s.AbortReason() != Abort::None)
        lines.push_back(std::format(
            "DLSS-NR calibrate: stopped after {} of {} steps{}: {}",
            std::count_if(steps.begin(), steps.end(), [](const StepResult& r) { return r.samples > 0; }),
            steps.size(), passes ? std::format(" of pass {} of {}", s.Pass() + 1, s.Passes()) : "", StopText(s)));

    const auto stepLines = [&](const std::vector<StepResult>& pass, unsigned index)
    {
        for (size_t i = 0; i < pass.size(); ++i)
        {
            const StepResult& r = pass[i];

            if (r.samples == 0)
                continue;

            lines.push_back(std::format("DLSS-NR calibrate: {}{:+.1f} EV (trim {:.3f}) {} | score raw {:.3f} band {:.3f}",
                                        passText(index), Tidy(r.ev), TrimForEv(r.ev, neutral), MeasureText(r),
                                        s.ScoreIn(pass, i, Detail::Raw), s.ScoreIn(pass, i, Detail::BandPass)));
        }
    };
    const auto verdictText = [](const PassVerdict& v)
    {
        return std::format("best raw {:+.1f} EV, best band {:+.1f} EV, result {:+.2f} EV{}", Tidy(v.bestRaw),
                           Tidy(v.bestBand), Tidy(v.result),
                           v.unsure ? " (unsure)" : "");
    };

    // The passes before the last, then the last (or the one a stop cut short).
    if (!s.FirstPassSteps().empty())
    {
        stepLines(s.FirstPassSteps(), 0);
        lines.push_back(std::format("DLSS-NR calibrate: pass 1: {}", verdictText(s.FirstPass())));
    }

    stepLines(steps, s.Pass());

    if (s.Finished() && passes)
        lines.push_back(std::format("DLSS-NR calibrate: pass {}: {}", s.Pass() + 1, verdictText(s.LastPass())));

    if (s.Finished())
        lines.push_back(std::format(
            "DLSS-NR calibrate: current {:+.2f} EV, best raw {:+.1f} EV, best band {:+.1f} EV, result {:+.2f} EV{}",
            Tidy(s.CurrentEv()), Tidy(s.BestEv(Detail::Raw)), Tidy(s.BestEv(Detail::BandPass)), Tidy(s.ResultEv()),
            s.Unsure()       ? " (unsure: detail varied no more than the measurement does on its own, keeps the current value)"
            : s.Unrepeated() ? std::format(" (did not repeat: pass 1 gave {:+.1f} EV, pass 2 {:+.1f} EV, keeps the "
                                           "current value)",
                                           Tidy(s.FirstPass().result), Tidy(s.LastPass().result))
            : s.Changed()    ? (s.AtLimit() ? " (the best step is the end of the range; offered anyway)" : "")
                             : " (flat: keeps the current value)"));

    if (s.Finished())
        lines.push_back(TargetText(s));

    return lines;
}

// Whether this evaluation needs BeginFrame: a start is pending, a run is on, resources are held, or the menu is
// looking (it shows availability). Lock-free; everything else is IdleFrame.
inline bool Wanted(const RunState& run, unsigned long long nowMs)
{
    return run.active.load(std::memory_order_acquire) ||
           nowMs - run.menuPolledMs.load(std::memory_order_relaxed) < kMenuMs;
}

// An evaluation while nothing is wanted: only the clock the NR-stopped check reads.
inline void IdleFrame(RunState& run, unsigned long long nowMs) { run.lastEvaluationMs.store(nowMs, std::memory_order_relaxed); }

// Before the white point is resolved, while Wanted(). `evaluation` counts the backend's NR evaluations (the readback
// delay is measured in it); `baseWhitePoint` is the tuned source's (0 without a reading).
inline FrameEvents BeginFrame(RunState& run, Backend& gpu, const StartPoints& start, unsigned int width, unsigned int height,
                              const Situation& situation, float baseWhitePoint, unsigned long long evaluation,
                              unsigned long long nowMs)
{
    std::lock_guard<std::mutex> lock(run.mutex);
    FrameEvents events;
    const unsigned long long last = run.lastEvaluationMs.exchange(nowMs, std::memory_order_relaxed);

    // NR stopped for a while (a loading screen, NR toggled off) with the menu closed: what the run measured before is
    // of another moment, and a start asked for back then is not wanted now.
    if (last != 0 && nowMs - last > kStallMs)
    {
        run.sweep.Abandon(Abort::NrOff);
        run.startRequested = false;
    }

    run.blocker = Availability(situation);
    const Blocker blocker = run.blocker;

    // A measured evaluation that never reached the stats pass (a failed evaluate, another path): its ticket is
    // returned empty, so the sweep does not wait for it.
    if (run.measurePending)
    {
        run.measurePending = false;
        Stats dropped {};
        dropped.detailBand = NAN;
        run.sweep.AddStats(run.frame.ticket, dropped);
    }

    for (unsigned int i = 0; i < kRing; ++i)
    {
        if (run.slotBusy[i] && evaluation >= run.slotDue[i])
        {
            run.slotBusy[i] = false;
            run.sweep.AddStats(run.slotTicket[i], gpu.Reduce(i));
        }
    }

    const Context ctx { width, height, situation.source, true, baseWhitePoint, blocker, situation.gameBaseWhitePoint };

    // A Tune with Before SR set runs after SR: the request itself moved NR there (TuneRunsAfterSr), and the run waits
    // until NR has settled at the new size.
    bool settling = false;

    if (run.startRequested && !run.sweep.Running() && !run.measuring && situation.beforeSrSet &&
        blocker == Blocker::None)
    {
        if (run.startAt == 0)
            run.startAt = evaluation + kAfterSrSettle;
        settling = evaluation < run.startAt;
    }

    if (run.startRequested && !run.sweep.Running() && !settling)
    {
        run.startRequested = false;
        run.startAt = 0;
        run.startError = "";

        if (blocker != Blocker::None)
        {
            run.startError = BlockerText(blocker);
        }
        else if (!(baseWhitePoint > 0.0f))
        {
            run.startError = "the exposure has no reading yet";
        }
        else if (!gpu.Held() && !gpu.Create())
        {
            run.startError = "could not allocate the calibration buffers";
        }
        else
        {
            // Each source tunes its own slider: Automatic around its 5x neutral, Game exposure around 1x.
            run.source = situation.source;
            const float current = situation.source == 1 ? start.gameExposure : start.automatic;
            if (run.measuring)
            {
                run.sweep.Start(current, MeasureSettings(situation.source), ctx);
            }
            else
            {
                Settings settings = situation.source == 1 ? GameExposureSettings() : Settings {};
                if (situation.naturalTarget)
                    settings.floorBelowGameEv = kNaturalFloorEv;
                run.sweep.Start(current, settings, ctx);
            }
            // One scale for every step and every run, whatever the slider was at (Sweep::MeasureWhitePoint).
            run.measureWhitePoint = run.sweep.MeasureWhitePoint();
            run.logged = false;
            events.started = true;
            // A measure leaves the brightness and Follow alone.
            events.relearnFollow = !run.measuring && RelearnFollowOnStart(situation);
            events.afterSr = !run.measuring && situation.beforeSrSet;
        }
    }

    run.frame = run.sweep.NextFrame(ctx);
    run.measurePending = run.frame.measure;
    run.frameWhitePoint = run.frame.override ? run.sweep.WhitePointFor(run.frame.ev) : 0.0f;

    if (run.frame.capture)
        run.current ^= 1u;

    if (!run.sweep.Running() && !run.logged)
    {
        run.logged = true;
        events.finished = true;

        if (run.measuring && run.sweep.Finished())
        {
            run.previous = run.latest;
            run.previousScale = run.latestScale;
            run.latest = run.sweep.Steps().front();
            run.latestScale = { run.source, run.measureWhitePoint };
            ++run.measurements;
        }
    }

    // Everything back once the run is over and nothing is still in flight to a readback.
    const bool inFlight = std::any_of(std::begin(run.slotBusy), std::end(run.slotBusy), [](bool b) { return b; });

    if (!run.sweep.Running() && gpu.Held() && !inFlight)
        gpu.Release();

    if (!run.sweep.Running() && !run.startRequested)
        run.tuneOn.store(false, std::memory_order_release);

    if (!run.sweep.Running() && !run.startRequested && !gpu.Held())
        run.active.store(false, std::memory_order_release);

    return events;
}

// This evaluation is part of a run and the resources are there: copy and measure.
inline bool Active(const RunState& run, const Backend& gpu) { return run.frame.capture && gpu.Held(); }

// A readback slot nothing is in flight to, or kRing when all are busy.
inline unsigned int FreeSlot(const RunState& run)
{
    for (unsigned int s = 0; s < kRing; ++s)
        if (!run.slotBusy[s])
            return s;
    return kRing;
}

// The stats of this evaluation were recorded into `slot`; readable from `evaluation + kReadDelay` on.
inline void Submitted(RunState& run, unsigned int slot, unsigned long long evaluation)
{
    std::lock_guard<std::mutex> lock(run.mutex);
    run.slotBusy[slot] = true;
    run.slotTicket[slot] = run.frame.ticket;
    run.slotDue[slot] = evaluation + kReadDelay;
    run.measurePending = false;
}

// Ends everything at NR shutdown: no run, no pending start, no pin left behind for whatever initialises next.
inline void Shutdown(RunState& run, Backend& gpu)
{
    std::lock_guard<std::mutex> lock(run.mutex);
    run.sweep.Abandon(Abort::NrOff);
    run.startRequested = false;
    run.frame = {};
    run.frameWhitePoint = 0.0f;
    run.measurePending = false;
    run.logged = true;

    for (bool& busy : run.slotBusy)
        busy = false;

    gpu.Release();
    run.tuneOn.store(false, std::memory_order_release);
    run.active.store(false, std::memory_order_release);
}

// The menu is looking, under the run's mutex: stamps the poll (so availability is worked out on the next evaluations)
// and says why a run cannot start. With no evaluation for kStallMs NR is not running: a run is abandoned, a pending
// start dropped, and the answer is NrStopped rather than a button that would wait forever.
inline Blocker PollLocked(RunState& run, unsigned long long nowMs)
{
    run.menuPolledMs.store(nowMs, std::memory_order_relaxed);

    const unsigned long long last = run.lastEvaluationMs.load(std::memory_order_relaxed);
    const bool stalled = last == 0 || nowMs - last > kStallMs;

    if (stalled)
    {
        run.sweep.Abandon(Abort::NrOff);
        run.startRequested = false;
        run.tuneOn.store(false, std::memory_order_release);
    }

    return stalled ? Blocker::NrStopped : run.blocker;
}

// A finished Tune whose result the menu still offers (Apply / Keep): a measure would clear it. Under the mutex.
inline bool TuneResultWaiting(const RunState& run) { return !run.measuring && run.sweep.Finished(); }

// A Tune is on (asked for or running) and NR is still calling in: after kStallMs without an evaluation it no longer
// counts, so nothing it holds can outlive NR (a failed build after SR returns before the run's BeginFrame, and would
// otherwise keep the run on for the session). Lock-free.
inline bool TuneOn(const RunState& run, unsigned long long nowMs)
{
    const unsigned long long last = run.lastEvaluationMs.load(std::memory_order_relaxed);
    return run.tuneOn.load(std::memory_order_acquire) && (last == 0 || nowMs - last <= kStallMs);
}

// Whether Follow the game's exposure holds its easing still (DlssNrFollowGame::Track's `hold`): while a Tune is on, as
// its steps are measured against a frozen base. Not for a measure, which measures the picture as it plays.
inline bool HoldsFollow(const RunState& run, unsigned long long nowMs) { return TuneOn(run, nowMs); }

// Whether NR runs after SR for now although Before SR is set: while a Tune is on. Before SR the game's frame is
// jittered, so a still scene reads as moving and a Tune cannot get through its stillness check; the user asked for
// Tune to run after SR there and go back afterwards. In memory only -- the setting itself is never changed, so nothing
// is left behind if the game exits mid-run. The result is measured after SR and applied to the Before SR picture. Not
// for a measure, which measures the setup as it is.
inline bool TuneRunsAfterSr(const RunState& run, unsigned long long nowMs) { return TuneOn(run, nowMs); }

// Two measurements compare (the menu's "vs previous") only when taken at the same scale: the same white point source,
// and measuring white points within 2% (the detail noise floor on a still NBA 2K27 scene was 0.3%).
inline bool SameScale(const RunState::Scale& a, const RunState::Scale& b)
{
    return a.source == b.source && a.whitePoint > 0.0f && b.whitePoint > 0.0f &&
           std::fabs(a.whitePoint / b.whitePoint - 1.0f) <= 0.02f;
}

// The menu's buttons.
inline void RequestStart(RunState& run, uint32_t source)
{
    std::lock_guard<std::mutex> lock(run.mutex);

    if (run.sweep.Running())
        return;

    run.source = source;
    run.measuring = false;
    run.startAt = 0;
    run.sweep.Clear();
    run.startError = "";
    run.startRequested = true;
    run.tuneOn.store(true, std::memory_order_release);
    run.active.store(true, std::memory_order_release);
}

// "Measure detail": the white point source is whatever is in use when it starts (it sets the measuring scale only).
inline void RequestMeasure(RunState& run)
{
    std::lock_guard<std::mutex> lock(run.mutex);

    // Not over a Tune result still waiting for Apply or Keep: starting would clear it (TuneResultWaiting).
    if (run.sweep.Running() || TuneResultWaiting(run))
        return;

    run.measuring = true;
    run.sweep.Clear();
    run.startError = "";
    run.startRequested = true;
    run.active.store(true, std::memory_order_release);
}

inline void RequestCancel(RunState& run)
{
    std::lock_guard<std::mutex> lock(run.mutex);
    run.startRequested = false;
    run.sweep.Cancel();
}

inline void Dismiss(RunState& run)
{
    std::lock_guard<std::mutex> lock(run.mutex);
    run.sweep.Clear();
    run.startError = "";
}

// The one run: only one NR backend evaluates at a time, and the menu talks to whichever it is.
inline RunState& TheRun()
{
    static RunState run;
    return run;
}
} // namespace DlssNrExposureCalibrate

// The game's side, in DlssNr_ExposureCalibrate.cpp (not built by the host test): the clock, the sliders' EVs, the log
// and re-learning Follow around the pure functions above, on TheRun().
class Config;

namespace DlssNrExposureCalibrate
{
bool WantedNow();
void IdleNow();
void BeginFrameNow(Backend& gpu, const ::Config& cfg, unsigned int width, unsigned int height,
                   const Situation& situation, float baseWhitePoint, unsigned long long evaluation);
void ShutdownNow(Backend& gpu);
} // namespace DlssNrExposureCalibrate
