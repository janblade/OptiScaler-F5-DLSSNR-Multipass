#pragma once

// "Tune for this scene": a sweep of the model input brightness (Automatic exposure's Trim, shown in the menu as EV
// around the 5x neutral) over the scene on screen, scoring each step and picking the best one.
//
// Why: the model has no exposure input -- a game's own integration passes none and hands it a 0..1 picture -- so how
// bright that picture is decides what the model can see. Too bright and highlights sit on the encode's flat shoulder,
// too dark and shadows sink into the floor. In the composed path the exposure changes only what the model is shown:
// the resolve's ratio brings the answer back to the game's own brightness, so the output of every step can be compared
// directly.
//
// Each step holds one EV for `settle` evaluations, so the model's recurrent history catches up, then measures
// `measure` of them. The GPU reduces every measured evaluation to a grid of tile statistics (its own D3D12 pass,
// precompile/dlssnr_detail_stats.hlsl), which comes back through a readback ring some evaluations later, tagged with the
// Ticket it was issued under, and ReduceGrid turns it into one Stats. Score per step:
//   detail / (the largest detail of any step) - flickerWeight * flicker / (that same largest detail)
//     - damageWeight * shoulder - floorWeight * floor - colourWeight * |saturation change|
//     - shadowWeight * shadows darkened - crushWeight * crushed
// flicker = how much the output changed between evaluations beyond what the input did (the still-camera case of a
// warping error); shoulder and floor = the share of the encoded model input on the shoulder or in the floor; saturation,
// shadows darkened (only darker counts) and crushed compare the output with the game's frame (Settings has the weights).
// A run is unsure when detail varies across the steps no more than the measurement's own noise: then the pick would
// follow the noise, not the picture, and the current value is kept. The noise is the larger of two, both in the units
// the detail is compared in (Finish has the rule): the input's -- every step measures the band detail of the same,
// untouched game frame, so however much that varies across the steps is the measurement on its own -- and the
// output's -- how uncertain each step's mean is from the spread of its own samples. The detail measure
// is selectable: the output's raw Laplacian (the focus-measure classic, sensitive to single-pixel grain) or a band-pass
// at the model's own scale, taken as the output's band energy minus the input's. Both are kept per step for the log.
//
// The weighting is a heuristic, not a published metric; the numbers per step are logged so it can be refitted.
//
// Movement does not end a run at once. A measured evaluation whose input moved (inputChange over motionLimit) sends the
// sweep back to that step: its measurements and every later step's are dropped, it settles again and is measured again,
// up to motionRetries times per step. Results issued before the rewind come back tagged with the old generation and are
// ignored. The input's own band detail (the game's frame, measured at one scale for every step) also marks where the
// camera is: if a step's mean drifts from the run's first step by more than sceneTolerance, the camera came to rest
// somewhere else and the sweep starts over, sceneRestarts times at most. Only then does movement stop the run.
//
// The same machinery measures without tuning: "Measure detail" (MeasureSettings) is a run of one step at the current
// value, shown the live exposure rather than a pinned one, measured over many evaluations and not scored -- the numbers
// an A/B of any setting is judged by, on a still scene.
//
// Also here, so the host test covers them: ReduceGrid (the grid -> Stats) and Availability (when a run may start or go
// on). Pure CPU, no D3D: DlssNr_ExposureCalibrate_Run.h drives it for both backends, the menu shows it,
// tests/nr_exposure_calibrate_smoke.cpp checks it.

