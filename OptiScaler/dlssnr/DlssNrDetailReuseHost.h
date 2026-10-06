#pragma once

// Reuse detail between frames: the part both backends share (menu: "Reuse detail between frames"; ini DetailReuse*).
//
// DlssNr_DetailReuse.inl (D3D12) and DlssNr_DetailReuse_Vk.inl (Vulkan) own the textures and record the GPU work. What
// does not depend on the API lives here, once: whether reuse may run this frame and why not, the frame rate gate, the
// cadence and its decision, the shader constants, the recent GPU times and the status the menu reads. The cadence itself
// is DlssNrDetailReuse.h.

#include <Config.h>
#include <State.h>

#include <dlssnr/DlssNrDetailReuse.h>
#include <dlssnr/DlssNrFeature_Dx12.h>
#include <dlssnr/DlssNr_GameDefaults.h>
#include <shaders/dlssnr/DlssNr_DetailReuseConstants.h>
#include <shaders/dlssnr/DlssNr_FinishedReady.h>
#include <shaders/dlssnr/DlssNr_ProxyCurve.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <utility>

namespace DlssNrDetailReuse
{

// What the backend knows about this frame, less its API's own objects.
struct HostFrame
{
    const Config* cfg = nullptr;
    bool beforeUpscale = false;   // NR runs before SR
    bool finishedPicture = false; // NR runs on the finished picture
    unsigned int workWidth = 0, workHeight = 0;
    unsigned int motionWidth = 0, motionHeight = 0, motionBaseX = 0, motionBaseY = 0;
    unsigned int motionAllocWidth = 0, motionAllocHeight = 0; // the motion texture itself
    unsigned int depthWidth = 0, depthHeight = 0, depthBaseX = 0, depthBaseY = 0;
    bool depthInverted = false;
    float mvScaleX = 1.0f, mvScaleY = 1.0f; // the game's own scale
    bool modelReset = false;                // the model starts over this frame
    bool blocked = false;                   // Tune or frame hold: the real model must run
    unsigned long long frameNumber = 0;     // the present counter, or NR's own frame count
    unsigned long long present = 0;         // the API's frame clock (DXGI presents, DlssNr::VkFrameClock on Vulkan)
    bool vulkan = false;                    // which DLSS-G stamp goes with that clock
    unsigned long long revision = 0;        // changes when anything that changes the model's answer changes
    bool passthrough = false;               // the frame is already tone-mapped: no proxy curve, nothing to decode
    uint32_t reversibleMode = 0;            // the proxy curve this frame's input was encoded with (DlssNr_ProxyCurve.h)
};

// The curve the resolve decodes a Replace answer through, for a proxy curve on a frame that is (passthrough) or is not
// already tone-mapped. Every Replace curve needs one here, or reuse would land its moved changes as differences.
constexpr DlssNrReplaceCurve ReplaceCurveFor(uint32_t reversibleMode, bool passthrough)
{
    if (passthrough || !DlssNrProxyCurve::IsReplace(reversibleMode))
        return DlssNrReplaceCurve_None;
    return reversibleMode == DlssNrProxyCurve::kNeutwoReplace ? DlssNrReplaceCurve_Neutwo : DlssNrReplaceCurve_Hybrid;
}

constexpr bool ReplaceCurvesMatch()
{
    for (uint32_t mode = 0; mode < DlssNrProxyCurve::kCount; ++mode)
    {
        if ((ReplaceCurveFor(mode, false) != DlssNrReplaceCurve_None) != DlssNrProxyCurve::IsReplace(mode) ||
            ReplaceCurveFor(mode, true) != DlssNrReplaceCurve_None)
            return false;
    }
    return ReplaceCurveFor(DlssNrProxyCurve::kNeutwoReplace, false) == DlssNrReplaceCurve_Neutwo &&
           ReplaceCurveFor(DlssNrProxyCurve::kBalancedReplace, false) == DlssNrReplaceCurve_Hybrid;
}
static_assert(ReplaceCurvesMatch(), "a Replace proxy curve with no reuse curve (or the reverse)");

// What Gate decided: whether reuse's textures are wanted this frame, whether they may stay while it is held off, and
// the settings they follow.
struct Wanted
{
    bool wanted = false;
    bool keep = false;
    bool withFg = false;  // kept on under frame generation (A/B testing)
    bool measure = false; // measure how much of the frame had no detail to move (MotionGuard is on)
    bool hold = false;    // that measurement says the picture is moving too fast: run the model this frame
    float steady = 0.0f;
    float fill = 0.0f;
};

// Frame generation is built from two real frames, so reused frames next to full ones show under it: the edges of the
// screen flickered in fast motion (NBA 2K27 with OptiFG 2026-09-28, The Witcher 3 2026-09-30) until MotionGuard stopped
// reuse from running through motion it cannot carry detail across. DetailReuseWithFG (on by default) keeps reuse running
// here; turned off, any of these reasons stands reuse down. Returns the reason, or null when none is seen: OptiScaler's own (active, not paused), the game's DLSS-G seen through NGX, or one owned by
// an external module.
inline const char* FrameGenerationInUse(unsigned long long present, bool vulkan)
{
    auto& state = State::Instance();
    if (state.currentFG != nullptr && state.currentFG->IsActive() && !state.currentFG->IsPaused())
        return "off while frame generation is on";
    // A DLSS-G evaluate within the last 30 presents that generates frames (or does not say how many: DLSS-G builds
    // without multi frame generation leave the count out). PresentsSince reads "never" as endlessly long ago.
    // Each API's stamp against its own clock.
    const bool generates = vulkan ? state.dlssgLastEvaluateGeneratesVk.load() : state.dlssgLastEvaluateGenerates;
    const unsigned long long stamp = vulkan ? state.dlssgLastEvaluateFrameVk.load() : state.dlssgLastEvaluateFrame;
    if (generates && DlssNr::PresentsSince(present, stamp) <= 30)
        return "off while the game's frame generation is on";
    if (state.externalFrameGeneration)
        return "off while an external frame generation owns the frames";
    return nullptr;
}

class Host
{
  public:
    explicit Host(const char* logName) : _logName(logName) {}

