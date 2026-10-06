#include "pch.h"

// "Tune for this scene", the game's side of the shared run (DlssNr_ExposureCalibrate_Run.h, which is pure so the host
// test can drive it): the clock, where the sliders stand, the log, re-learning Follow the game's exposure, and the
// menu's API. Both backends (DlssNr_ExposureCalibrate_Dx12.inl, dlssnr/DlssNr_ExposureCalibrate_Vk.inl) call the
// *Now functions; the menu calls DlssNr::ExposureCalibration and the buttons.

#include "DlssNr_ExposureCalibrate_Run.h"
#include "DlssNr_FollowGame.h"

#include <dlssnr/DlssNrFeature_Dx12.h>
#include <dlssnr/DlssNr_GameDefaults.h>

#include <Config.h>

namespace DlssNrExposureCalibrate
{
bool WantedNow() { return Wanted(TheRun(), GetTickCount64()); }

void IdleNow() { IdleFrame(TheRun(), GetTickCount64()); }

void BeginFrameNow(Backend& gpu, const ::Config& cfg, unsigned int width, unsigned int height,
                   const Situation& situation, float baseWhitePoint, unsigned long long evaluation)
{
    RunState& run = TheRun();

    // Each slider in its own EV: Automatic around its 5x neutral, Game exposure around 1x.
    // With Trim anchors the Trim in force is the table's at this base white point, not the slider's (which they replace).
    const float autoTrim = DlssNrTrim::TrimForKey(
        situation.source == 3 ? baseWhitePoint : 0.0f, DlssNr::AutoTrimEffective(cfg),
        DlssNrTrim::Parse(cfg.DlssNrAutoExposureTrimAnchors.value_or_default()),
        cfg.DlssNrAutoExposureTrimPreview.value_or_default());
    const float gameTrim = DlssNrTrim::TrimForKey(
        situation.source == 1 ? baseWhitePoint : 0.0f, cfg.DlssNrWhitePointTrim.value_or_default(),
        DlssNrTrim::Parse(cfg.DlssNrGameExposureTrimAnchors.value_or_default()),
        cfg.DlssNrGameExposureTrimPreview.value_or_default());
    const StartPoints start { EvForTrim(autoTrim), EvForTrim(gameTrim, kGameExposureNeutralTrim) };

    const FrameEvents events =
        BeginFrame(run, gpu, start, width, height, situation, baseWhitePoint, evaluation, GetTickCount64());

    if (events.started)
    {
        std::lock_guard<std::mutex> lock(run.mutex);
        if (run.measuring)
            LOG_INFO("DLSS-NR measure: started, {} evaluations at the current settings, measuring at white point {:.4g}",
                     run.sweep.Config().measure, run.measureWhitePoint);
        else
        {
            LOG_INFO("DLSS-NR calibrate: started at {:+.2f} EV, {} steps, measuring at white point {:.4g}",
                     Tidy(run.sweep.CurrentEv()), run.sweep.StepCount(), run.measureWhitePoint);
            LOG_INFO("{}", TargetText(run.sweep));
        }
        if (events.afterSr)
            LOG_INFO("DLSS-NR calibrate: Before SR is set; this run measures after SR and NR goes back before SR when "
                     "it ends");
    }

    // A run tunes against Automatic's own exposure; Follow is learned again meanwhile, so the result holds once it is
    // back in force (RelearnFollowOnStart).
    if (events.relearnFollow)
    {
        DlssNrFollowGame::Instance().Reset();
        LOG_INFO("DLSS-NR calibrate: learning Follow the game's exposure again during the run (it was {:+.2f} EV off "
                 "Automatic)",
                 situation.followDisagreementEv);
    }

    {
        std::lock_guard<std::mutex> lock(run.mutex);
        for (const Note& note : run.sweep.TakeNotes())
            LOG_INFO("{}", NoteText(note, run.sweep.Config().measureOnly));
    }

    if (events.finished)
    {
        std::lock_guard<std::mutex> lock(run.mutex);

        for (const std::string& line : ResultLines(run.sweep))
            LOG_INFO("{}", line);
    }
}

void ShutdownNow(Backend& gpu) { Shutdown(TheRun(), gpu); }
} // namespace DlssNrExposureCalibrate