#include "DlssNr_TrimAnchors.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace DlssNrExposureCalibrate
{
// The menu's 0 EV for Automatic exposure: Trim 5x. EV = -log2(trim / neutral), + is brighter (DlssNr_Menu.cpp
// TrimToEv / EvToTrim). Game exposure's slider is centred on Trim 1x (the game's exposure as is) instead.
constexpr float kNeutralTrim = 5.0f;
constexpr float kGameExposureNeutralTrim = 1.0f;

inline float TrimForEv(float ev, float neutral = kNeutralTrim) { return DlssNrTrim::ClampTrim(neutral * std::exp2(-ev)); }
inline float EvForTrim(float trim, float neutral = kNeutralTrim)
{
    return -std::log2(std::max(trim, DlssNrTrim::kMinTrim) / neutral);
}

// An EV for display: "%+.1f" prints a tiny negative (or -0, which a fast floating-point build will not turn into +0
// by adding 0) as "-0.0". Anything that rounds to zero is shown positive.
inline float Tidy(float ev) { return std::fabs(ev) < 0.05f ? std::fabs(ev) : ev; }

// How far Follow-game's base white point (the game's times its learned calibration) sits from Automatic's own, in EV.
// Following is meant to land where Automatic would; NBA 2K27 kept a -3.42 EV calibration learned in other conditions
// and sat 4.3 EV off, so Tune's steps were all 4.3 EV brighter than their labels. 0 when either is unknown.
constexpr float kFollowDisagreementLimitEv = 1.0f;

inline float BaseDisagreementEv(float followed, float automatic)
{
    return followed > 0.0f && automatic > 0.0f ? std::log2(followed / automatic) : 0.0f;
}

enum class Detail : uint32_t
{
    Raw = 0,
    BandPass = 1,
};

enum class Abort : uint32_t
{
    None = 0,
    Cancelled,
    Motion,        // the input changed between measured evaluations: the camera or the scene moved
    Resolution,    // the frame size changed
    NrOff,         // NR was switched off, or stopped running
    SourceChanged, // the white point source is no longer the one being calibrated
    ExposureMoved, // the base white point (Automatic's metering, or the followed game exposure) moved
    Unavailable,   // a run could not go on for longer than Settings::unavailableTolerance (StopBlocker says why)
    NothingMeasured, // every measurement came back empty (another NR path ran, or the stats pass failed)
    TooFewMeasured,  // a measure got fewer than half its evaluations back: not a number to compare with
};

inline const char* AbortText(Abort a)
{
    switch (a)
    {
    case Abort::None:
        return "";
    case Abort::Cancelled:
        return "cancelled";
    case Abort::Motion:
        return "the picture kept moving: hold the camera still and try again";
    case Abort::Resolution:
        return "the resolution changed";
    case Abort::NrOff:
        return "NR stopped running";
    case Abort::SourceChanged:
        return "the exposure source changed";
    case Abort::ExposureMoved:
        return "the exposure changed: hold the camera still and try again";
    case Abort::Unavailable:
        return "it could not go on";
    case Abort::NothingMeasured:
        return "nothing could be measured; nothing changed";
    case Abort::TooFewMeasured:
        return "too few frames could be measured: try again";
    }
    return "";
}

// Why a run cannot start, or go on, on the current evaluation.
enum class Blocker : uint32_t
{
    None = 0,
    Source,          // not Automatic exposure or Game exposure
    ProxyMode,       // the proxy backend runs the model on another path; the stats pass never runs
    HoldFrame,       // frame hold pins its own white point and freezes the input
    FinishedPicture, // finished-picture mode keeps its own display white point
    NotHdr,          // an already tone-mapped frame has no scene exposure to tune
    AutoNotRunning,  // Automatic's meter has no reading this evaluation
    NoGameExposure,  // Game exposure: the game supplied no exposure texture, or there is no reading yet
    NrStopped,       // NR has not run for a while
    ColourConverted, // Colour encoding converts the frame (gamma 2.2, PQ): the measurement would mix encodings
};

inline const char* BlockerText(Blocker b)
{
    switch (b)
    {
    case Blocker::None:
        return "";
    case Blocker::Source:
        return "only for Automatic exposure and Game exposure";
    case Blocker::ProxyMode:
        return "not while NR runs through the proxy backend";
    case Blocker::HoldFrame:
        return "not while the frame is held";
    case Blocker::FinishedPicture:
        return "not in finished-picture mode";
    case Blocker::NotHdr:
        return "the game's frame is not linear HDR";
    case Blocker::AutoNotRunning:
        return "Automatic exposure has no reading";
    case Blocker::NoGameExposure:
        return "the game supplies no exposure on this frame";
    case Blocker::NrStopped:
        return "NR is not running";
    case Blocker::ColourConverted:
        return "not while Colour Encoding converts the game's colour (gamma 2.2 or PQ)";
    }
    return "";
}

// What availability depends on, gathered by the caller each evaluation. The Follow fields matter to Automatic only,
// and only to RelearnFollowOnStart: Follow never blocks a run. Nor do Trim anchors: a run pins its own white point
// (the frozen base times the step's Trim), so it measures the same with or without them, and its result is offered as a
// point on the table.
struct Situation
{
    uint32_t source = 0;
    bool proxyMode = false;
    bool holdFrame = false;
    bool finishedPicture = false;
    bool hdr = false;
    bool autoRunning = false;
    bool followLocked = false;         // Follow the game's exposure is on and has a learned calibration
    float followDisagreementEv = 0.0f; // while following: its base against Automatic's own, in EV (for the log)
    bool gameExposureNow = false;
    bool gameExposureReading = false;
    // The game's own exposure as a white point (PreExposure / exposure), 0 without one: Natural's floor is put on the
    // tuned slider through it (Context::gameBaseWhitePoint). Read whatever the source.
    float gameBaseWhitePoint = 0.0f;
    bool naturalTarget = false;   // DlssNr TuneTarget 1: Natural (Settings::floorBelowGameEv)
    bool colourConverted = false; // DlssNrColourEncoding::ShaderConverts for this frame
    bool beforeSrSet = false;     // RunBeforeSR is on: a Tune runs after SR instead (TuneRunsAfterSr), so it waits
                                  // for NR to settle there before its first step
};

inline Blocker Availability(const Situation& s)
{
    if (s.source != 3 && s.source != 1)
        return Blocker::Source;
    if (s.proxyMode)
        return Blocker::ProxyMode;
    if (s.holdFrame)
        return Blocker::HoldFrame;
    if (s.finishedPicture)
        return Blocker::FinishedPicture;
    if (s.colourConverted)
        return Blocker::ColourConverted;
    if (!s.hdr)
        return Blocker::NotHdr;

    if (s.source == 1)
    {
        if (!s.gameExposureNow || !s.gameExposureReading)
            return Blocker::NoGameExposure;
    }
    else
    {
        // Follow the game's exposure does not block: a run tunes against Automatic's own exposure, whatever Follow is
        // doing (see RelearnFollowOnStart).
        if (!s.autoRunning)
            return Blocker::AutoNotRunning;
    }

    return Blocker::None;
}

// Whether starting a run on Automatic re-learns Follow the game's exposure. A run tunes the Trim against Automatic's
// own base white point; once Follow is back in force the same Trim multiplies the followed base (the game's times
// the learned calibration). The two agree only as well as the calibration holds, and any disagreement lands in the
// picture after the run (4.3 EV in NBA 2K27, 3.2 EV in RDR2; even 0.9 EV is almost two steps), so a learned
// calibration is always learned again, from Automatic as it is now, during the run -- whether it is following at that
// moment or not (a frame without the game's exposure). Learning takes about two seconds, well inside a run. Still
// learning (not locked yet) has nothing to re-learn.
inline bool RelearnFollowOnStart(const Situation& s) { return s.source == 3 && s.followLocked; }

struct Settings
{
    float minEv = -3.0f;
    float maxEv = 4.0f;
    float stepEv = 0.5f;
    // Tune target Natural (DlssNr TuneTarget 1): no step darker than this many EV below the game's own exposure
    // (Context::gameBaseWhitePoint) is swept or offered; 0 is Max detail, no floor. Why: the detail score rises at
    // every darker step whenever nothing clips, so it runs to the dark end. In offline tests on captured game frames
    // (2026-10-06) the pick sat 6.3 dB from the step closest to the model fed the game's linear colour as is, the way
    // the game integrations hand it over, in The Witcher 3; with a floor 1.5 EV under the game's exposure every scene
    // landed within 0.1 dB of it, 0.4 dB in all (1 EV: 2.4, 2 EV: 2.3). Measures of fidelity to the game's frame did not do it: NVIDIA's own output moves the coarse
    // tone as much as our steps do. A choice, not the rule: NBA 2K27's tuned -3.5 EV, preferred by eye, sits under it.
    float floorBelowGameEv = 0.0f;
    unsigned settle = 8;
    // The first step jumps from the current value to the bottom of the sweep (4.5 EV from the +1.5 default); after 8
    // evaluations the model's history still flickered 5x the rest in NBA 2K27.
    unsigned firstSettle = 24;
    unsigned measure = 4; // even, so Reuse bottleneck's computed/reused alternation is covered equally
    Detail detail = Detail::BandPass;
    float flickerWeight = 1.0f;
    float damageWeight = 2.0f; // the shoulder share (highlights the model was shown compressed)
    // The floor share (the model was shown near black). Half the shoulder's: in The Witcher 3 and Cyberpunk 2077
    // (2026-09-30) the steps with a floor had their shadows lifted in the output (up to 23%), and crushing grew toward the
    // bright steps, where the floor is empty -- the output's own shadow terms below catch what this was meant to.
    float floorWeight = 1.0f;
    // A step whose output is more or less saturated than the game's input loses this times the fraction (0.5: 10% more or
    // less chroma costs 0.05). NBA 2K27, The Witcher 3 and Cyberpunk 2077 desaturated at every step, 3-35%, least at the
    // darker steps; Cyberpunk's colour read 14% apart between the passes at its darkest step (the scene's own lights), so
    // the weight stays low enough that noise cannot turn a pass. A rail against a model that recolours, not a steer.
    //
    // Tried at 1.0 and put back (2026-09-30): the picture was read as warm, but warmth is not a property of the exposure.
    // Two Witcher 3 scenes disagreed in sign -- a dark one peaked warm (+0.0073) near 0 EV with saturation +20% at
    // -1.5 EV, while another read cool at every step (-0.0010 to -0.0043), cooler the brighter it went. Raising this
    // moves the result toward whichever step recolours least, which lines up with warmth differently in every scene, and
    // at 1.0 it changed the second scene's outcome not at all (the colour term spans 0.18 of a 1.7 score range there;
    // shoulder and crushing decide it). Warmth would need a term of its own to be steered, and this weight cannot stand
    // in for one.
    float colourWeight = 0.5f;
    // The output's shadows (the input below ~2% of white). Darkened loses this times the share of the picture in shadow
    // times the fraction it darkened -- lifting is free, and a frame with hardly any shadow cannot veto a step on the
    // strength of a handful of pixels (1.0: half the picture in shadow, left 20% darker, costs 0.1). Crushed is the
    // share of the SHADOWS the model took to under half their level, and loses crushWeight times that share (10: 1% of
    // the picture crushed costs 0.1). The Witcher 3's dark scene crushed 0.02% at -3 EV and 4.3% at +4 EV.
    float shadowWeight = 1.0f;
    float crushWeight = 10.0f;
    // Mean absolute input change between evaluations, display-encoded. A still NBA 2K27 scene measured 0.0002, a
    // moving one 0.003-0.012 (which chose nonsense at the old 0.05).
    float motionLimit = 0.001f;
    // A step whose input moved is measured again, this many times at most, before movement stops the run.
    unsigned motionRetries = 3;
    // How far a step's mean input band detail may drift from the run's first step (relative) before the view counts as
    // changed. A still NBA 2K27 scene read the same to 4 digits at every step; a still Cyberpunk 2077 camera with ray
    // reconstruction drifted up to 7% from its noise alone.
    float sceneTolerance = 0.12f;
    // How many times a changed view starts the sweep over before movement stops the run.
    unsigned sceneRestarts = 1;
    float flatTolerance = 0.02f; // a best score this close to the current step's keeps the current value
    // A run is this many passes over the same steps, and it offers a change only when they agree (within one step):
    // one pass is not evidence enough. Single runs in the same place have jumped -- NBA 2K27 +3.5 EV, then 0.0 EV;
    // RDR2 +0.5 EV (flat), then +2.5 EV 30 seconds later (2026-09-30 logs) -- and a far pick that "popped" in one
    // scene crushed blacks and oversaturated others (user report). Measure detail is always one pass.
    unsigned passes = 2; // 1 or 2 only: Finish compares the last two, so a third would be silently dropped
    // Unsure when detail varies across the steps no more than unsureNoise times the input band's own spread across
    // them (at least noiseFloor, the stats' last printed digit): the input is the same frame at every step.
    float unsureNoise = 4.0f;
    float noiseFloor = 1e-5f;
    float baseToleranceEv = 0.25f; // how far the live base white point may drift from the frozen one
    float neutralTrim = kNeutralTrim; // the slider's 0 EV, and the scale detail is measured at
    // Consecutive evaluations a run may be unavailable (the game dropping its exposure texture for a frame, Automatic's
    // meter missing one) before it aborts; meanwhile it holds its step and measures nothing.
    unsigned unavailableTolerance = 30;
    // "Measure detail": one step at the current value, the white point left live (Frame::override stays off, so the
    // picture is exactly what the settings give), measured and not scored. Movement is handled as in a sweep.
    bool measureOnly = false;
};

// Game exposure (white point source 1): its slider's 0 EV is the game's exposure as is, and a pre-exposed game's own
// exposure sits well above Automatic's (NBA 2K27: about 3 EV), so the sweep reaches further down, within the slider's
// -5.6 .. +2.0 EV.
inline Settings GameExposureSettings()
{
    Settings s;
    s.neutralTrim = kGameExposureNeutralTrim;
    s.minEv = -5.5f;
    s.maxEv = 2.0f;
    return s;
}

// Tune target Natural's floor (Settings::floorBelowGameEv).
constexpr float kNaturalFloorEv = 1.5f;

// "Measure detail" on the white point source `source` (its neutral is the scale detail is measured at, as in a sweep):
// 8 evaluations for the copies to fill, then 60 measured, about a second at 60 fps. Even, like a sweep's step.
constexpr unsigned kMeasureEvaluations = 60;
inline Settings MeasureSettings(uint32_t source)
{
    Settings s = source == 1 ? GameExposureSettings() : Settings {};
    s.measureOnly = true;
    s.passes = 1;
    s.firstSettle = s.settle;
    s.measure = kMeasureEvaluations;
    return s;
}

// What the sweep checks is still the same situation it started in.
struct Context
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t whitePointSource = 0;
    bool nrEnabled = false;
    // The base white point the Trim multiplies (PreExposure / exposure: the game's for Game exposure, Automatic's own
    // metering for Automatic, following the game's exposure or not), 0 when unknown. Frozen at Start: every step is
    // shown the frozen base times its own Trim, so the steps differ only by the Trim even if the exposure moves; a
    // move beyond baseToleranceEv aborts.
    float baseWhitePoint = 0.0f;
    // Why the run could not go on this evaluation (None when it can). Tolerated briefly; see unavailableTolerance.
    Blocker blocker = Blocker::None;
    // The game's own exposure as a white point (PreExposure / exposure), 0 when the game gives none. Whatever the
    // source: Natural's floor is put on the tuned slider through it. Frozen at Start like the base.
    float gameBaseWhitePoint = 0.0f;
};