    Cadence cadence;
    Decision decision;
    DlssNrDetailReuseConstants params {};

    // Why reuse cannot run this frame, then what its textures should do. Measures the rendered frame rate whether or
    // not reuse runs, so the gate knows when to let it back. shaderReady() is asked last, and only when reuse is on:
    // it may build the shader. It returns null when the shader is ready, else why not.
    template <class ShaderReady> Wanted Gate(const HostFrame& f, ShaderReady&& shaderReady)
    {
        const Config& cfg = *f.cfg;
        const bool on = cfg.DlssNrDetailReuse.value_or_default();
        // A failed allocation is retried once the option is switched off and on, or at another working size.
        if ((_allocFailed || _extrasFailed) && (!on || f.workWidth != _failedWidth || f.workHeight != _failedHeight))
            _allocFailed = _extrasFailed = false;
        // Under frame generation it runs unless asked not to (DetailReuseWithFG, on by default).
        const char* fg = FrameGenerationInUse(f.present, f.vulkan);
        Wanted w;
        w.withFg = fg != nullptr && cfg.DlssNrDetailReuseWithFg.value_or_default();
        // NR runs once per rendered frame, so the time between two calls is the rendered frame rate, frame generation
        // or not.
        const auto now = std::chrono::steady_clock::now();
        const double sinceLast = _lastNrFrame.time_since_epoch().count() != 0
                                     ? std::chrono::duration<double>(now - _lastNrFrame).count()
                                     : 0.0;
        _lastNrFrame = now;
        const bool fastEnough = _rateGate.Update(sinceLast, cfg.DlssNrDetailReuseMinFps.value_or_default());
        // How much of the picture arrived with no detail to move, from the GPU a few frames ago (RecordDropped). Read
        // once: a frame with no new reading keeps the guard's state.
        const float maxDropped = std::clamp(cfg.DlssNrDetailReuseMaxDropped.value_or_default(), 0.0f, 100.0f) / 100.0f;
        w.measure = maxDropped > 0.0f;
        w.hold = _motionGuard.Update(std::exchange(_dropped, -1.0f), maxDropped, sinceLast);
        if (!w.measure)
            _droppedLast = -1.0f; // nothing is being measured: the menu must not show an old reading
        const char* whyNot = nullptr;
        if (f.beforeUpscale)
            whyNot = "unavailable while NR runs before SR";
        else if (f.finishedPicture)
            whyNot = "unavailable in Finished Picture";
        else if (fg != nullptr && !w.withFg)
            whyNot = fg;
        else if (!fastEnough)
            whyNot = kBelowMinimumFps;
        else if (_allocFailed)
            whyNot = "its history textures could not be allocated";
        else if (on)
            whyNot = shaderReady();
        _why = on && whyNot != nullptr ? whyNot : "";

        const std::string state = !_why.empty()       ? _why
                                  : on && w.withFg ? "available, kept on with frame generation"
                                                   : "available";
        if (state != _loggedWhy)
        {
            LOG_INFO("{}: {} (rendered frame rate {:.0f} fps)", _logName, state, _rateGate.Fps());
            _loggedWhy = state;
        }

        // Held off while the picture moves too fast: not logged as a state, it comes and goes with the motion (the
        // menu shows it, and the frame counts carry it).
        w.steady = std::clamp(cfg.DlssNrDetailReuseSteady.value_or_default(), 0.0f, 1.0f);
        w.fill = std::clamp(cfg.DlssNrDetailReuseFill.value_or_default(), 0.0f, 1.0f);
        // Held off for now (frame generation, which often pauses and resumes, or the frame rate): the textures stay a
        // while.
        const bool heldOff = on && whyNot != nullptr && (whyNot == fg || whyNot == kBelowMinimumFps);
        _gatedFrames = heldOff ? _gatedFrames + 1 : 0;
        w.wanted = on && whyNot == nullptr;
        w.keep = heldOff && _gatedFrames <= kKeepGatedFrames;
        return w;
    }

