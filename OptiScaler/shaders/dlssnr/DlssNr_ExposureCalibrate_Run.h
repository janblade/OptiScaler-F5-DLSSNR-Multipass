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
    Blocker measureBlocker = Blocker::NrStopped; // the same for "Measure detail"
    const char* startError = "";          // why the last start did not happen, "" if it did
    // The run (or the start asked for) is a Measure detail, not a Tune. Atomic: Follow's easing reads it lock-free
    // (HoldsFollow).
    std::atomic<bool> measuring { false };
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
                       "shoulder {:.4f} floor {:.4f}",
                       r.samples, r.detailRaw, r.DetailOf(Detail::BandPass), r.detailBand, r.inputBand, r.Flicker(),
                       r.outputChange, r.inputChange, r.shoulder, r.floor);
}

// The log of a run that ended: the abort, one line per measured step, the result.
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

    if (s.AbortReason() != Abort::None)
        lines.push_back(std::format(
            "DLSS-NR calibrate: stopped after {} of {} steps: {}",
            std::count_if(steps.begin(), steps.end(), [](const StepResult& r) { return r.samples > 0; }),
            steps.size(), StopText(s)));

    for (size_t i = 0; i < steps.size(); ++i)
    {
        const StepResult& r = steps[i];

        if (r.samples == 0)
            continue;

        lines.push_back(std::format("DLSS-NR calibrate: {:+.1f} EV (trim {:.3f}) {} | score raw {:.3f} band {:.3f}",
                                    Tidy(r.ev), TrimForEv(r.ev, neutral), MeasureText(r), s.Score(i, Detail::Raw),
                                    s.Score(i, Detail::BandPass)));
    }

    if (s.Finished())
        lines.push_back(std::format(
            "DLSS-NR calibrate: current {:+.2f} EV, best raw {:+.1f} EV, best band {:+.1f} EV, result {:+.2f} EV{}",
            Tidy(s.CurrentEv()), Tidy(s.BestEv(Detail::Raw)), Tidy(s.BestEv(Detail::BandPass)), Tidy(s.ResultEv()),
            s.Unsure()    ? " (unsure: detail varied no more than the measurement does on its own, keeps the current value)"
            : s.AtEdge()  ? " (at the edge of the range: the real best may lie beyond, keeps the current value)"
            : s.Changed() ? ""
                          : " (flat: keeps the current value)"));

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
    run.measureBlocker = Availability(situation, true);
    const Blocker blocker = run.measuring ? run.measureBlocker : run.blocker;

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

    const Context ctx { width, height, situation.source, true, baseWhitePoint, blocker };

    if (run.startRequested && !run.sweep.Running())
    {
        run.startRequested = false;
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
                run.sweep.Start(current, MeasureSettings(situation.source), ctx);
            else if (situation.source == 1)
                run.sweep.Start(current, GameExposureSettings(), ctx);
            else
                run.sweep.Start(current, Settings {}, ctx);
            // One scale for every step and every run, whatever the slider was at (Sweep::MeasureWhitePoint).
            run.measureWhitePoint = run.sweep.MeasureWhitePoint();
            run.logged = false;
            events.started = true;
            // A measure leaves the brightness and Follow alone.
            events.relearnFollow = !run.measuring && RelearnFollowOnStart(situation);
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
    }

    return stalled ? Blocker::NrStopped : run.blocker;
}

// A finished Tune whose result the menu still offers (Apply / Keep): a measure would clear it. Under the mutex.
inline bool TuneResultWaiting(const RunState& run) { return !run.measuring && run.sweep.Finished(); }

// Whether Follow the game's exposure holds its easing still (DlssNrFollowGame::Track's `hold`): while a Tune runs, as
// its steps are measured against a frozen base. Not for a measure, which measures the picture as it plays. Lock-free.
inline bool HoldsFollow(const RunState& run)
{
    return run.active.load(std::memory_order_acquire) && !run.measuring.load(std::memory_order_acquire);
}

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
    run.sweep.Clear();
    run.startError = "";
    run.startRequested = true;
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