// One measured evaluation, reduced over the whole frame. Detail and change are display-encoded luma.
struct Stats
{
    float detailRaw = 0.0f;    // output: mean |Laplacian|
    float detailBand = 0.0f;   // output: mean |difference of Gaussians| at the model's scale
    float inputBand = 0.0f;    // the same band on the input (the game's frame before NR)
    float outputChange = 0.0f; // mean |output - previous output|
    float inputChange = 0.0f;  // mean |input - previous input|
    float shoulder = 0.0f;     // share of the encoded model input above the shoulder
    float floor = 0.0f;        // share of the encoded model input in the floor
    // Colour, output against input at the frozen white point (dlssnr_detail_stats.hlsl): OkLab chroma of each, their
    // distance in OkLab's a-b plane, and the output's b minus the input's (+ warmer).
    float chromaIn = 0.0f;
    float chromaOut = 0.0f;
    float colourShift = 0.0f;
    float warmth = 0.0f;
    // Shadows (the input's display-encoded luma below the stats pass's kShadowLevel, ~2% of white), each divided by the
    // pixels measured: the share in the shadows, their input and output levels summed, the share crushed (a pixel not
    // black on input brought to under half its level).
    float shadowShare = 0.0f;
    float shadowIn = 0.0f;
    float shadowOut = 0.0f;
    // Of the whole picture, but counted among the shadows alone: a pixel the model took below half its level, of those
    // whose input sits above ~2% of white and inside the shadow band (dlssnr_detail_stats.hlsl). A midtone halved is
    // not crushed by this measure.
    float crushed = 0.0f;
};