    // An allocation failed at this working size: not retried until the size changes or the option is switched off
    // and on. extras: the estimate or the steadiness texture (reuse still runs without them).
    void AllocationFailed(const HostFrame& f, bool extras)
    {
        (extras ? _extrasFailed : _allocFailed) = true;
        _failedWidth = f.workWidth;
        _failedHeight = f.workHeight;
    }

    bool ExtrasFailed() const { return _extrasFailed; }

    // New textures: the recent GPU times were measured on the old ones.
    void TexturesRebuilt() { _gpuRecentCount = 0; }

    // Which frame this is (the cadence) and the shader constants for it.
    void Decide(const HostFrame& f, bool active, const Wanted& w)
    {
        FrameFacts facts;
        facts.enabled = active;
        facts.reset = f.modelReset;
        facts.blocked = f.blocked;
        facts.hold = w.hold;
        // Without frame generation NR runs on every present, so any skipped present is a gap. With it, the present
        // counter can also count generated frames (up to 3 per real one with multi frame generation).
        facts.frame = f.frameNumber;
        facts.maxStep = w.withFg ? 4 : 1;
        // The saved change is in the values of the proxy curve it was made in, so another curve (or a frame that turns
        // passthrough, or back) starts over like any other change to the model's answer.
        const DlssNrReplaceCurve replaceCurve = ReplaceCurveFor(f.reversibleMode, f.passthrough);
        facts.revision = f.revision;
        for (const unsigned long long part : { (unsigned long long) f.reversibleMode, f.passthrough ? 1ull : 0ull })
            facts.revision = facts.revision * 1000003ull ^ part;
        decision = cadence.Next(facts);

        params = {};
        params.WorkWidth = f.workWidth;
        params.WorkHeight = f.workHeight;
        params.MotionWidth = f.motionWidth;
        params.MotionHeight = f.motionHeight;
        params.MotionBaseX = f.motionBaseX;
        params.MotionBaseY = f.motionBaseY;
        params.DepthWidth = f.depthWidth;
        params.DepthHeight = f.depthHeight;
        params.DepthBaseX = f.depthBaseX;
        params.DepthBaseY = f.depthBaseY;
        params.DepthInverted = f.depthInverted ? 1u : 0u;
        // The game's own scale: raw times it is pixels of the motion subrect, which the shader divides by its size.
        params.MvScaleX = f.mvScaleX;
        params.MvScaleY = f.mvScaleY;
        // The four values behind trust, live from the config so they can be found in a game rather than guessed.
        // Their defaults mirror the k constants in DlssNr_DetailReuseConstants.h, so an untouched install behaves
        // as before, with two exceptions: DepthTolerance's own default was raised at the same time (see there),
        // and it is now a per-game default rather than a single one -- DetailReuseDepthToleranceEffective
        // (DlssNr_GameDefaults.h) gives a short list of measured games (currently just RDR2) their own value
        // instead, unless the user set DetailReuseDepthTolerance themselves.
        params.DepthTolerance = DlssNr::DetailReuseDepthToleranceEffective(*f.cfg);
        params.ClipGamma = f.cfg->DlssNrDetailReuseClipGamma.value_or_default();
        params.ClipFalloff = f.cfg->DlssNrDetailReuseClipFalloff.value_or_default();
        params.SigmaFloor = f.cfg->DlssNrDetailReuseSigmaFloor.value_or_default();
        params.Steady = w.steady;
        params.FillStrength = w.fill;
        // Scaled with the working size, like the colour box: the same reach on screen at any model resolution.
        params.FillRadius = kDlssNrDetailReuseFillRadius1080p *
                            std::clamp(std::sqrt((float) f.workWidth * (float) f.workHeight / (1920.0f * 1080.0f)),
                                       0.5f, 2.0f);
        params.DebugView = f.cfg->DlssNrDetailReuseDebug.value_or_default() ? 1u : 0u;
        params.ReplaceCurve = replaceCurve;
        params.MotionReject = kDlssNrDetailReuseMotionReject;
        params.SteadyDeadZone = kDlssNrDetailReuseSteadyDeadZone;
    }