namespace DlssNr
{
ExposureCalibrationStatus ExposureCalibration()
{
    namespace Cal = DlssNrExposureCalibrate;
    Cal::RunState& run = Cal::TheRun();
    std::lock_guard<std::mutex> lock(run.mutex);
    const Cal::Sweep& sweep = run.sweep;
    const Cal::Blocker blocker = Cal::PollLocked(run, GetTickCount64());

    ExposureCalibrationStatus s {};
    s.available = blocker == Cal::Blocker::None;
    s.unavailable = Cal::BlockerText(blocker);
    s.measure = run.measuring;

    // A Tune and a Measure detail are blocked by the same things.
    s.measureAvailable = s.available;
    s.measureUnavailable = s.unavailable;

    // One run at a time: while either kind runs, the other waits; and a Tune result still offered is not cleared by a
    // measure.
    if (run.startRequested || sweep.Running())
    {
        s.available = s.measureAvailable = false;
        s.unavailable = s.measureUnavailable = run.measuring ? "a measurement is running" : "Tune is running";
    }
    else if (Cal::TuneResultWaiting(run))
    {
        s.measureAvailable = false;
        s.measureUnavailable = "apply or dismiss the Tune result first";
    }

    const auto measurement = [](const Cal::StepResult& r)
    {
        ExposureCalibrationStatus::Measurement m;
        m.detail = r.DetailOf(Cal::Detail::BandPass);
        m.detailOut = r.detailBand;
        m.detailIn = r.inputBand;
        m.raw = r.detailRaw;
        m.flicker = r.Flicker();
        m.flickerOut = r.outputChange;
        m.flickerIn = r.inputChange;
        m.frames = r.samples;
        return m;
    };
    s.measurements = run.measurements;
    s.latest = measurement(run.latest);
    s.previous = measurement(run.previous);
    s.hasPrevious = run.previous.samples > 0;
    s.comparable = s.hasPrevious && Cal::SameScale(run.latestScale, run.previousScale);
    if (run.measurements > 0)
    {
        s.detailWords = Cal::DetailWords(run.latest);
        s.flickerWords = Cal::FlickerWords(run.latest);
        s.colourWords = Cal::ColourWords(run.latest);
        s.shadowWords = Cal::ShadowWords(run.latest);
        if (s.comparable)
            s.compareWords = Cal::CompareWords(run.latest, run.previous);
    }
    s.starting = run.startRequested;
    s.startError = run.startError;
    s.running = sweep.Running();
    s.finished = sweep.Finished();
    s.progress = sweep.Progress();
    s.stepEv = Cal::Tidy(sweep.StepEv());
    s.stepIndex = (unsigned) sweep.StepIndex();
    s.stepCount = (unsigned) sweep.StepCount();
    s.pass = sweep.Pass();
    s.passes = sweep.Passes();
    s.aborted = Cal::StopText(sweep);
    s.currentEv = Cal::Tidy(sweep.CurrentEv());
    s.baseWhitePoint = sweep.FrozenBase();
    s.source = run.source;

    if (s.finished)
    {
        s.changed = sweep.Changed();
        s.unsure = sweep.Unsure();
        s.unrepeated = sweep.Unrepeated();
        s.firstPassEv = Cal::Tidy(sweep.FirstPass().result);
        s.lastPassEv = Cal::Tidy(sweep.LastPass().result);
        s.resultEv = Cal::Tidy(sweep.ResultEv());
        s.bestRawEv = Cal::Tidy(sweep.BestEv(Cal::Detail::Raw));
        s.rawAgreed = sweep.RawAgreed();
        s.bestBandEv = Cal::Tidy(sweep.BestEv(Cal::Detail::BandPass));
        s.naturalWanted = sweep.NaturalWanted();
        s.hasFloor = sweep.HasFloor();
        s.floorEv = sweep.HasFloor() ? Cal::Tidy(sweep.FloorEv()) : 0.0f;
    }

    const auto& steps = sweep.Steps();
    for (size_t i = 0; i < steps.size(); ++i)
    {
        if (steps[i].samples == 0)
            continue;
        // The step the result stands on (the current value's nearest when it keeps it), for Details.
        if (s.finished && (s.ev.empty() || std::fabs(steps[i].ev - sweep.ResultEv()) <
                                               std::fabs(s.resultStepEv - sweep.ResultEv())))
        {
            s.resultStepEv = steps[i].ev;
            s.resultSaturation = steps[i].Saturation();
            s.resultWarmth = steps[i].warmth;
            s.resultShadowDarkening = steps[i].ShadowDarkening();
            s.resultCrushed = steps[i].crushed;
        }
        s.ev.push_back(steps[i].ev);
        s.scoreRaw.push_back(sweep.Score(i, Cal::Detail::Raw));
        s.scoreBand.push_back(sweep.Score(i, Cal::Detail::BandPass));
    }

    return s;
}

void StartExposureCalibration(uint32_t source) { DlssNrExposureCalibrate::RequestStart(DlssNrExposureCalibrate::TheRun(), source); }

void StartMeasureDetail() { DlssNrExposureCalibrate::RequestMeasure(DlssNrExposureCalibrate::TheRun()); }

void CancelExposureCalibration() { DlssNrExposureCalibrate::RequestCancel(DlssNrExposureCalibrate::TheRun()); }

void DismissExposureCalibration() { DlssNrExposureCalibrate::Dismiss(DlssNrExposureCalibrate::TheRun()); }
} // namespace DlssNr