inline float AddedBand(const Stats& s) { return s.detailBand - s.inputBand; }

// The stats pass's grid: 64x64 tiles, each four RGBA32F texels side by side -- (x, y) raw detail, band out, band in,
// output change; (x + 64, y) input change, shoulder share, floor share, pixels measured; (x + 128, y) chroma in, chroma
// out, colour shift, warmth; (x + 192, y) shadow share, shadow level in, out, crushed share. A row is 256 * 4 floats.
constexpr unsigned kGridTiles = 64;
constexpr unsigned kGridColumns = 4; // texels per tile
constexpr unsigned kGridRowFloats = kGridTiles * kGridColumns * 4;

// The grid -> one Stats: tile means weighted by the pixels each measured, empty tiles left out. A grid with nothing in
// it is an empty sample (detailBand NaN), which AddStats drops, not a zero.
inline Stats ReduceGrid(const float* grid)
{
    double sum[15] = {};
    double pixels = 0.0;

    for (unsigned y = 0; y < kGridTiles; ++y)
    {
        const float* row = grid + (size_t) y * kGridRowFloats;

        for (unsigned x = 0; x < kGridTiles; ++x)
        {
            const float* a = row + x * 4;
            const float* b = row + (x + kGridTiles) * 4;
            const float* c = row + (x + 2 * kGridTiles) * 4;
            const float* d = row + (x + 3 * kGridTiles) * 4;
            const double n = b[3];

            if (!(n > 0.0) || !std::isfinite(n))
                continue;

            sum[0] += a[0] * n;
            sum[1] += a[1] * n;
            sum[2] += a[2] * n;
            sum[3] += a[3] * n;
            sum[4] += b[0] * n;
            sum[5] += b[1] * n;
            sum[6] += b[2] * n;
            for (int i = 0; i < 4; ++i)
            {
                sum[7 + i] += c[i] * n;
                sum[11 + i] += d[i] * n;
            }
            pixels += n;
        }
    }

    Stats s {};

    if (pixels <= 0.0)
    {
        s.detailBand = NAN;
        return s;
    }

    s.detailRaw = (float) (sum[0] / pixels);
    s.detailBand = (float) (sum[1] / pixels);
    s.inputBand = (float) (sum[2] / pixels);
    s.outputChange = (float) (sum[3] / pixels);
    s.inputChange = (float) (sum[4] / pixels);
    s.shoulder = (float) (sum[5] / pixels);
    s.floor = (float) (sum[6] / pixels);
    s.chromaIn = (float) (sum[7] / pixels);
    s.chromaOut = (float) (sum[8] / pixels);
    s.colourShift = (float) (sum[9] / pixels);
    s.warmth = (float) (sum[10] / pixels);
    s.shadowShare = (float) (sum[11] / pixels);
    s.shadowIn = (float) (sum[12] / pixels);
    s.shadowOut = (float) (sum[13] / pixels);
    s.crushed = (float) (sum[14] / pixels);
    return s;
}

inline bool Finite(const Stats& s)
{
    return std::isfinite(s.detailRaw) && std::isfinite(s.detailBand) && std::isfinite(s.inputBand) &&
           std::isfinite(s.outputChange) && std::isfinite(s.inputChange) && std::isfinite(s.shoulder) &&
           std::isfinite(s.floor) && std::isfinite(s.chromaIn) && std::isfinite(s.chromaOut) &&
           std::isfinite(s.colourShift) && std::isfinite(s.warmth) && std::isfinite(s.shadowShare) &&
           std::isfinite(s.shadowIn) && std::isfinite(s.shadowOut) && std::isfinite(s.crushed);
}

struct Ticket
{
    uint32_t run = 0;
    uint32_t step = 0;
    uint32_t generation = 0; // bumped when the sweep goes back after movement: older results are dropped
};

// Something a run did that the log should say: a step measured again, or the sweep started over.
struct Note
{
    enum class Kind : uint32_t
    {
        Retry,   // the step moved and is measured again
        Restart, // the view changed and the sweep starts over
    };
    Kind kind = Kind::Retry;
    float ev = 0.0f;       // the step
    float value = 0.0f;    // Retry: the input change; Restart: the input band now
    float against = 0.0f;  // Retry: the motion limit; Restart: the run's first input band
    unsigned count = 0;    // Retry: which retry of this step; Restart: which restart
};

// What one evaluation should do.
struct Frame
{
    bool override = false; // use `ev` instead of the configured Trim
    bool capture = false;  // part of a run: copy the input and the output (a measure's evaluations do, unpinned)
    float ev = 0.0f;
    bool measure = false; // dispatch the stats pass and hand its result back with `ticket`
    Ticket ticket {};
};

// The means of one step's measured evaluations.
struct StepResult
{
    float ev = 0.0f;
    unsigned samples = 0;
    float detailRaw = 0.0f;
    float detailBand = 0.0f;
    float inputBand = 0.0f;
    float outputChange = 0.0f;
    float inputChange = 0.0f;
    float shoulder = 0.0f;
    float floor = 0.0f;
    float chromaIn = 0.0f;
    float chromaOut = 0.0f;
    float colourShift = 0.0f;
    float warmth = 0.0f;
    float shadowShare = 0.0f;
    float shadowIn = 0.0f;
    float shadowOut = 0.0f;
    // Of the whole picture, but counted among the shadows alone: a pixel the model took below half its level, of those
    // whose input sits above ~2% of white and inside the shadow band (dlssnr_detail_stats.hlsl). A midtone halved is
    // not crushed by this measure.
    float crushed = 0.0f;
    // Sums of squared deviations of the step's samples (Welford), for each detail measure: how much they spread.
    float spreadRaw = 0.0f;
    float spreadBand = 0.0f;

    // Chroma below this is a grey picture: nothing to compare, and a ratio taken there is noise (ColourWords uses the
    // same figure). Shadow below this share of the picture is too little to score, the figure ShadowWords declines at.
    static constexpr float kGreyChroma = 1e-4f;
    static constexpr float kShadowShareFloor = 0.005f;

    float Flicker() const { return std::max(outputChange - inputChange, 0.0f); }
    // How much more saturated the output is than the input (0.1 = 10% more chroma; negative is less).
    // Below kGreyChroma the picture has no colour worth comparing (the same floor ColourWords reads), and the ratio is
    // bounded: a hundredth of the chroma of a grey frame would otherwise divide into a huge score.
    float Saturation() const
    {
        return chromaIn > kGreyChroma ? std::clamp(chromaOut / chromaIn - 1.0f, -1.0f, 1.0f) : 0.0f;
    }
    // How much darker the model left the shadows than it found them (0.2 = 20% darker; negative is lifted). A ratio of
    // sums over the shadow pixels alone, so it says nothing about how much of the picture is in shadow: ScoreIn weighs
    // it by shadowShare, and below kShadowShareFloor there is not enough shadow to read at all.
    float ShadowDarkening() const { return shadowIn > 1e-6f ? 1.0f - shadowOut / shadowIn : 0.0f; }
    // What the shadows cost this step: how much of the picture is shadow times how much darker it was left.
    float ShadowCost() const
    {
        return shadowShare >= kShadowShareFloor ? shadowShare * std::max(ShadowDarkening(), 0.0f) : 0.0f;
    }
    float DetailOf(Detail d) const { return d == Detail::Raw ? detailRaw : detailBand - inputBand; }