    // The end of the frame's decision. Returns whether the model's history must start over: it last ran two frames ago
    // but gets no composed vectors now (reuse was switched off, frame generation came on, the size changed, or
    // composing failed), so its history would be moved by one frame of motion.
    bool Finish(const HostFrame& f, bool active, bool reused, bool composedReadable)
    {
        bool resetModel = false;
        if (!reused && _modelSkipped && !composedReadable && !f.modelReset)
        {
            resetModel = true;
            if (!_loggedRealign)
            {
                _loggedRealign = true;
                LOG_INFO("{}: model history reset after a reused frame (no composed vectors)", _logName);
            }
        }
        _modelSkipped = reused;
        _activeLast = active;
        Publish(f.present);
        return resetModel;
    }

    void RecordGpuTime(double ms) { _gpuRecent[_gpuRecentCount++ % 16] = ms; }

    // The share of a measured frame (0..1) that had no detail to move, read back from the GPU. Several may arrive
    // between two frames: the largest is kept, since one flickering frame is seen. Summed up for the log, which speaks
    // every kDroppedLogSeconds and only about that window, so a threshold can be chosen from real play without the
    // line becoming spam (the reuse status line next to it is every ~10 s too).
    void RecordDropped(float share)
    {
        if (!(share >= 0.0f) || !(share <= 1.0f))
            return;
        _dropped = std::max(_dropped, share);
        _droppedLast = share;
        _droppedLow = _droppedSeen ? std::min(_droppedLow, share) : share;
        _droppedHigh = _droppedSeen ? std::max(_droppedHigh, share) : share;
        _droppedSum += share;
        ++_droppedSeen;
        const unsigned long long held = cadence.Held();
        const auto now = std::chrono::steady_clock::now();
        if (_droppedLogged.time_since_epoch().count() == 0)
        {
            _droppedLogged = now;
            _droppedHeld = held;
        }
        else if (std::chrono::duration<double>(now - _droppedLogged).count() >= kDroppedLogSeconds)
        {
            LOG_INFO("{}: over the last {:.0f}s, {:.1f}% of the picture had no detail to move on average of {} measured "
                     "frames ({:.1f}% to {:.1f}%); reuse paused on {} of them",
                     _logName, kDroppedLogSeconds, 100.0 * _droppedSum / _droppedSeen, _droppedSeen,
                     100.0 * _droppedLow, 100.0 * _droppedHigh, held - _droppedHeld);
            _droppedLogged = now;
            _droppedHeld = held;
            _droppedSeen = 0;
            _droppedSum = 0.0;
        }
    }