    // The standard error of the step's mean detail, from its samples' spread (0 with fewer than two). Its samples are
    // consecutive evaluations, which are correlated, so this reads low; the unsure rule allows for that.
    float MeanError(Detail d) const
    {
        if (samples < 2)
            return 0.0f;
        const float spread = d == Detail::Raw ? spreadRaw : spreadBand;
        return std::sqrt(std::max(spread, 0.0f) / (float) (samples - 1) / (float) samples);
    }
};

// What one pass over the steps concluded (the result is the current value unless a step clearly won).
struct PassVerdict
{
    float result = 0.0f;
    bool unsure = false;
    float bestRaw = 0.0f;
    float bestBand = 0.0f;
    // The flat top around the best step: the run of measured steps on either side scoring within flatTolerance of it.
    float topLo = 0.0f;
    float topHi = 0.0f;
};

class Sweep
{
  public:
    void Start(float currentEv, const Settings& settings, const Context& context)
    {
        ++run_;
        settings_ = settings;
        // Finish compares the last two passes, so a third would be judged against the second and the first silently
        // dropped. One (Measure detail) or two.
        settings_.passes = std::clamp(settings_.passes, 1u, 2u);
        context_ = context;
        currentEv_ = currentEv;
        abort_ = Abort::None;
        finished_ = false;
        unsure_ = false;
        atLimit_ = false;
        unrepeated_ = false;
        pass_ = 0;
        first_ = second_ = PassVerdict {}; // nothing of the last run's passes survives into the next
        firstSteps_.clear();
        first_ = second_ = PassVerdict {};
        stopBlocker_ = Blocker::None;
        unavailable_ = 0;
        step_ = 0;
        frameInStep_ = 0;
        outstanding_ = 0;
        generation_ = 0;
        restarts_ = 0;
        sceneBand_ = -1.0f;
        lastInputChange_ = 0.0f;
        lastSceneBand_ = -1.0f;
        shownEv_ = currentEv;
        longSettle_ = false;
        notes_.clear();
        steps_.clear();

        const float lo = std::max(settings.minEv, EvForTrim(DlssNrTrim::kMaxTrim, settings.neutralTrim));
        const float hi = std::min(settings.maxEv, EvForTrim(DlssNrTrim::kMinTrim, settings.neutralTrim));
        const float step = settings.stepEv > 0.01f ? settings.stepEv : 0.5f;

        // Natural's floor on this slider: the step whose white point is the game's own, floorBelowGameEv darker. Its
        // trim is gameBase / base (Game exposure: 1, so the floor is -floorBelowGameEv). Never above the second-last
        // step, so a sweep is left. Steps keep the usual grid; the first one swept is the first at or above it.
        floorEv_ = -std::numeric_limits<float>::infinity();
        if (!settings.measureOnly && settings.floorBelowGameEv > 0.0f && context.gameBaseWhitePoint > 0.0f &&
            context.baseWhitePoint > 0.0f)
        {
            const float gameEv = -std::log2(context.gameBaseWhitePoint / context.baseWhitePoint / settings.neutralTrim);
            floorEv_ = std::min(gameEv - settings.floorBelowGameEv, hi - step);
        }
        anchorEv_ = std::max(currentEv, floorEv_);

        if (settings.measureOnly)
        {
            StepResult r;
            r.ev = currentEv;
            steps_.push_back(r);
        }
        else
        {
            for (int i = 0; lo + i * step <= hi + 1e-4f; ++i)
            {
                if (lo + i * step < floorEv_ - 1e-4f)
                    continue;
                StepResult r;
                r.ev = lo + i * step;
                steps_.push_back(r);
            }
        }

        retries_.assign(steps_.size(), 0);
        running_ = !steps_.empty() && settings.measure > 0;
    }

    void Cancel() { Stop(Abort::Cancelled); }

    // Ended from outside: NR stopped calling in, for one.
    void Abandon(Abort reason) { Stop(reason); }

    // Forget a finished or aborted run once its result has been dealt with. Not while running.
    void Clear()
    {
        if (running_)
            return;
        finished_ = false;
        unsure_ = false;
        atLimit_ = false;
        unrepeated_ = false;
        pass_ = 0;
        first_ = second_ = PassVerdict {}; // nothing of the last run's passes survives into the next
        firstSteps_.clear();
        abort_ = Abort::None;
        steps_.clear();
    }

    // Called once per NR evaluation, before the model runs.
    Frame NextFrame(const Context& now)
    {
        Frame f;

        if (!running_)
            return f;

        if (!now.nrEnabled)
            return Stop(Abort::NrOff), f;
        if (now.width != context_.width || now.height != context_.height)
            return Stop(Abort::Resolution), f;
        if (now.whitePointSource != context_.whitePointSource)
            return Stop(Abort::SourceChanged), f;

        // Briefly unavailable: hold the step, measure nothing, and skip the base check (a fallback base can jump
        // meanwhile). For long, abort with the reason.
        if (now.blocker != Blocker::None)
        {
            if (++unavailable_ > settings_.unavailableTolerance)
            {
                stopBlocker_ = now.blocker;
                return Stop(Abort::Unavailable), f;
            }

            f.override = !settings_.measureOnly;
            f.capture = true;
            f.ev = step_ < steps_.size() ? steps_[step_].ev : currentEv_;
            return f;
        }

        unavailable_ = 0;

        if (context_.baseWhitePoint > 0.0f && now.baseWhitePoint > 0.0f &&
            std::fabs(std::log2(now.baseWhitePoint / context_.baseWhitePoint)) > settings_.baseToleranceEv)
            return Stop(Abort::ExposureMoved), f;

        f.override = !settings_.measureOnly;
        f.capture = true;

        // Every step issued: back at the current value while the last results come home.
        if (step_ >= steps_.size())
        {
            f.ev = currentEv_;
            shownEv_ = f.ev;
            return f;
        }

        f.ev = steps_[step_].ev;
        shownEv_ = f.ev;

        if (frameInStep_ >= SettleOf(step_))
        {
            f.measure = true;
            f.ticket = { run_, (uint32_t) step_, generation_ };
            ++outstanding_;
        }

        if (++frameInStep_ >= SettleOf(step_) + settings_.measure)
        {
            frameInStep_ = 0;
            longSettle_ = false;
            ++step_;
        }

        return f;
    }

    // A measured evaluation's stats, whenever the readback delivers them.
    void AddStats(const Ticket& ticket, const Stats& s)
    {
        if (!running_ || ticket.run != run_ || ticket.step >= steps_.size() || ticket.generation != generation_)
            return;

        if (outstanding_ > 0)
            --outstanding_;

        if (Finite(s))
        {
            if (s.inputChange > settings_.motionLimit)
                return Moved(ticket.step, s.inputChange);

            StepResult& r = steps_[ticket.step];
            const float n = (float) r.samples;
            const float rawBefore = r.DetailOf(Detail::Raw), bandBefore = r.DetailOf(Detail::BandPass);
            auto mean = [n](float& m, float v) { m = (m * n + v) / (n + 1.0f); };
            mean(r.detailRaw, s.detailRaw);
            mean(r.detailBand, s.detailBand);
            mean(r.inputBand, s.inputBand);
            mean(r.outputChange, s.outputChange);
            mean(r.inputChange, s.inputChange);
            mean(r.shoulder, s.shoulder);
            mean(r.floor, s.floor);
            mean(r.chromaIn, s.chromaIn);
            mean(r.chromaOut, s.chromaOut);
            mean(r.colourShift, s.colourShift);
            mean(r.warmth, s.warmth);
            mean(r.shadowShare, s.shadowShare);
            mean(r.shadowIn, s.shadowIn);
            mean(r.shadowOut, s.shadowOut);
            mean(r.crushed, s.crushed);
            ++r.samples;

            // Welford: the means above moved from `before` to the step's DetailOf now.
            const float rawNow = s.detailRaw, bandNow = s.detailBand - s.inputBand;
            r.spreadRaw += (rawNow - rawBefore) * (rawNow - r.DetailOf(Detail::Raw));
            r.spreadBand += (bandNow - bandBefore) * (bandNow - r.DetailOf(Detail::BandPass));

            // Where the camera is, judged on whole steps: a single reading is noisier than a step's mean.
            if (r.samples == settings_.measure)
            {
                if (sceneBand_ < 0.0f)
                    sceneBand_ = r.inputBand;
                else if (std::fabs(r.inputBand - sceneBand_) > settings_.sceneTolerance * sceneBand_)
                    return ViewChanged(ticket.step, r.inputBand);
            }
        }

        if (step_ >= steps_.size() && outstanding_ == 0)
            Finish();
    }

    bool Running() const { return running_; }
    bool Finished() const { return finished_; }
    Abort AbortReason() const { return abort_; }
    size_t StepCount() const { return steps_.size(); }
    const std::vector<StepResult>& Steps() const { return steps_; }
    const Settings& Config() const { return settings_; }
    float CurrentEv() const { return currentEv_; }
    // Natural's floor on the tuned slider, when it applied to this run (Settings::floorBelowGameEv). NaturalWanted
    // without HasFloor: Natural was asked for but the game gave no exposure to put it on, so the run was Max detail.
    bool NaturalWanted() const { return settings_.floorBelowGameEv > 0.0f && !settings_.measureOnly; }
    bool HasFloor() const { return std::isfinite(floorEv_); }
    float FloorEv() const { return floorEv_; }

    // The base white point frozen at Start (0 when it was unknown), and the white point a step is shown with.
    float FrozenBase() const { return context_.baseWhitePoint; }
    float WhitePointFor(float ev) const
    {
        return std::clamp(context_.baseWhitePoint * TrimForEv(ev, settings_.neutralTrim), 0.01f, 4096.0f);
    }

    // The scale detail is measured on: the frozen base at the slider's 0 EV (Automatic's middle-grey metering, or the
    // game's own exposure for Game exposure), the same whatever
    // the slider was at. Measuring at the starting value made the result follow the start: from -3.3 EV the scale
    // squashed the shadows and darker steps won, from +4.3 EV it flattened the highlights and brighter steps won.
    float MeasureWhitePoint() const { return WhitePointFor(0.0f); }

    // The step on screen while running (StepCount() while the last results come home).
    size_t StepIndex() const { return step_; }
    float StepEv() const { return step_ < steps_.size() ? steps_[step_].ev : currentEv_; }

    // Why an Abort::Unavailable run could not go on.
    Blocker StopBlocker() const { return stopBlocker_; }

    // For an Abort::Motion stop: the input change that ended it (0 when the view changed instead), and the input band
    // the changed view read against the run's first (-1 when it was not a view change).
    float LastInputChange() const { return lastInputChange_; }
    float LastSceneBand() const { return lastSceneBand_; }
    float SceneBand() const { return sceneBand_; }

    // What the run did since the last call, for the log.
    std::vector<Note> TakeNotes() { return std::exchange(notes_, {}); }

    // Finished, but detail varied across the steps no more than the measurement's own noise: the result is the
    // current value.
    bool Unsure() const { return unsure_; }

    // Whether the result offered is the first or last step the run measured. Logged, never a reason to keep the current
    // value: the range is a limit, so the best step inside it is the best there is to offer.
    bool AtLimit() const { return atLimit_; }

    // The raw measure's best, for the menu's "Apply raw instead" -- and whether the passes agree on it. BestEv reads the
    // last pass alone, so without this the button would offer a value only one pass ever proposed while the run itself
    // was offering another (found in review, 2026-09-30).
    bool RawAgreed() const
    {
        if (!finished_ || unsure_ || unrepeated_ || firstSteps_.empty())
            return pass_ == 0; // a one-pass run (Measure detail) has nothing to disagree with
        const int a = BestIn(firstSteps_, Detail::Raw), b = BestIn(steps_, Detail::Raw);
        if (a < 0 || b < 0)
            return false;
        const float step = settings_.stepEv > 0.01f ? settings_.stepEv : 0.5f;
        return std::fabs(firstSteps_[(size_t) a].ev - steps_[(size_t) b].ev) <= step + 1e-3f;
    }

    // Finished, but the passes came to results more than a step apart: none is offered, the result is the current
    // value (Settings::passes).
    bool Unrepeated() const { return unrepeated_; }

    // The passes: how many a run makes, which one is on (0-based), and what each concluded -- the first once it is
    // done (its steps kept for the log; the sweep's own Steps() are the pass on now, or the last), the last once the run
    // has finished.
    unsigned Passes() const { return settings_.measureOnly ? 1u : std::max(settings_.passes, 1u); }
    unsigned Pass() const { return pass_; }
    const std::vector<StepResult>& FirstPassSteps() const { return firstSteps_; }
    const PassVerdict& FirstPass() const { return first_; }
    const PassVerdict& LastPass() const { return second_; }

    // Progress 0..1 for the menu, over every pass.
    float Progress() const
    {
        if (steps_.empty())
            return 0.0f;
        const float per = (float) (settings_.settle + settings_.measure);
        const float first = (float) (SettleOf(0) - settings_.settle);
        const float perPass = first + steps_.size() * per;
        const float done = pass_ * perPass + (step_ == 0 ? frameInStep_ : first + step_ * per + frameInStep_);
        return std::min(1.0f, done / (Passes() * perPass));
    }