    DlssNr::DetailReuseInfo Status() const
    {
        DlssNr::DetailReuseInfo status { cadence.Full(), cadence.Reused(), cadence.Fallback(), cadence.Held(), _why };
        const unsigned int count = std::min(_gpuRecentCount, 16u);
        for (unsigned int i = 0; i < count; ++i)
        {
            status.lightMs = i == 0 ? _gpuRecent[i] : std::min(status.lightMs, _gpuRecent[i]);
            status.heavyMs = i == 0 ? _gpuRecent[i] : std::max(status.heavyMs, _gpuRecent[i]);
            status.averageMs += _gpuRecent[i] / count;
        }
        status.active = _activeLast;
        status.baseFps = _rateGate.Fps();
        status.holding = _motionGuard.Holding();
        status.dropped = _droppedLast;
        return status;
    }

    // For the menu, without the backend's own lock. present: the same counter the frames carried. A status not
    // published for kStalePresents reads as not active: NR stopped reaching the backend (NR turned off, the proxy
    // path, the game stops upscaling).
    DlssNr::DetailReuseInfo Published(unsigned long long present)
    {
        std::lock_guard<std::mutex> lock(_publishedMutex);
        DlssNr::DetailReuseInfo status = _published;
        if (present > _publishedPresent && present - _publishedPresent > kStalePresents)
            status.active = false;
        return status;
    }

    // Shutdown: everything the textures' lifetime decided is forgotten with them.
    void Reset()
    {
        _allocFailed = _extrasFailed = _modelSkipped = _activeLast = false;
        _failedWidth = _failedHeight = _gatedFrames = 0;
        _dropped = _droppedLast = -1.0f;
        _droppedSeen = 0;
        _droppedHeld = 0;
        _droppedSum = 0.0;
        _droppedLogged = {};
        _lastNrFrame = {}; // the next frame measures no interval, rather than the whole time NR was away
        _motionGuard = {};
        cadence.Drop();
        std::lock_guard<std::mutex> lock(_publishedMutex);
        _published = {};
    }

  private:
    static constexpr const char* kBelowMinimumFps = "off below the minimum frame rate";
    static constexpr unsigned long long kStalePresents = 30;
    // Frame generation pauses briefly and often (menu, loading, some games toggle it): the textures stay through this
    // many NR frames of it before they are released.
    static constexpr unsigned int kKeepGatedFrames = 300;

    // The status the menu reads, published at the end of each frame's decision under a lock of its own: the
    // backend's lock is held through the whole of NR's recording, and the menu must not wait on it every frame.
    void Publish(unsigned long long present)
    {
        auto status = Status();
        std::lock_guard<std::mutex> lock(_publishedMutex);
        _published = std::move(status);
        _publishedPresent = present;
    }

    const char* _logName;
    bool _allocFailed = false, _extrasFailed = false;
    unsigned int _failedWidth = 0, _failedHeight = 0; // the working size an allocation failed at
    bool _modelSkipped = false;                       // the model was skipped on the last NR frame
    bool _activeLast = false;
    unsigned int _gatedFrames = 0; // NR frames in a row with reuse held off for now (frame generation, low frame rate)
    FrameRateGate _rateGate;
    MotionGuard _motionGuard;
    float _dropped = -1.0f;     // this frame's reading for the guard, taken once (negative: none)
    float _droppedLast = -1.0f; // the newest reading, for the menu
    float _droppedLow = 0.0f, _droppedHigh = 0.0f;
    double _droppedSum = 0.0;
    unsigned int _droppedSeen = 0;
    unsigned long long _droppedHeld = 0; // Held() when the window started, so the line counts that window only
    std::chrono::steady_clock::time_point _droppedLogged {};
    static constexpr double kDroppedLogSeconds = 10.0;
    std::chrono::steady_clock::time_point _lastNrFrame {};
    std::string _why;
    std::string _loggedWhy;
    bool _loggedRealign = false;
    double _gpuRecent[16] = {}; // the last 16 whole-pass GPU times: full and reused frames cost differently
    unsigned int _gpuRecentCount = 0;

    std::mutex _publishedMutex;
    DlssNr::DetailReuseInfo _published;
    unsigned long long _publishedPresent = 0;
};

} // namespace DlssNrDetailReuse