    float Score(size_t i, Detail d) const { return ScoreIn(steps_, i, d); }

    // A step's score among `steps` (a pass): its detail against the pass's best, less flicker and damage.
    float ScoreIn(const std::vector<StepResult>& steps, size_t i, Detail d) const
    {
        float top = 0.0f;
        for (const StepResult& r : steps)
            if (r.samples > 0)
                top = std::max(top, r.DetailOf(d));

        const StepResult& r = steps[i];
        const float scale = top > 1e-9f ? top : 1.0f;
        return std::max(r.DetailOf(d), 0.0f) / scale - settings_.flickerWeight * r.Flicker() / scale -
               settings_.damageWeight * r.shoulder - settings_.floorWeight * r.floor -
               settings_.colourWeight * std::fabs(r.Saturation()) -
               settings_.shadowWeight * r.ShadowCost() - settings_.crushWeight * r.crushed;
    }

    // The best step's EV under a measure, or the current value when nothing was measured.
    float BestEv(Detail d) const { return BestEvIn(steps_, d); }
    float BestEvIn(const std::vector<StepResult>& steps, Detail d) const
    {
        const int best = BestIn(steps, d);
        return best < 0 ? currentEv_ : steps[(size_t) best].ev;
    }

    // The chosen value: the best step under the selected measure, unless the curve is flat around the current value.
    float ResultEv() const { return result_; }
    bool Changed() const { return finished_ && std::fabs(result_ - currentEv_) > 1e-4f; }

  private:
    // The first step, and a step measured again after a jump of more than a step (a late rewind while the sweep was
    // already back at the current value), settle long: after 8 evaluations the model's history still flickered.
    unsigned SettleOf(size_t step) const
    {
        return step == 0 || (step == step_ && longSettle_) ? std::max(settings_.firstSettle, settings_.settle)
                                                           : settings_.settle;
    }

    int BestIndex(Detail d) const { return BestIn(steps_, d); }

    int BestIn(const std::vector<StepResult>& steps, Detail d) const
    {
        int best = -1;
        for (size_t i = 0; i < steps.size(); ++i)
            if (steps[i].samples > 0 && (best < 0 || ScoreIn(steps, i, d) > ScoreIn(steps, (size_t) best, d)))
                best = (int) i;
        return best;
    }

    void Stop(Abort reason)
    {
        if (!running_)
            return;
        running_ = false;
        abort_ = reason;
    }

    // Back to `step`: its measurements and every later step's are dropped, it settles and is measured again, and
    // results already issued come back under the old generation and are ignored.
    void Rewind(size_t step)
    {
        for (size_t i = step; i < steps_.size(); ++i)
        {
            const float ev = steps_[i].ev;
            steps_[i] = StepResult {};
            steps_[i].ev = ev;
        }
        longSettle_ = step < steps_.size() && std::fabs(steps_[step].ev - shownEv_) > settings_.stepEv + 1e-3f;
        step_ = step;
        frameInStep_ = 0;
        outstanding_ = 0;
        ++generation_;
    }

    // A measured evaluation's input moved: measure the step again, or stop once it has been tried often enough.
    void Moved(size_t step, float change)
    {
        if (retries_[step] >= settings_.motionRetries)
        {
            lastInputChange_ = change;
            return Stop(Abort::Motion);
        }

        ++retries_[step];
        notes_.push_back({ Note::Kind::Retry, steps_[step].ev, change, settings_.motionLimit, retries_[step] });
        Rewind(step);
    }

    // The camera came to rest somewhere else: start the sweep over, or stop once it has happened often enough.
    void ViewChanged(size_t step, float band)
    {
        if (restarts_ >= settings_.sceneRestarts)
        {
            lastSceneBand_ = band;
            return Stop(Abort::Motion);
        }

        ++restarts_;
        notes_.push_back({ Note::Kind::Restart, steps_[step].ev, band, sceneBand_, restarts_ });
        sceneBand_ = -1.0f;
        std::fill(retries_.begin(), retries_.end(), 0u);
        // Every pass starts over: the earlier ones measured the other view.
        pass_ = 0;
        firstSteps_.clear();
        first_ = PassVerdict {};
        Rewind(0);
    }

    // Finish and Judge stand the current value in for "keep what you have" through anchorEv_: the current value, or
    // Natural's floor when it sits under it, so a result never keeps a value the floor rules out. A run that gives no
    // result (unsure, unrepeated, aborted) still keeps the current value itself.
    void Finish()
    {
        result_ = currentEv_;

        // Nothing came back with a number in it: not a result, however the steps were spent.
        const int best = BestIndex(settings_.detail);
        if (best < 0)
        {
            Stop(Abort::NothingMeasured);
            return;
        }

        // A measure stands for the scene only with enough of its evaluations back (failed copies or a full readback
        // ring drop samples).
        if (settings_.measureOnly && steps_.front().samples < settings_.measure / 2)
        {
            Stop(Abort::TooFewMeasured);
            return;
        }

        // Another pass to go: keep this one's steps and verdict, and sweep the same steps again from the first. Its
        // measurements come back under a new generation; the view is still checked against the run's first step.
        if (pass_ + 1 < Passes())
        {
            first_ = Judge(steps_);
            firstSteps_ = steps_;
            ++pass_;
            std::fill(retries_.begin(), retries_.end(), 0u);
            Rewind(0);
            return;
        }

        running_ = false;
        finished_ = true;

        // A measure is its numbers, nothing to choose.
        if (settings_.measureOnly)
            return;

        second_ = Judge(steps_);

        if (pass_ == 0)
        {
            first_ = second_;
            unsure_ = second_.unsure;
            result_ = unsure_ ? currentEv_ : second_.result;
            atLimit_ = AtEndOfRange(result_);
            return;

        }

        // Every pass must say the same: unsure if either was; a change only if both land within a step of each
        // other, and then the one nearer the current value (the smaller move).
        if (first_.unsure || second_.unsure)
        {
            unsure_ = true;
            return;
        }

        // Or both moved and their flat tops overlap: on a plateau the best step is whichever end the noise favours (The
        // Witcher 3, 2026-09-30: -2.0, -1.5 and -1.0 EV scored within 0.002, and the passes took -1.0 and -2.0). Then
        // the result is the point of the overlap nearest the current value. A pass that kept the current value (flat
        // everywhere) does not agree with one that found a peak: its top is the whole curve and would overlap anything.
        const float step = settings_.stepEv > 0.01f ? settings_.stepEv : 0.5f;
        const float lo = std::max(first_.topLo, second_.topLo), hi = std::min(first_.topHi, second_.topHi);
        const bool close = std::fabs(first_.result - second_.result) <= step + 1e-3f;
        const bool bothMoved =
            std::fabs(first_.result - anchorEv_) > 1e-4f && std::fabs(second_.result - anchorEv_) > 1e-4f;
        if (!close && (!bothMoved || lo > hi + 1e-3f))
        {
            unrepeated_ = true;
            return;
        }

        if (close)
            result_ = std::fabs(first_.result - anchorEv_) <= std::fabs(second_.result - anchorEv_) ? first_.result
                                                                                                    : second_.result;
        else
            result_ = std::clamp(anchorEv_, lo, hi);
        atLimit_ = AtEndOfRange(result_);
    }

    // Whether `ev` is the first or last step this run actually measured. Read off the result that is offered, so the
    // note cannot appear beside a run that kept the current value, nor be lost when only one pass's best sat there.
    bool AtEndOfRange(float ev) const
    {
        const StepResult* lo = nullptr;
        const StepResult* hi = nullptr;
        for (const StepResult& r : steps_)
        {
            if (r.samples == 0)
                continue;
            if (lo == nullptr)
                lo = &r;
            hi = &r;
        }
        if (lo == nullptr || std::fabs(ev - anchorEv_) <= 1e-4f)
            return false;
        return std::fabs(ev - lo->ev) <= 1e-4f || std::fabs(ev - hi->ev) <= 1e-4f;
    }

    // What one pass's steps say: the best step under the selected measure, unless detail varied no more than the
    // measurement does on its own (unsure), or the curve is flat around the current value.
    PassVerdict Judge(const std::vector<StepResult>& steps) const
    {
        PassVerdict v;
        v.result = anchorEv_;
        v.bestRaw = BestEvIn(steps, Detail::Raw);
        v.bestBand = BestEvIn(steps, Detail::BandPass);

        const int best = BestIn(steps, settings_.detail);
        if (best < 0)
            return v;

        const float topScore = ScoreIn(steps, (size_t) best, settings_.detail);
        const auto onTop = [&](size_t i)
        { return steps[i].samples > 0 && ScoreIn(steps, i, settings_.detail) >= topScore - settings_.flatTolerance; };
        size_t lo = (size_t) best, hi = (size_t) best;
        while (lo > 0 && onTop(lo - 1))
            --lo;
        while (hi + 1 < steps.size() && onTop(hi + 1))
            ++hi;
        v.topLo = steps[lo].ev;
        v.topHi = steps[hi].ev;

        // The step nearest the current value stands for it.
        int nearest = -1;
        for (size_t i = 0; i < steps.size(); ++i)
            if (steps[i].samples > 0 &&
                (nearest < 0 || std::fabs(steps[i].ev - anchorEv_) < std::fabs(steps[(size_t) nearest].ev - anchorEv_)))
                nearest = (int) i;

        float detailLo = std::numeric_limits<float>::infinity(), detailHi = -std::numeric_limits<float>::infinity();
        float inputLo = std::numeric_limits<float>::infinity(), inputHi = -std::numeric_limits<float>::infinity();
        float meanError = 0.0f;
        for (const StepResult& r : steps)
        {
            if (r.samples == 0)
                continue;
            detailLo = std::min(detailLo, r.DetailOf(settings_.detail));
            detailHi = std::max(detailHi, r.DetailOf(settings_.detail));
            inputLo = std::min(inputLo, r.inputBand);
            inputHi = std::max(inputHi, r.inputBand);
            meanError = std::max(meanError, r.MeanError(settings_.detail));
        }
        // Unsure when detail varies no more than the measurement does on its own. Two sources, the larger counts: the
        // input's band detail -- the same frame at every step -- spread across the steps; and the least certain step
        // mean, from the spread of its own samples (NR's output can wobble while the game's frame is bit-identical:
        // Reuse bottleneck alternating, a noisy still). Flicker is not that yardstick -- it is how much the output
        // changes from one evaluation to the next, not how well a step's mean repeats, and it stays in the score. It
        // was, until NBA 2K27 on the HLG and PQ curves: three runs in a row repeated every step to 0.00002 and all put
        // band-pass at +1.5 EV, yet detail varied 0.0005 against 0.00056 of flicker, so every run came back unsure.
        // Movement is the movement checks' job (AddStats).
        const float noise = std::max({ inputHi - inputLo, meanError, settings_.noiseFloor });
        if (detailHi - detailLo <= settings_.unsureNoise * noise)
        {
            v.unsure = true;
            return v;
        }

        // A best on the first or last measured step used to be thrown away, on the grounds that the real best might lie
        // beyond the range. It is taken now (2026-09-30, user): the range is a limit, not a search window, so the best
        // step inside it is the best value there is to offer, and refusing left Tune silent in any scene whose score
        // runs all one way -- a Witcher 3 run scored -3.0 EV at 0.322 against the current -0.5 EV at 0.107, repeated in
        // both passes, and applied nothing. It clears the same bars as any other result: both passes agreeing, and
        // beating the step nearest the current value by more than flatTolerance. The log still notes where it sat.
        int firstMeasured = -1, lastMeasured = -1;
        for (size_t i = 0; i < steps.size(); ++i)
        {
            if (steps[i].samples == 0)
                continue;
            if (firstMeasured < 0)
                firstMeasured = (int) i;
            lastMeasured = (int) i;
        }

        const float bestScore = ScoreIn(steps, (size_t) best, settings_.detail);
        const float nearestScore = ScoreIn(steps, (size_t) nearest, settings_.detail);

        if (bestScore - nearestScore > settings_.flatTolerance)
            v.result = steps[(size_t) best].ev;
        return v;
    }

    Settings settings_ {};
    Context context_ {};
    std::vector<StepResult> steps_;
    float currentEv_ = 0.0f;
    float floorEv_ = -std::numeric_limits<float>::infinity(); // Natural's floor on the slider, -inf without one
    float anchorEv_ = 0.0f;                                   // max(currentEv_, floorEv_): Finish has why
    float result_ = 0.0f;
    uint32_t run_ = 0;
    size_t step_ = 0;
    unsigned frameInStep_ = 0;
    unsigned outstanding_ = 0;
    uint32_t generation_ = 0;
    std::vector<unsigned> retries_;
    unsigned restarts_ = 0;
    float sceneBand_ = -1.0f; // the mean input band of the run's first measured step, -1 before it
    float lastInputChange_ = 0.0f;
    float lastSceneBand_ = -1.0f;
    std::vector<Note> notes_;
    float shownEv_ = 0.0f;     // the EV the last evaluation was shown
    bool longSettle_ = false;  // the step on screen was rewound to from more than a step away
    bool running_ = false;
    bool finished_ = false;
    bool unsure_ = false;
    bool atLimit_ = false;
    bool unrepeated_ = false;
    unsigned pass_ = 0;                  // the pass on now, 0-based
    std::vector<StepResult> firstSteps_; // the first pass's steps, once it is done
    PassVerdict first_ {}, second_ {};
    Blocker stopBlocker_ = Blocker::None;
    unsigned unavailable_ = 0;
    Abort abort_ = Abort::None;
};
} // namespace DlssNrExposureCalibrate
