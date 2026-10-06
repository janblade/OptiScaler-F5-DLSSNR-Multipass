// Host check of DlssNr_ExposureCalibrate.h: the "Tune for this scene" sweep over the model input brightness. No GPU
// and no game needed.
// cl /std:c++20 /EHsc /W4 tests/nr_exposure_calibrate_smoke.cpp
#include "../OptiScaler/shaders/dlssnr/DlssNr_ExposureCalibrate.h"
#include "../OptiScaler/shaders/dlssnr/DlssNr_ExposureCalibrate_Run.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <utility>
#include <string>
#include <vector>

using namespace DlssNrExposureCalibrate;

static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

static bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

static const Context kCtx { 2560, 1440, 3, true };

// One pass over the steps: what most checks here are about (the sweep, its scoring, its stops). A shipped run makes
// Settings::passes of them and must agree with itself; that is checked on its own below.
static Settings OnePass()
{
    Settings s;
    s.passes = 1;
    return s;
}

// A still scene whose detail peaks at `peakEv`: detail falls off with the distance from the peak, no flicker, no damage.
static Stats Peaked(float ev, float peakEv)
{
    Stats s {};
    const float d = 1.0f / (1.0f + (ev - peakEv) * (ev - peakEv));
    s.detailRaw = d;
    s.detailBand = d;
    s.inputBand = 0.1f;
    return s;
}

// A still scene whose added detail rises at every darker step, as Tune's score does when nothing clips (offline tests
// on a Witcher 3 frame): the best is always the darkest step swept.
static Stats DarkFavoured(float ev)
{
    Stats s {};
    s.detailRaw = s.detailBand = 0.2f + 0.02f * (4.0f - ev);
    s.inputBand = 0.1f;
    return s;
}

// Drives a whole run: every evaluation asks the sweep what to do, and a measured one's stats come back `lag` evaluations
// later, as they would from the readback ring. Returns the number of evaluations it took.
static int Run(Sweep& sweep, const std::function<Stats(float ev)>& scene, int lag = 4, const Context& ctx = kCtx,
               int limit = 10000)
{
    struct Pending
    {
        int due;
        Ticket ticket;
        Stats stats;
    };
    std::vector<Pending> pending;
    int n = 0;

    for (; n < limit && sweep.Running(); ++n)
    {
        const Frame f = sweep.NextFrame(ctx);

        if (!sweep.Running())
            break;

        if (f.measure)
            pending.push_back({ n + lag, f.ticket, scene(f.ev) });

        for (size_t i = 0; i < pending.size();)
        {
            if (pending[i].due <= n)
            {
                sweep.AddStats(pending[i].ticket, pending[i].stats);
                pending.erase(pending.begin() + (std::ptrdiff_t) i);
            }
            else
            {
                ++i;
            }
        }
    }

    return n;
}

int main()
{
    // The steps: -3 .. +4 EV in 0.5 EV steps, 8 settle + 4 measured evaluations each, in ascending order.
    {
        Sweep s;
        s.Start(1.5f, OnePass(), kCtx);
        CHECK(s.Running());
        CHECK(s.StepCount() == 15);

        std::vector<float> evs;
        std::vector<int> measured;
        int n = 0;

        const int total = 15 * 12 + (int) (OnePass().firstSettle - OnePass().settle);
        while (n < total)
        {
            const Frame f = s.NextFrame(kCtx);
            if (evs.empty() || !Near(evs.back(), f.ev))
            {
                evs.push_back(f.ev);
                measured.push_back(0);
            }
            measured.back() += f.measure ? 1 : 0;
            if (f.measure)
                s.AddStats(f.ticket, Peaked(f.ev, 2.0f));
            ++n;
        }

        CHECK(evs.size() == 15);
        CHECK(Near(evs.front(), -3.0f) && Near(evs.back(), 4.0f));
        for (size_t i = 1; i < evs.size(); ++i)
            CHECK(Near(evs[i] - evs[i - 1], 0.5f));
        for (int m : measured)
            CHECK(m == 4);
        // The measured evaluations of a later step are its last four, after eight to settle.
        Sweep t;
        t.Start(0.0f, OnePass(), kCtx);
        for (unsigned i = 0; i < OnePass().firstSettle + OnePass().measure; ++i)
            t.NextFrame(kCtx);
        for (int i = 0; i < 8; ++i)
            CHECK(!t.NextFrame(kCtx).measure);
        CHECK(t.NextFrame(kCtx).measure);
    }

    // The measured count is even, so Reuse bottleneck's computed/reused alternation is covered equally.
    CHECK(OnePass().measure % 2 == 0);

    // Picks the highest score, with results arriving four evaluations late.
    {
        Sweep s;
        s.Start(1.5f, OnePass(), kCtx);
        Run(s, [](float ev) { return Peaked(ev, 2.5f); });
        CHECK(s.Finished());
        CHECK(Near(s.ResultEv(), 2.5f));
        CHECK(s.Steps().size() == 15);
        for (const StepResult& r : s.Steps())
            CHECK(r.samples == 4);
    }

    // Still finishes with no lag at all, and with a long one.
    for (int lag : { 0, 1, 7 })
    {
        Sweep s;
        s.Start(0.0f, OnePass(), kCtx);
        Run(s, [](float ev) { return Peaked(ev, -1.0f); }, lag);
        CHECK(s.Finished());
        CHECK(Near(s.ResultEv(), -1.0f));
    }

    // A step with more detail that flickers loses to a stable one.
    {
        Sweep s;
        s.Start(0.0f, OnePass(), kCtx);
        Run(s, [](float ev) {
            Stats st = Peaked(ev, 1.0f);
            if (Near(ev, 3.0f))
            {
                st.detailRaw = st.detailBand = 1.2f;
                st.outputChange = 0.6f; // the model's own change, the input did not move
            }
            return st;
        });
        CHECK(s.Finished());
        CHECK(Near(s.ResultEv(), 1.0f));
    }

    // Output change the input explains (animation in the scene) is not flicker. (Below the motion check's reach here.)
    {
        Settings loose = OnePass();
        loose.motionLimit = 1.0f;
        Sweep s;
        s.Start(0.0f, loose, kCtx);
        Run(s, [](float ev) {
            Stats st = Peaked(ev, 3.0f);
            st.detailRaw *= 0.05f;
            st.detailBand *= 0.05f;
            st.inputBand = 0.0f;
            // at the peak only, and as large as the detail: counted as flicker it would lose the peak
            if (Near(ev, 3.0f))
                st.inputChange = st.outputChange = 0.04f;
            return st;
        });
        CHECK(s.Finished());
        CHECK(Near(s.ResultEv(), 3.0f));
    }

    // Damage (highlights on the shoulder, shadows in the floor) pulls the choice away from the peak.
    {
        Sweep s;
        s.Start(0.0f, OnePass(), kCtx);
        Run(s, [](float ev) {
            Stats st = Peaked(ev, 4.0f);
            st.shoulder = ev > 2.0f ? 0.5f : 0.0f;
            return st;
        });
        CHECK(s.Finished());
        CHECK(s.ResultEv() <= 2.0f + 1e-4f);
    }

    // The detail measure is selectable; both are kept per step for the log.
    {
        auto scene = [](float ev) {
            Stats st {};
            st.detailRaw = 1.0f / (1.0f + (ev + 2.0f) * (ev + 2.0f));  // raw peaks at -2 (grain)
            st.detailBand = 1.0f / (1.0f + (ev - 2.0f) * (ev - 2.0f)); // band-pass peaks at +2
            return st;
        };
        Settings raw = OnePass();
        raw.detail = Detail::Raw;
        Settings band = OnePass();
        band.detail = Detail::BandPass;
        Sweep a, b;
        a.Start(0.0f, raw, kCtx);
        b.Start(0.0f, band, kCtx);
        Run(a, scene);
        Run(b, scene);
        CHECK(Near(a.ResultEv(), -2.0f));
        CHECK(Near(b.ResultEv(), 2.0f));
        CHECK(Near(a.BestEv(Detail::BandPass), 2.0f));
        CHECK(Near(b.BestEv(Detail::Raw), -2.0f));
        const StepResult& r = b.Steps()[0];
        CHECK(Near(r.detailRaw, scene(-3.0f).detailRaw) && Near(r.detailBand, scene(-3.0f).detailBand));
    }

    // Band-pass detail is the output's band energy minus the input's.
    {
        Stats st {};
        st.detailBand = 0.5f;
        st.inputBand = 0.2f;
        CHECK(Near(AddedBand(st), 0.3f));
    }

    // Movement is measured again before it stops a run.
    {
        // A still scene: the result every retry case below must reproduce.
        Sweep still;
        still.Start(0.0f, OnePass(), kCtx);
        Run(still, [](float ev) { return Peaked(ev, 1.5f); });
        CHECK(still.Finished() && Near(still.ResultEv(), 1.5f));

        // One moved measurement at +1.0 EV (a player walking through): the step is measured again, the result is the
        // still one, every step has its four samples, and the log gets one note -- whatever the readback lag.
        for (int lag : { 0, 4, 7 })
        {
            Sweep s;
            s.Start(0.0f, OnePass(), kCtx);
            int moved = 0;
            Run(s, [&moved](float ev) {
                Stats st = Peaked(ev, 1.5f);
                if (Near(ev, 1.0f) && moved++ == 1)
                    st.inputChange = 0.004f;
                return st;
            }, lag);
            CHECK(s.Finished());
            CHECK(Near(s.ResultEv(), still.ResultEv()));
            for (const StepResult& r : s.Steps())
                CHECK(r.samples == 4);
            const std::vector<Note> notes = s.TakeNotes();
            CHECK(notes.size() == 1 && notes[0].kind == Note::Kind::Retry && Near(notes[0].ev, 1.0f) &&
                  Near(notes[0].value, 0.004f) && notes[0].count == 1);
            CHECK(s.TakeNotes().empty());
        }

        // A step that keeps moving stops the run after motionRetries more tries, and the stop says what it measured.
        {
            Sweep s;
            s.Start(0.0f, OnePass(), kCtx);
            Run(s, [](float ev) {
                Stats st = Peaked(ev, 1.5f);
                if (Near(ev, 1.0f))
                    st.inputChange = 0.003f;
                return st;
            });
            CHECK(!s.Finished() && s.AbortReason() == Abort::Motion);
            CHECK(Near(s.LastInputChange(), 0.003f) && s.LastSceneBand() < 0.0f);
            CHECK(s.TakeNotes().size() == OnePass().motionRetries);
            const std::string text = StopText(s);
            CHECK(text.find("0.00300") != std::string::npos && text.find("0.00100") != std::string::npos);
        }

        // The camera moved and came to rest somewhere else (the input's band detail changed): the sweep starts over on the
        // new view and finds that view's best.
        {
            Sweep s;
            s.Start(0.0f, OnePass(), kCtx);
            int evaluations = 0;
            Run(s, [&evaluations](float ev) {
                const bool after = evaluations++ >= 20; // the camera settles elsewhere during the sweep's sixth step
                Stats st = Peaked(ev, after ? -1.0f : 1.5f);
                st.inputBand = after ? 0.2f : 0.1f;
                if (evaluations == 21)
                    st.inputChange = 0.01f; // the move itself
                return st;
            });
            CHECK(s.Finished());
            CHECK(Near(s.ResultEv(), -1.0f));
            const std::vector<Note> notes = s.TakeNotes();
            bool restarted = false;
            for (const Note& n : notes)
                restarted = restarted || (n.kind == Note::Kind::Restart && Near(n.value, 0.2f) && Near(n.against, 0.1f) &&
                                          n.ev >= -0.5f - 1e-4f && n.ev <= 0.0f + 1e-4f); // the step the band changed at
            CHECK(restarted);
        }

        // A view that keeps changing stops the run after sceneRestarts restarts.
        {
            Sweep s;
            s.Start(0.0f, OnePass(), kCtx);
            int evaluations = 0;
            Run(s, [&evaluations](float ev) {
                Stats st = Peaked(ev, 1.5f);
                st.inputBand = 0.1f * (1.0f + (float) (evaluations++ / 10)); // a slow pan: a new view every 10 readings
                return st;
            });
            CHECK(!s.Finished() && s.AbortReason() == Abort::Motion);
            CHECK(s.LastSceneBand() > 0.0f);
            CHECK(StopText(s).find("view changed") != std::string::npos);
        }
    }

    // Camera motion aborts.
    {
        Sweep s;
        s.Start(0.0f, OnePass(), kCtx);
        Run(s, [](float ev) {
            Stats st = Peaked(ev, 1.0f);
            if (ev > 0.0f)
                st.inputChange = 0.2f;
            return st;
        });
        CHECK(!s.Running() && !s.Finished());
        CHECK(s.AbortReason() == Abort::Motion);
    }

    // A resolution, NR on/off or white point source change aborts.
    {
        Context c = kCtx;
        c.width = 1920;
        Sweep s;
        s.Start(0.0f, OnePass(), kCtx);
        s.NextFrame(kCtx);
        s.NextFrame(c);
        CHECK(s.AbortReason() == Abort::Resolution);

        c = kCtx;
        c.nrEnabled = false;
        s.Start(0.0f, OnePass(), kCtx);
        s.NextFrame(c);
        CHECK(s.AbortReason() == Abort::NrOff);

        c = kCtx;
        c.whitePointSource = 1;
        s.Start(0.0f, OnePass(), kCtx);
        s.NextFrame(c);
        CHECK(s.AbortReason() == Abort::SourceChanged);

        s.Start(0.0f, OnePass(), kCtx);
        s.Cancel();
        CHECK(!s.Running() && s.AbortReason() == Abort::Cancelled);
    }

    // The base white point (the exposure) is pinned for the run: a move of more than 0.25 EV aborts, a smaller one does
    // not, and an unknown base (0) is never compared.
    {
        Context c = kCtx;
        c.baseWhitePoint = 2.0f;
        Sweep s;
        s.Start(0.0f, OnePass(), c);
        CHECK(Near(s.FrozenBase(), 2.0f));
        Context drift = c;
        drift.baseWhitePoint = 2.0f * std::exp2(0.2f);
        s.NextFrame(drift);
        CHECK(s.Running());
        drift.baseWhitePoint = 2.0f * std::exp2(-0.3f);
        s.NextFrame(drift);
        CHECK(!s.Running() && s.AbortReason() == Abort::ExposureMoved);

        Sweep u;
        Context unknown = kCtx;
        unknown.baseWhitePoint = 0.0f;
        u.Start(0.0f, OnePass(), c);
        u.NextFrame(unknown);
        CHECK(u.Running());
    }

    // Detail is measured on one scale whatever the slider was at: Automatic's metering at 0 EV. Measuring "as the
    // picture looked at the start" moved the scale with the slider (15.5 from -3.3 EV, 0.080 from +4.3 EV in NBA 2K27)
    // and the result followed the starting value.
    {
        Context c = kCtx;
        c.baseWhitePoint = 0.39f;
        Sweep left, right;
        left.Start(-3.3f, OnePass(), c);
        right.Start(4.3f, OnePass(), c);
        CHECK(Near(left.MeasureWhitePoint(), right.MeasureWhitePoint()));
        CHECK(Near(left.MeasureWhitePoint(), 0.39f * TrimForEv(0.0f)));
    }

    // Game exposure has its own slider: 0 EV is Trim 1 (the game's exposure as is), and the sweep runs -5.5 .. +2 EV,
    // inside that slider's range, measured at the game's own exposure.
    {
        Settings g = GameExposureSettings();
        CHECK(g.passes == 2);
        g.passes = 1;
        CHECK(Near(g.neutralTrim, 1.0f));
        CHECK(Near(TrimForEv(1.0f, 1.0f), 0.5f) && Near(EvForTrim(0.5f, 1.0f), 1.0f));
        Context c = kCtx;
        c.whitePointSource = 1;
        c.baseWhitePoint = 0.7579f;
        Sweep s;
        s.Start(0.0f, g, c);
        CHECK(s.StepCount() == 16);
        const Frame f = s.NextFrame(c);
        CHECK(Near(f.ev, -5.5f));
        CHECK(Near(s.WhitePointFor(-5.5f), 0.7579f * TrimForEv(-5.5f, 1.0f)));
        CHECK(Near(s.MeasureWhitePoint(), 0.7579f));
        CHECK(Near(s.Steps().back().ev, 2.0f));
        // Automatic's defaults are unchanged.
        CHECK(Near(OnePass().neutralTrim, 5.0f) && Near(TrimForEv(0.0f), 5.0f));
    }

    // The white point of a step: the frozen base times that step's Trim, within the pass's clamp.
    {
        Context c = kCtx;
        c.baseWhitePoint = 2.0f;
        Sweep s;
        s.Start(0.0f, OnePass(), c);
        const Frame f = s.NextFrame(c);
        CHECK(Near(f.ev, -3.0f));
        CHECK(Near(s.WhitePointFor(f.ev), 2.0f * TrimForEv(-3.0f)));
        Context huge = kCtx;
        huge.baseWhitePoint = 1e6f;
        Sweep h;
        h.Start(0.0f, OnePass(), huge);
        CHECK(Near(h.WhitePointFor(-3.0f), 4096.0f));
    }

    // Stats that arrive after an abort, or with a ticket from an earlier run, are ignored.
    {
        Sweep s;
        s.Start(0.0f, OnePass(), kCtx);
        Frame f {};
        for (unsigned i = 0; i <= OnePass().firstSettle; ++i)
            f = s.NextFrame(kCtx);
        CHECK(f.measure);
        s.Start(0.0f, OnePass(), kCtx);
        s.AddStats(f.ticket, Peaked(-3.0f, -3.0f));
        CHECK(s.Steps()[0].samples == 0);
    }

    // A flat curve keeps the current value: the best is within the tolerance of the step nearest to it.
    {
        Sweep s;
        s.Start(2.3f, OnePass(), kCtx);
        Run(s, [](float ev) {
            Stats st {};
            st.detailRaw = st.detailBand = 1.0f + (Near(ev, 0.0f) ? 0.01f : 0.0f);
            return st;
        });
        CHECK(s.Finished());
        CHECK(Near(s.ResultEv(), 2.3f));
        CHECK(!s.Changed());
    }

    // A clear winner is a change.
    {
        Sweep s;
        s.Start(2.3f, OnePass(), kCtx);
        Run(s, [](float ev) { return Peaked(ev, -1.0f); });
        CHECK(s.Changed());
    }

    // The result stays within the slider's range, and EV -> Trim is the menu's conversion.
    {
        CHECK(Near(TrimForEv(0.0f), 5.0f));
        CHECK(Near(TrimForEv(1.0f), 2.5f));
        CHECK(Near(TrimForEv(-1.0f), 10.0f));
        CHECK(Near(TrimForEv(10.0f), DlssNrTrim::kMinTrim));
        CHECK(Near(TrimForEv(-10.0f), DlssNrTrim::kMaxTrim));
        CHECK(Near(EvForTrim(TrimForEv(1.5f)), 1.5f));

        Settings wide = OnePass();
        wide.minEv = -8.0f;
        wide.maxEv = 8.0f;
        Sweep s;
        s.Start(0.0f, wide, kCtx);
        CHECK(s.StepCount() > 0);
        CHECK(s.NextFrame(kCtx).ev >= EvForTrim(DlssNrTrim::kMaxTrim) - 1e-4f);
        Run(s, [](float ev) { return Peaked(ev, 8.0f); });
        CHECK(s.ResultEv() <= EvForTrim(DlssNrTrim::kMinTrim) + 1e-4f);
    }

    // Real runs from NBA 2K27 (OptiScaler.log, 2026-09-27), per step: raw, band out, band in, output change, input
    // change, shoulder, floor. Run 1 was a still scene, run 2 a moving one.
    struct Row
    {
        float ev, raw, bandOut, bandIn, outChange, inChange, shoulder, floor;
    };
    static const Row kStill[] = {
        { -3.0f, .03784f, .01984f, .01873f, .00344f, .00024f, 0, .3162f },
        { -2.5f, .03908f, .01986f, .01873f, .00214f, .00022f, 0, .2598f },
        { -2.0f, .04076f, .02047f, .01873f, .00201f, .00024f, 0, .2051f },
        { -1.5f, .04238f, .02103f, .01873f, .00183f, .00022f, 0, .1488f },
        { -1.0f, .04353f, .02139f, .01873f, .00165f, .00024f, 0, .1051f },
        { -0.5f, .04441f, .02161f, .01873f, .00157f, .00022f, 0, .0782f },
        { 0.0f, .04500f, .02169f, .01873f, .00168f, .00024f, 0, .0498f },
        { 0.5f, .04505f, .02161f, .01873f, .00227f, .00022f, 0, .0228f },
        { 1.0f, .04456f, .02125f, .01873f, .00340f, .00024f, .0003f, .0087f },
        { 1.5f, .04313f, .02067f, .01873f, .00334f, .00022f, .0081f, .0032f },
        { 2.0f, .04169f, .02010f, .01873f, .00251f, .00024f, .0623f, .0011f },
        { 2.5f, .04007f, .01953f, .01873f, .00184f, .00022f, .2243f, .0004f },
        { 3.0f, .03845f, .01896f, .01873f, .00114f, .00024f, .2642f, .0001f },
        { 3.5f, .03674f, .01828f, .01873f, .00089f, .00022f, .2919f, 0 },
        { 4.0f, .03500f, .01755f, .01873f, .00077f, .00024f, .3197f, 0 },
    };
    static const Row kMoving[] = {
        { -3.0f, .00894f, .00539f, .00323f, .01803f, .00791f, 0, .0220f },
        { -2.5f, .00904f, .00527f, .00315f, .01221f, .00514f, 0, .0003f },
        { -2.0f, .00901f, .00484f, .00292f, .01635f, .00664f, 0, .0009f },
        { -1.5f, .00912f, .00451f, .00276f, .01707f, .00691f, 0, .0005f },
        { -1.0f, .00842f, .00442f, .00286f, .01573f, .00527f, 0, .0005f },
        { -0.5f, .00869f, .00419f, .00289f, .01781f, .00578f, 0, 0 },
        { 0.0f, .00860f, .00402f, .00310f, .01897f, .00607f, 0, 0 },
        { 0.5f, .00837f, .00365f, .00315f, .02065f, .01027f, .0003f, 0 },
        { 1.0f, .00772f, .00331f, .00309f, .02124f, .01174f, .0009f, 0 },
        { 1.5f, .00705f, .00297f, .00279f, .02107f, .00984f, .0022f, 0 },
        { 2.0f, .00696f, .00297f, .00304f, .02066f, .00895f, .0063f, 0 },
        { 2.5f, .00629f, .00270f, .00295f, .01838f, .00771f, .0100f, 0 },
        { 3.0f, .00593f, .00248f, .00291f, .01587f, .00715f, .0115f, 0 },
        { 3.5f, .00539f, .00242f, .00297f, .01628f, .01092f, .0161f, 0 },
        { 4.0f, .00480f, .00221f, .00281f, .01266f, .01020f, .0675f, 0 },
    };
    auto replay = [](const Row* rows) {
        return [rows](float ev) {
            for (int i = 0; i < 15; ++i)
            {
                if (Near(rows[i].ev, ev))
                {
                    const Row& r = rows[i];
                    Stats st {};
                    st.detailRaw = r.raw;
                    st.detailBand = r.bandOut;
                    st.inputBand = r.bandIn;
                    st.outputChange = r.outChange;
                    st.inputChange = r.inChange;
                    st.shoulder = r.shoulder;
                    st.floor = r.floor;
                    return st;
                }
            }
            Stats none {};
            none.detailBand = NAN;
            return none;
        };
    };

    // A paused NBA 2K27 scene (run 3 of 2026-09-27). Its first step flickered 5x the rest -- the model's history after
    // a 4.5 EV jump -- which made the old spread-based check call a still scene unsure.
    static const Row kPaused[] = {
        { -3.0f, .03961f, .02065f, .01852f, .00657f, .00020f, 0, .2183f },
        { -2.5f, .04056f, .02116f, .01852f, .00175f, .00021f, 0, .1808f },
        { -2.0f, .04176f, .02157f, .01852f, .00146f, .00020f, 0, .1348f },
        { -1.5f, .04273f, .02182f, .01852f, .00140f, .00021f, 0, .0945f },
        { -1.0f, .04322f, .02183f, .01852f, .00143f, .00020f, 0, .0534f },
        { -0.5f, .04333f, .02172f, .01852f, .00150f, .00021f, 0, .0275f },
        { 0.0f, .04254f, .02141f, .01852f, .00117f, .00020f, .0001f, .0150f },
        { 0.5f, .04154f, .02094f, .01852f, .00110f, .00021f, .0321f, .0083f },
        { 1.0f, .04032f, .02030f, .01852f, .00099f, .00020f, .2418f, .0042f },
        { 1.5f, .03885f, .01964f, .01852f, .00095f, .00021f, .3590f, .0015f },
        { 2.0f, .03720f, .01906f, .01852f, .00089f, .00020f, .3924f, .0003f },
        { 2.5f, .03548f, .01852f, .01852f, .00085f, .00021f, .4222f, .0001f },
        { 3.0f, .03390f, .01800f, .01852f, .00082f, .00020f, .4406f, 0 },
        { 3.5f, .03225f, .01746f, .01852f, .00079f, .00021f, .4592f, 0 },
        { 4.0f, .03057f, .01698f, .01852f, .00071f, .00020f, .4891f, 0 },
    };

    // The paused run is sure: band-pass lands in its flat top (-1.0 .. 0.0 EV), raw at 0.0 EV.
    {
        Sweep s;
        s.Start(1.5f, OnePass(), kCtx);
        Run(s, replay(kPaused));
        CHECK(s.Finished());
        CHECK(!s.Unsure());
        CHECK(s.ResultEv() >= -1.0f - 1e-4f && s.ResultEv() <= 0.0f + 1e-4f);
        CHECK(Near(s.BestEv(Detail::Raw), 0.0f));
        CHECK(s.Changed());
    }

    // A run whose best is the first or last step (NBA 2K27 with Follow-game's stale -3.42 EV calibration, every step
    // 4.3 EV brighter than its label): its best sits at an end of the range, which is offered like any other.
    static const Row kEdge[] = {
        { -3.0f, .04136f, .01895f, .01637f, .00176f, .00022f, 0, .0324f },
        { -2.5f, .04180f, .01879f, .01637f, .01143f, .00023f, 0, .0226f },
        { -2.0f, .04198f, .01857f, .01637f, .01253f, .00022f, 0, .0136f },
        { -1.5f, .04134f, .01823f, .01637f, .01039f, .00023f, 0, .0085f },
        { -1.0f, .04002f, .01778f, .01637f, .00613f, .00022f, 0, .0056f },
        { -0.5f, .03757f, .01724f, .01637f, .00187f, .00023f, .0040f, .0010f },
        { 0.0f, .03583f, .01660f, .01637f, .00093f, .00022f, .4745f, .0002f },
        { 0.5f, .03396f, .01589f, .01637f, .00088f, .00023f, .6421f, .0001f },
        { 1.0f, .03205f, .01538f, .01637f, .00082f, .00022f, .6859f, 0 },
        { 1.5f, .03036f, .01504f, .01637f, .00080f, .00023f, .7077f, 0 },
        { 2.0f, .02900f, .01483f, .01637f, .00072f, .00022f, .7267f, 0 },
        { 2.5f, .02767f, .01469f, .01637f, .00068f, .00023f, .7487f, 0 },
        { 3.0f, .02664f, .01459f, .01637f, .00064f, .00022f, .7750f, 0 },
        { 3.5f, .02586f, .01453f, .01637f, .00060f, .00023f, .8021f, 0 },
        { 4.0f, .02528f, .01450f, .01637f, .00056f, .00022f, .8282f, 0 },
    };
    {
        Sweep s;
        s.Start(-1.2f, OnePass(), kCtx);
        Run(s, replay(kEdge));
        CHECK(s.Finished());
        // Its best sits at an end of the range; it is offered now rather than discarded.
        CHECK(s.Changed() && Near(s.ResultEv(), s.BestEv(Detail::BandPass)));
    }

    // A peak the sweep brackets, and one that sits on the top step: both are offered.
    {
        Sweep s;
        s.Start(1.5f, OnePass(), kCtx);
        Run(s, replay(kStill));
        CHECK(s.Finished());
        Sweep p;
        p.Start(2.0f, OnePass(), kCtx);
        Run(p, [](float ev) { return Peaked(ev, 4.0f); });
        CHECK(p.Finished() && p.Changed() && Near(p.ResultEv(), 4.0f));
    }

    // The best step at the end of the range is offered, not thrown away: the range is a limit, not a search window, so
    // the best step inside it is the best value there is (2026-09-30). It still has to clear what any other result does.
    {
        // A score that climbs all the way to the darkest step: the best is the bottom of the range.
        const auto darkest = [](float ev)
        {
            Stats st {};
            st.detailRaw = st.detailBand = 1.0f - 0.1f * (ev + 3.0f); // 1.0 at -3 EV, falling with brightness
            st.inputBand = 0.1f;
            return st;
        };
        Sweep s;
        s.Start(0.0f, OnePass(), kCtx);
        Run(s, darkest);
        CHECK(s.Finished() && Near(s.BestEv(Detail::BandPass), -3.0f));
        CHECK(s.Changed() && Near(s.ResultEv(), -3.0f));

        // The same shape, but so shallow that the bottom step beats the current one by less than flatTolerance: the
        // current value is kept, as anywhere else on a flat curve.
        const auto shallow = [](float ev)
        {
            Stats st {};
            st.detailRaw = st.detailBand = 1.0f - 0.0005f * (ev + 3.0f);
            st.inputBand = 0.1f;
            return st;
        };
        Sweep f;
        f.Start(0.0f, OnePass(), kCtx);
        Run(f, shallow);
        CHECK(f.Finished() && !f.Changed() && Near(f.ResultEv(), 0.0f));

        // Two passes that disagree still keep the current value, edge or not: the first climbs to the bottom step, the
        // second to the top one.
        Sweep d;
        d.Start(0.0f, Settings(), kCtx); // two passes
        Run(d, [&](float ev)
            {
                Stats st {};
                const float slope = d.Pass() == 0 ? -0.1f : 0.1f; // pass 1 climbs to the bottom step, pass 2 to the top
                st.detailRaw = st.detailBand = 1.0f + slope * (ev + 3.0f);
                st.inputBand = 0.1f;
                return st;
            });
        CHECK(d.Finished() && !d.Changed() && Near(d.ResultEv(), 0.0f));
    }

    // Follow-game's base against Automatic's own, in EV; unknown (0) never disagrees.
    CHECK(Near(BaseDisagreementEv(0.0708f, 1.361f), std::log2(0.0708f / 1.361f)));
    CHECK(std::fabs(BaseDisagreementEv(0.0708f, 1.361f)) > kFollowDisagreementLimitEv);
    CHECK(std::fabs(BaseDisagreementEv(1.2f, 1.361f)) < kFollowDisagreementLimitEv);
    CHECK(BaseDisagreementEv(0.0f, 1.361f) == 0.0f && BaseDisagreementEv(1.0f, 0.0f) == 0.0f);

    // The first step settles longer: it is the one big jump, from the current value to the bottom of the sweep.
    {
        Sweep s;
        s.Start(1.5f, OnePass(), kCtx);
        for (unsigned i = 0; i < OnePass().firstSettle; ++i)
            CHECK(!s.NextFrame(kCtx).measure);
        CHECK(s.NextFrame(kCtx).measure);
    }

    // The still run: band-pass +0.0 EV, raw +0.5 EV, confidently, from the +1.5 EV default.
    {
        Sweep s;
        s.Start(1.5f, OnePass(), kCtx);
        Run(s, replay(kStill));
        CHECK(s.Finished());
        CHECK(!s.Unsure());
        CHECK(Near(s.ResultEv(), 0.0f));
        CHECK(Near(s.BestEv(Detail::Raw), 0.5f));
        CHECK(s.Changed());
    }

    // The moving run aborts on motion with the default limit...
    {
        Sweep s;
        s.Start(0.0f, OnePass(), kCtx);
        Run(s, replay(kMoving));
        CHECK(!s.Finished() && s.AbortReason() == Abort::Motion);
    }

    // ...and with the movement checks lifted it offers its best, which sits at an end of the range: its best band sits at the top step,
    // so the real best may lie beyond. (Before, the unsure rule caught it; that rule no longer reads movement --
    // the movement checks are what stop a moving run.)
    {
        Settings loose = OnePass();
        loose.motionLimit = 1.0f;
        loose.sceneTolerance = 1e9f;
        Sweep s;
        s.Start(0.0f, loose, kCtx);
        Run(s, replay(kMoving));
        CHECK(s.Finished() && s.Changed() && Near(s.ResultEv(), s.BestEv(Detail::BandPass)));
    }

    // Cyberpunk 2077 with ray reconstruction, a still camera (OptiScaler.log, 2026-09-28): the output flickers about 10x
    // more at the bright end than at the dark end, so the median flicker hid the detail and every run came back unsure
    // (issue #71). Run 1 is sure: detail and flicker both point darker.
    static const Row kCyberpunk[] = {
        { -3.0f, .03249f, .01422f, .01044f, .0021f, .00035f, 0, .0538f },
        { -2.5f, .03245f, .01422f, .01045f, .00218f, .0003f, .0001f, .0258f },
        { -2.0f, .03209f, .01411f, .01045f, .00197f, .00026f, .0001f, .0114f },
        { -1.5f, .03155f, .0139f, .01044f, .00182f, .00026f, .0002f, .0055f },
        { -1.0f, .03072f, .01359f, .01043f, .00235f, .0003f, .0006f, .0027f },
        { -0.5f, .02941f, .01313f, .01043f, .00301f, .00079f, .0014f, .0009f },
        { 0.0f, .02784f, .01246f, .01034f, .00399f, .00067f, .0021f, .0001f },
        { 0.5f, .02659f, .01189f, .0104f, .00602f, .00071f, .0029f, 0 },
        { 1.0f, .02551f, .01142f, .01052f, .009f, .00074f, .0045f, 0 },
        { 1.5f, .02463f, .01102f, .01061f, .01198f, .00075f, .0064f, 0 },
        { 2.0f, .024f, .01076f, .01064f, .01458f, .00069f, .0108f, 0 },
        { 2.5f, .02382f, .01073f, .01065f, .01494f, .00068f, .0168f, 0 },
        { 3.0f, .02395f, .01082f, .01063f, .01301f, .00054f, .0309f, 0 },
        { 3.5f, .02424f, .01091f, .01061f, .00897f, .0003f, .0613f, 0 },
        { 4.0f, .0243f, .01096f, .0106f, .00488f, .00027f, .1184f, 0 },
    };
    {
        Sweep s;
        s.Start(1.5f, OnePass(), kCtx);
        Run(s, replay(kCyberpunk));
        CHECK(s.Finished());
        CHECK(!s.Unsure());
        CHECK(!s.AtLimit()); // its best is bracketed, not at an end
        CHECK(Near(s.ResultEv(), -2.0f));
        CHECK(s.Changed());
    }

    // Its run 3 keeps the current value: detail and flicker both point to the darkest step, so the real best may lie
    // beyond the range. (The old flicker rule called it unsure; its input band drifts 8% across the steps, and detail
    // varies well beyond 4x that.)
    static const Row kCyberpunkNoisy[] = {
        { -3.0f, .00987f, .00614f, .00436f, .00151f, .00018f, 0, .0024f },
        { -2.5f, .00995f, .00617f, .00435f, .00282f, .0002f, 0, 0 },
        { -2.0f, .00957f, .00607f, .00427f, .00206f, .00034f, 0, 0 },
        { -1.5f, .00935f, .00592f, .00418f, .00229f, .00028f, 0, 0 },
        { -1.0f, .00893f, .00568f, .0041f, .00269f, .00024f, 0, 0 },
        { -0.5f, .00829f, .00537f, .00404f, .00363f, .00015f, 0, 0 },
        { 0.0f, .00767f, .00507f, .00404f, .0039f, .0002f, 0, 0 },
        { 0.5f, .00714f, .00483f, .00411f, .00498f, .00031f, 0, 0 },
        { 1.0f, .00659f, .00453f, .0042f, .00619f, .00025f, 0, 0 },
        { 1.5f, .00626f, .00427f, .00427f, .00798f, .00022f, 0, 0 },
        { 2.0f, .00607f, .00409f, .00431f, .01049f, .00016f, 0, 0 },
        { 2.5f, .00603f, .00398f, .0043f, .01173f, .00018f, 0, 0 },
        { 3.0f, .00599f, .00392f, .00423f, .01155f, .00033f, 0, 0 },
        { 3.5f, .00595f, .00387f, .00414f, .00798f, .00026f, .0012f, 0 },
        { 4.0f, .00602f, .0039f, .00406f, .00526f, .00022f, .017f, 0 },
    };
    {
        Sweep s;
        s.Start(1.5f, OnePass(), kCtx);
        Run(s, replay(kCyberpunkNoisy));
        CHECK(s.Finished() && Near(s.BestEv(Detail::BandPass), -3.0f) && s.Changed() && Near(s.ResultEv(), -3.0f));
    }

    // NBA 2K27 on the HLG and PQ curves and on Neutwo (OptiScaler.log, 2026-09-29 and -30): the flicker rule called the
    // first three unsure though they repeat. Their input band is the same to the last digit at every step.
    static const Row kHlg[] = { // 2026-09-30 11:14:24, HLG, started at -3.3 EV; two runs before it gave the same curve
        { -3.0f, .01666f, .00988f, .00995f, .00143f, .00012f, 0, .2424f },
        { -2.5f, .01646f, .00981f, .00995f, .00133f, .00013f, 0, .2087f },
        { -2.0f, .01629f, .00975f, .00995f, .00114f, .00012f, 0, .1814f },
        { -1.5f, .01622f, .00977f, .00995f, .00105f, .00013f, 0, .0863f },
        { -1.0f, .01617f, .00990f, .00995f, .00094f, .00012f, 0, .0415f },
        { -0.5f, .01600f, .00991f, .00995f, .00084f, .00013f, 0, .0178f },
        { 0.0f, .01599f, .00998f, .00995f, .00077f, .00012f, 0, .0066f },
        { 0.5f, .01617f, .01005f, .00995f, .00076f, .00013f, 0, .0010f },
        { 1.0f, .01613f, .01017f, .00995f, .00070f, .00012f, 0, 0 },
        { 1.5f, .01601f, .01017f, .00995f, .00067f, .00013f, 0, 0 },
        { 2.0f, .01601f, .01015f, .00995f, .00070f, .00012f, 0, 0 },
        { 2.5f, .01632f, .01014f, .00995f, .00074f, .00013f, .0119f, 0 },
        { 3.0f, .01677f, .01019f, .00995f, .00077f, .00012f, .1423f, 0 },
        { 3.5f, .01682f, .01021f, .00995f, .00085f, .00013f, .5433f, 0 },
        { 4.0f, .01715f, .01024f, .00995f, .00087f, .00012f, .5936f, 0 },
    };
    static const Row kHlgOther[] = { // 2026-09-30 11:23:52, HLG, another scene, started at +1.0 EV
        { -3.0f, .01284f, .00786f, .00879f, .00153f, .00010f, 0, .2100f },
        { -2.5f, .01275f, .00784f, .00879f, .00135f, .00011f, 0, .0643f },
        { -2.0f, .01261f, .00788f, .00879f, .00121f, .00010f, 0, .0287f },
        { -1.5f, .01260f, .00797f, .00879f, .00121f, .00011f, 0, .0182f },
        { -1.0f, .01258f, .00804f, .00879f, .00105f, .00010f, 0, .0128f },
        { -0.5f, .01243f, .00810f, .00879f, .00102f, .00011f, 0, .0090f },
        { 0.0f, .01232f, .00811f, .00879f, .00099f, .00010f, 0, .0054f },
        { 0.5f, .01223f, .00805f, .00879f, .00097f, .00011f, 0, .0027f },
        { 1.0f, .01239f, .00799f, .00879f, .00100f, .00010f, .0140f, .0011f },
        { 1.5f, .01263f, .00798f, .00879f, .00101f, .00011f, .1062f, .0001f },
        { 2.0f, .01263f, .00791f, .00879f, .00107f, .00010f, .3320f, 0 },
        { 2.5f, .01276f, .00796f, .00879f, .00103f, .00011f, .3531f, 0 },
        { 3.0f, .01283f, .00803f, .00879f, .00096f, .00010f, .3672f, 0 },
        { 3.5f, .01270f, .00822f, .00879f, .00086f, .00011f, .3834f, 0 },
        { 4.0f, .01278f, .00852f, .00879f, .00072f, .00010f, .4019f, 0 },
    };
    static const Row kPq[] = { // 2026-09-30 11:32:42, PQ, started at +0.5 EV: detail nearly flat, the floor decides
        { -3.0f, .03292f, .01983f, .01603f, .00150f, .00018f, 0, .3296f },
        { -2.5f, .03311f, .01997f, .01603f, .00155f, .00020f, 0, .2239f },
        { -2.0f, .03325f, .02009f, .01603f, .00140f, .00018f, 0, .1693f },
        { -1.5f, .03322f, .02011f, .01603f, .00140f, .00020f, 0, .1290f },
        { -1.0f, .03323f, .02015f, .01603f, .00133f, .00018f, 0, .0902f },
        { -0.5f, .03325f, .02022f, .01603f, .00136f, .00020f, 0, .0554f },
        { 0.0f, .03300f, .02011f, .01603f, .00139f, .00018f, 0, .0301f },
        { 0.5f, .03278f, .02003f, .01603f, .00142f, .00020f, 0, .0132f },
        { 1.0f, .03324f, .02017f, .01603f, .00140f, .00018f, .0030f, .0085f },
        { 1.5f, .03330f, .02015f, .01603f, .00147f, .00020f, .0628f, .0046f },
        { 2.0f, .03331f, .02011f, .01603f, .00142f, .00018f, .2279f, .0027f },
        { 2.5f, .03337f, .01999f, .01603f, .00149f, .00020f, .2819f, .0015f },
        { 3.0f, .03403f, .02013f, .01603f, .00151f, .00018f, .3214f, .0002f },
        { 3.5f, .03439f, .02026f, .01603f, .00155f, .00020f, .3631f, 0 },
        { 4.0f, .03405f, .02003f, .01603f, .00157f, .00018f, .4100f, 0 },
    };
    static const Row kNeutwo[] = { // 2026-09-29 09:27:50, Neutwo, started at +0.5 EV: the run that was always sure
        { -3.0f, .02870f, .01669f, .01613f, .00117f, .00021f, 0, .3609f },
        { -2.5f, .03066f, .01735f, .01613f, .00116f, .00021f, 0, .2635f },
        { -2.0f, .03188f, .01769f, .01613f, .00109f, .00021f, 0, .1400f },
        { -1.5f, .03262f, .01796f, .01613f, .00097f, .00021f, 0, .0605f },
        { -1.0f, .03286f, .01803f, .01613f, .00096f, .00021f, 0, .0254f },
        { -0.5f, .03286f, .01795f, .01612f, .00091f, .00021f, 0, .0129f },
        { 0.0f, .03275f, .01777f, .01613f, .00085f, .00021f, 0, .0057f },
        { 0.5f, .03263f, .01758f, .01612f, .00084f, .00021f, 0, .0021f },
        { 1.0f, .03245f, .01738f, .01613f, .00080f, .00021f, .0021f, .0008f },
        { 1.5f, .03239f, .01724f, .01612f, .00078f, .00021f, .0053f, .0003f },
        { 2.0f, .03221f, .01707f, .01613f, .00073f, .00021f, .0168f, .0001f },
        { 2.5f, .03205f, .01692f, .01613f, .00067f, .00021f, .1696f, .0001f },
        { 3.0f, .03178f, .01680f, .01613f, .00064f, .00021f, .2436f, 0 },
        { 3.5f, .03152f, .01667f, .01613f, .00060f, .00021f, .2819f, 0 },
        { 4.0f, .03123f, .01658f, .01613f, .00057f, .00021f, .3223f, 0 },
    };
    {
        struct Case { const Row* rows; float start, expect; const char* name; };
        for (const Case& c : { Case { kHlg, -3.3f, 1.5f, "HLG" }, Case { kHlgOther, 1.0f, 0.5f, "HLG, other scene" },
                               Case { kPq, 0.5f, 1.0f, "PQ" }, Case { kNeutwo, 0.5f, -0.5f, "Neutwo" } })
        {
            Sweep s;
            s.Start(c.start, OnePass(), kCtx);
            Run(s, replay(c.rows));
            CHECK(s.Finished() && !s.Unsure());
            CHECK(Near(s.ResultEv(), c.expect));
        }
    }

    // Unsure still means something: detail that varies less than the input does on its own across the steps (the
    // same frame measuring differently -- noise, or a scene that is not quite still) keeps the current value.
    {
        Sweep s;
        s.Start(1.0f, OnePass(), kCtx);
        Run(s, [](float ev) {
            Stats st = Peaked(ev, 2.0f);
            const int step = (int) std::lround((ev + 3.0f) * 2.0f);
            st.inputBand = 0.01f + ((step & 1) ? 0.0002f : -0.0002f); // spread 0.0004
            st.detailBand = st.inputBand + 0.001f + 0.0003f * st.detailRaw; // added detail varies 0.0003 at most
            return st;
        });
        CHECK(s.Finished() && s.Unsure() && Near(s.ResultEv(), 1.0f) && !s.Changed());
        // ...and a genuinely flat curve on a perfectly steady input is unsure too (range 0 is within the floor).
        Sweep f;
        f.Start(1.0f, OnePass(), kCtx);
        Run(f, [](float) {
            Stats st {};
            st.detailRaw = 0.02f;
            st.detailBand = 0.011f;
            st.inputBand = 0.01f;
            return st;
        });
        CHECK(f.Finished() && f.Unsure() && !f.Changed());
    }

    // The step on screen, for the menu's progress text.
    {
        Sweep s;
        s.Start(0.0f, OnePass(), kCtx);
        CHECK(s.StepIndex() == 0 && Near(s.StepEv(), -3.0f));
        for (unsigned i = 0; i < OnePass().firstSettle + OnePass().measure; ++i)
            s.NextFrame(kCtx);
        CHECK(s.StepIndex() == 1 && Near(s.StepEv(), -2.5f));
    }

    // An EV for display: never "-0.0".
    CHECK(!std::signbit(Tidy(-0.0f)));
    CHECK(!std::signbit(Tidy(-0.04f)) && Near(Tidy(-0.04f), 0.04f));
    CHECK(Near(Tidy(-0.5f), -0.5f) && Near(Tidy(1.25f), 1.25f));

    // Review Pass findings (2026-09-27).

    // A run in which nothing could be measured (every ticket came back empty) is not a result: it aborts.
    {
        Sweep s;
        s.Start(1.5f, OnePass(), kCtx);
        Run(s, [](float) {
            Stats none {};
            none.detailBand = NAN;
            return none;
        });
        CHECK(!s.Running() && !s.Finished());
        CHECK(s.AbortReason() == Abort::NothingMeasured);
    }

    // The end of the range is judged over the measured steps: with the first step lost, a best at the second counts as
    // the end -- and is offered, like any other best.
    {
        Sweep s;
        s.Start(0.0f, OnePass(), kCtx);
        Run(s, [](float ev) {
            Stats st = Peaked(ev, -2.5f);
            if (Near(ev, -3.0f))
                st.detailBand = NAN;
            return st;
        });
        CHECK(s.Finished());
        CHECK(s.AtLimit() && s.Changed() && Near(s.ResultEv(), -2.5f));
    }

    // A brief spell of unavailability (the game dropping its exposure texture for a frame) holds the run on the same
    // step without measuring, and does not trip the base check while it lasts; a long one aborts with its reason.
    {
        Context c = kCtx;
        c.baseWhitePoint = 2.0f;
        Sweep s;
        s.Start(0.0f, OnePass(), c);
        Context gone = c;
        gone.blocker = Blocker::NoGameExposure;
        gone.baseWhitePoint = 8.0f; // the fallback base jumps meanwhile
        for (unsigned i = 0; i < OnePass().unavailableTolerance; ++i)
        {
            const Frame f = s.NextFrame(gone);
            CHECK(s.Running());
            CHECK(f.override && !f.measure && Near(f.ev, -3.0f));
        }
        CHECK(s.StepIndex() == 0);
        CHECK(s.NextFrame(c).override && s.Running()); // back: carries on
        for (unsigned i = 0; i <= OnePass().unavailableTolerance; ++i)
            s.NextFrame(gone);
        CHECK(!s.Running() && s.AbortReason() == Abort::Unavailable);
        CHECK(s.StopBlocker() == Blocker::NoGameExposure);
    }

    // The 64x64 tile grid -> one Stats: means weighted by the pixels each tile measured; empty tiles left out; a grid
    // with nothing in it is an empty sample (NaN), not a zero.
    {
        std::vector<float> grid(kGridRowFloats * kGridTiles, 0.0f);
        auto tile = [&](unsigned x, unsigned y, float raw, float band, float in, float outChange, float inChange,
                        float shoulder, float floor, float pixels) {
            float* a = grid.data() + (size_t) y * kGridRowFloats + x * 4;
            float* b = grid.data() + (size_t) y * kGridRowFloats + (x + kGridTiles) * 4;
            a[0] = raw; a[1] = band; a[2] = in; a[3] = outChange;
            b[0] = inChange; b[1] = shoulder; b[2] = floor; b[3] = pixels;
        };
        CHECK(std::isnan(ReduceGrid(grid.data()).detailBand));
        tile(0, 0, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 0.5f, 0.0f, 100.0f);
        tile(63, 63, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 0.0f, 1.0f, 300.0f);
        const Stats st = ReduceGrid(grid.data());
        CHECK(Near(st.detailRaw, 2.5f) && Near(st.detailBand, 3.5f) && Near(st.inputBand, 4.5f));
        CHECK(Near(st.outputChange, 5.5f) && Near(st.inputChange, 6.5f));
        CHECK(Near(st.shoulder, 0.125f) && Near(st.floor, 0.75f));
    }

    // Availability, rule by rule.
    {
        Situation autoOk;
        autoOk.source = 3;
        autoOk.hdr = true;
        autoOk.autoRunning = true;
        CHECK(Availability(autoOk) == Blocker::None);

        Situation gameOk;
        gameOk.source = 1;
        gameOk.hdr = true;
        gameOk.gameExposureNow = true;
        gameOk.gameExposureReading = true;
        CHECK(Availability(gameOk) == Blocker::None);

        Situation t = autoOk;
        t.source = 0;
        CHECK(Availability(t) == Blocker::Source);
        t = autoOk;
        t.proxyMode = true;
        CHECK(Availability(t) == Blocker::ProxyMode);
        t = autoOk;
        t.holdFrame = true;
        CHECK(Availability(t) == Blocker::HoldFrame);
        t = autoOk;
        t.finishedPicture = true;
        CHECK(Availability(t) == Blocker::FinishedPicture);
        // A Colour encoding the shader converts (gamma 2.2, PQ) would measure linear input against encoded output.
        t = autoOk;
        t.colourConverted = true;
        CHECK(Availability(t) == Blocker::ColourConverted);
        CHECK(BlockerText(Blocker::ColourConverted)[0] != '\0');
        t = autoOk;
        t.hdr = false;
        CHECK(Availability(t) == Blocker::NotHdr);
        t = autoOk;
        t.autoRunning = false;
        CHECK(Availability(t) == Blocker::AutoNotRunning);
        // Follow never blocks: a run tunes against Automatic's own exposure whether Follow is learning, following or
        // off the mark (RDR2: 3.2 EV).
        t = autoOk;
        CHECK(!RelearnFollowOnStart(t)); // Follow off, or still learning: nothing to re-learn
        t.followLocked = true;
        t.followDisagreementEv = -4.3f;
        CHECK(Availability(t) == Blocker::None);
        // ... and a learned calibration is always learned again, however close it looks (0.9 EV is almost two steps
        // in the picture after the run), so the tuned Trim holds once Follow is back in force.
        CHECK(RelearnFollowOnStart(t));
        t.followDisagreementEv = 0.0f;
        CHECK(RelearnFollowOnStart(t));
        t = gameOk; // Game exposure never touches Follow
        t.followLocked = true;
        CHECK(Availability(t) == Blocker::None);
        CHECK(!RelearnFollowOnStart(t));
        t = gameOk;
        t.gameExposureNow = false;
        CHECK(Availability(t) == Blocker::NoGameExposure);
        t = gameOk;
        t.gameExposureReading = false;
        CHECK(Availability(t) == Blocker::NoGameExposure);
        t = gameOk;
        t.followLocked = true; // Follow belongs to Automatic; it does not block Game exposure
        CHECK(Availability(t) == Blocker::None);
        for (int b = 1; b <= (int) Blocker::NrStopped; ++b)
            CHECK(BlockerText((Blocker) b)[0] != '\0');
    }

    // The run controller (DlssNr_ExposureCalibrate_Run.h) with a fake GPU: a still scene, every measured evaluation
    // submitted, readbacks read back only once due, the resources given back only when nothing is in flight.
    {
        struct FakeGpu final : Backend
        {
            const RunState* run = nullptr;
            const unsigned long long* now = nullptr; // the evaluation being run
            unsigned long long submittedAt[kRing] = {};
            bool held = false;
            int creates = 0, releases = 0, reduces = 0, early = 0, releasedInFlight = 0;

            bool Held() const override { return held; }
            bool Create() override
            {
                ++creates;
                held = true;
                return true;
            }
            void Release() override
            {
                ++releases;
                for (bool busy : run->slotBusy)
                    releasedInFlight += busy ? 1 : 0;
                held = false;
            }
            Stats Reduce(unsigned int slot) override
            {
                ++reduces;
                early += (*now - submittedAt[slot] < kReadDelay) ? 1 : 0;
                Stats st {};
                st.detailRaw = st.detailBand = 0.1f;
                st.inputBand = 0.05f;
                return st;
            }
        };

        Situation ok {};
        ok.source = 3;
        ok.hdr = true;
        ok.autoRunning = true;
        const StartPoints start { 0.0f, 0.0f };
        const float base = 1.0f;

        // Drives `evaluations` NR evaluations from `evaluation`, 16 ms apart; `submit` submits every measured one,
        // stamped `submitOffset` evaluations on (Vulkan counts the evaluation before it submits: 1).
        const auto drive = [&](RunState& run, FakeGpu& gpu, unsigned long long& evaluation, unsigned long long& ms,
                               int evaluations, bool submit, int& started, int& finished, int& relearns,
                               const Situation& s, unsigned long long submitOffset = 0) {
            for (int i = 0; i < evaluations; ++i, ++evaluation, ms += 16)
            {
                if (!Wanted(run, ms))
                {
                    IdleFrame(run, ms);
                    continue;
                }
                const FrameEvents ev = BeginFrame(run, gpu, start, 2560, 1440, s, base, evaluation, ms);
                started += ev.started ? 1 : 0;
                finished += ev.finished ? 1 : 0;
                relearns += ev.relearnFollow ? 1 : 0;
                if (ev.started)
                    CHECK(run.measureWhitePoint > 0.0f);
                // A sweep pins the white point; a measure leaves it live.
                if (Active(run, gpu))
                    CHECK(run.measuring ? run.frameWhitePoint == 0.0f : run.frameWhitePoint > 0.0f);
                if (submit && Active(run, gpu) && run.measurePending)
                {
                    const unsigned int slot = FreeSlot(run);
                    CHECK(slot < kRing);
                    if (slot < kRing)
                    {
                        gpu.submittedAt[slot] = evaluation;
                        Submitted(run, slot, evaluation + submitOffset);
                    }
                }
            }
        };

        // A whole run, submitted: it starts once, finishes once with a result, reads nothing early, releases once and
        // only with nothing in flight, then goes quiet (no pin, not wanted).
        {
            RunState run;
            FakeGpu gpu;
            unsigned long long evaluation = 1, ms = 100000;
            gpu.run = &run;
            gpu.now = &evaluation;
            int started = 0, finished = 0, relearns = 0;

            CHECK(!Wanted(run, ms));
            IdleFrame(run, ms);
            RequestStart(run, 3);
            CHECK(Wanted(run, ms));
            drive(run, gpu, evaluation, ms, 800, true, started, finished, relearns, ok);
            CHECK(started == 1 && finished == 1 && relearns == 0);
            CHECK(run.sweep.Finished() && run.sweep.AbortReason() == Abort::None);
            CHECK(gpu.creates == 1 && gpu.releases == 1);
            CHECK(gpu.early == 0 && gpu.reduces > 0);
            CHECK(gpu.releasedInFlight == 0);
            CHECK(!gpu.held && !run.active.load());
            CHECK(run.frameWhitePoint == 0.0f && !Active(run, gpu));
            CHECK(!ResultLines(run.sweep).empty());
            bool stepLine = false;
            for (const std::string& line : ResultLines(run.sweep))
                stepLine = stepLine || (line.find("DLSS-NR calibrate: pass 1: -3.0 EV (trim ") == 0 &&
                                        line.find(") n 4 | raw 0.10000 band 0.05000 (out 0.10000 in 0.05000) | flicker ") !=
                                            std::string::npos &&
                                        line.find(" | score raw ") != std::string::npos);
            CHECK(stepLine);
            CHECK(!Wanted(run, ms + kMenuMs)); // the menu stopped looking a while ago
        }

        // Nothing ever submitted (another NR path ran): every ticket comes back empty, the run ends as NothingMeasured
        // rather than waiting forever, and the resources are given back.
        {
            RunState run;
            FakeGpu gpu;
            unsigned long long evaluation = 1, ms = 100000;
            gpu.run = &run;
            gpu.now = &evaluation;
            int started = 0, finished = 0, relearns = 0;
            IdleFrame(run, ms);
            RequestStart(run, 3);
            drive(run, gpu, evaluation, ms, 400, false, started, finished, relearns, ok);
            CHECK(finished == 1 && run.sweep.AbortReason() == Abort::NothingMeasured);
            CHECK(gpu.reduces == 0 && !gpu.held && !run.active.load());
        }

        // A start that cannot happen: the reason for the menu, no resources, not active.
        {
            RunState run;
            FakeGpu gpu;
            unsigned long long evaluation = 1, ms = 100000;
            gpu.run = &run;
            gpu.now = &evaluation;
            int started = 0, finished = 0, relearns = 0;
            Situation blocked = ok;
            blocked.hdr = false;
            IdleFrame(run, ms);
            RequestStart(run, 3);
            drive(run, gpu, evaluation, ms, 3, true, started, finished, relearns, blocked);
            CHECK(started == 0 && gpu.creates == 0 && std::string(run.startError).size() > 0);
            CHECK(!run.active.load());
        }

        // Follow locked on Automatic: the start asks for it to be learned again, once.
        {
            RunState run;
            FakeGpu gpu;
            unsigned long long evaluation = 1, ms = 100000;
            gpu.run = &run;
            gpu.now = &evaluation;
            int started = 0, finished = 0, relearns = 0;
            Situation follow = ok;
            follow.followLocked = true;
            IdleFrame(run, ms);
            RequestStart(run, 3);
            drive(run, gpu, evaluation, ms, 20, true, started, finished, relearns, follow);
            CHECK(started == 1 && relearns == 1);
        }

        // NR stops mid-run (no evaluation for longer than kStallMs): abandoned on the next evaluation, and the menu
        // meanwhile says NR is not running.
        {
            RunState run;
            FakeGpu gpu;
            unsigned long long evaluation = 1, ms = 100000;
            gpu.run = &run;
            gpu.now = &evaluation;
            int started = 0, finished = 0, relearns = 0;
            IdleFrame(run, ms);
            RequestStart(run, 3);
            drive(run, gpu, evaluation, ms, 40, true, started, finished, relearns, ok);
            CHECK(run.sweep.Running());
            ms += kStallMs + 1;
            {
                std::lock_guard<std::mutex> lock(run.mutex);
                CHECK(PollLocked(run, ms) == Blocker::NrStopped);
            }
            CHECK(!run.sweep.Running() && run.sweep.AbortReason() == Abort::NrOff);
            drive(run, gpu, evaluation, ms, 40, true, started, finished, relearns, ok);
            CHECK(gpu.releasedInFlight == 0 && !gpu.held);
            {
                std::lock_guard<std::mutex> lock(run.mutex);
                CHECK(PollLocked(run, ms) == Blocker::None); // running again, nothing in the way
            }
        }

        // Measure detail: every evaluation measured (the ring never runs out), the white point never pinned, Follow
        // not re-learned; the result kept beside the one before it.
        {
            RunState run;
            FakeGpu gpu;
            unsigned long long evaluation = 1, ms = 100000;
            gpu.run = &run;
            gpu.now = &evaluation;
            int started = 0, finished = 0, relearns = 0;
            Situation s = ok;
            s.followLocked = true;
            IdleFrame(run, ms);
            RequestMeasure(run);
            drive(run, gpu, evaluation, ms, 200, true, started, finished, relearns, s);
            CHECK(started == 1 && finished == 1 && relearns == 0);
            CHECK(run.sweep.Finished() && run.sweep.AbortReason() == Abort::None);
            CHECK(run.measurements == 1 && run.latest.samples == kMeasureEvaluations && run.previous.samples == 0);
            CHECK(Near(run.latest.detailBand, 0.1f) && Near(run.latest.inputBand, 0.05f));
            CHECK(gpu.early == 0 && gpu.releasedInFlight == 0 && !gpu.held && !run.active.load());
            const std::vector<std::string> lines = ResultLines(run.sweep);
            CHECK(lines.size() == 1 && lines[0].find("DLSS-NR measure: n 60 |") == 0);
            CHECK(lines[0].find("band 0.05000 (out 0.10000 in 0.05000)") != std::string::npos);

            CHECK(run.latestScale.source == 3 && Near(run.latestScale.whitePoint, run.measureWhitePoint));

            // Vulkan's timing (a slot due 17 evaluations on): still every sample, the ring does not run out.
            RequestMeasure(run);
            drive(run, gpu, evaluation, ms, 200, true, started, finished, relearns, s, 1);
            CHECK(run.measurements == 2 && run.previous.samples == kMeasureEvaluations);
            CHECK(run.latest.samples == kMeasureEvaluations && gpu.early == 0);
            CHECK(SameScale(run.latestScale, run.previousScale));

            // Follow's easing is held by a Tune, not by a measure.
            RequestMeasure(run);
            drive(run, gpu, evaluation, ms, 20, true, started, finished, relearns, ok);
            CHECK(run.active.load() && !HoldsFollow(run, ms));
            RequestCancel(run);
            drive(run, gpu, evaluation, ms, 40, true, started, finished, relearns, ok);
            CHECK(!run.active.load() && !HoldsFollow(run, ms));

            // A Tune after it is a Tune again: it starts, and the measure is over.
            RequestStart(run, 3);
            drive(run, gpu, evaluation, ms, 3, true, started, finished, relearns, s);
            CHECK(!run.measuring && std::string(run.startError).empty());
        }

        // A Tune result still offered (Apply / Keep) is not cleared by a measure; once dismissed, a measure starts.
        {
            RunState run;
            FakeGpu gpu;
            unsigned long long evaluation = 1, ms = 100000;
            gpu.run = &run;
            gpu.now = &evaluation;
            int started = 0, finished = 0, relearns = 0;
            IdleFrame(run, ms);
            RequestStart(run, 3);
            drive(run, gpu, evaluation, ms, 20, true, started, finished, relearns, ok);
            CHECK(HoldsFollow(run, ms)); // a Tune holds Follow's easing
            drive(run, gpu, evaluation, ms, 800, true, started, finished, relearns, ok);
            CHECK(run.sweep.Finished() && TuneResultWaiting(run));
            const size_t steps = run.sweep.Steps().size();
            RequestMeasure(run);
            CHECK(!run.measuring && !run.startRequested && run.sweep.Finished() && run.sweep.Steps().size() == steps);
            Dismiss(run);
            CHECK(!TuneResultWaiting(run));
            RequestMeasure(run);
            CHECK(run.measuring && run.startRequested);
        }

        // Before SR set: a Tune runs after SR from the moment it is asked for until it is over (its readbacks drained),
        // and starts only after NR has had kAfterSrSettle evaluations there; a measure does neither.
        {
            RunState run;
            FakeGpu gpu;
            unsigned long long evaluation = 1, ms = 100000;
            gpu.run = &run;
            gpu.now = &evaluation;
            int started = 0, finished = 0, relearns = 0;
            Situation before = ok;
            before.beforeSrSet = true;
            IdleFrame(run, ms);
            CHECK(!TuneRunsAfterSr(run, ms));
            RequestStart(run, 3);
            CHECK(TuneRunsAfterSr(run, ms)); // at once, so the next evaluation is already after SR
            drive(run, gpu, evaluation, ms, (int) kAfterSrSettle, true, started, finished, relearns, before);
            CHECK(started == 0 && run.startRequested && TuneRunsAfterSr(run, ms));
            drive(run, gpu, evaluation, ms, 1, true, started, finished, relearns, before);
            CHECK(started == 1 && run.sweep.Running() && TuneRunsAfterSr(run, ms));
            drive(run, gpu, evaluation, ms, 800, true, started, finished, relearns, before);
            CHECK(finished == 1 && run.sweep.Finished() && !run.active.load() && !TuneRunsAfterSr(run, ms));

            // Tune again: waits again.
            RequestStart(run, 3);
            drive(run, gpu, evaluation, ms, 5, true, started, finished, relearns, before);
            CHECK(started == 1 && TuneRunsAfterSr(run, ms));
            RequestCancel(run);
            drive(run, gpu, evaluation, ms, 40, true, started, finished, relearns, before);
            CHECK(!run.active.load() && !TuneRunsAfterSr(run, ms)); // a cancelled wait goes back too

            // A measure: before SR as set, no wait.
            RequestMeasure(run);
            CHECK(!TuneRunsAfterSr(run, ms));
            int mstarted = 0;
            drive(run, gpu, evaluation, ms, 1, true, mstarted, finished, relearns, before);
            CHECK(mstarted == 1);

            // After SR set: no wait.
            RunState plain;
            FakeGpu g2;
            unsigned long long e2 = 1;
            g2.run = &plain;
            g2.now = &e2;
            int s2 = 0;
            IdleFrame(plain, ms);
            RequestStart(plain, 3);
            drive(plain, g2, e2, ms, 1, true, s2, finished, relearns, ok);
            CHECK(s2 == 1);
        }

        // A Tune whose NR stopped calling in (a failed build after SR returns before BeginFrame) no longer counts after
        // kStallMs: the placement and Follow's easing are released even with nobody looking at the menu.
        {
            RunState run;
            const unsigned long long ms = 100000;
            IdleFrame(run, ms);
            RequestStart(run, 3);
            CHECK(TuneRunsAfterSr(run, ms) && HoldsFollow(run, ms));
            CHECK(!TuneRunsAfterSr(run, ms + kStallMs + 1) && !HoldsFollow(run, ms + kStallMs + 1));
        }

        // A start that is refused (a blocker) does not wait after SR, and lets go at once.
        {
            RunState run;
            FakeGpu gpu;
            unsigned long long evaluation = 1, t = 100000;
            gpu.run = &run;
            gpu.now = &evaluation;
            int started = 0, finished = 0, relearns = 0;
            Situation blocked = ok;
            blocked.beforeSrSet = true;
            blocked.hdr = false;
            IdleFrame(run, t);
            RequestStart(run, 3);
            drive(run, gpu, evaluation, t, 1, true, started, finished, relearns, blocked);
            CHECK(started == 0 && std::string(run.startError).size() > 0 && !TuneRunsAfterSr(run, t));
        }

        // Two results compare only at the same scale.
        {
            CHECK(SameScale({ 3, 1.508f }, { 3, 1.513f }));
            CHECK(!SameScale({ 3, 1.508f }, { 3, 1.26f }));
            CHECK(!SameScale({ 3, 1.5f }, { 1, 1.5f }));
            CHECK(!SameScale({ 3, 1.5f }, { 3, 0.0f }));
        }

        // Measure detail with nothing ever submitted: it says so, and gives everything back.
        {
            RunState run;
            FakeGpu gpu;
            unsigned long long evaluation = 1, ms = 100000;
            gpu.run = &run;
            gpu.now = &evaluation;
            int started = 0, finished = 0, relearns = 0;
            IdleFrame(run, ms);
            RequestMeasure(run);
            drive(run, gpu, evaluation, ms, 200, false, started, finished, relearns, ok);
            CHECK(finished == 1 && run.sweep.AbortReason() == Abort::NothingMeasured && run.measurements == 0);
            const std::vector<std::string> lines = ResultLines(run.sweep);
            CHECK(lines.size() == 1 && lines[0].find("DLSS-NR measure: stopped: nothing could be measured") == 0);
            CHECK(!gpu.held && !run.active.load());
        }

        // Cancel mid-run, and Shutdown: slots cleared, resources given back, no pin.
        {
            RunState run;
            FakeGpu gpu;
            unsigned long long evaluation = 1, ms = 100000;
            gpu.run = &run;
            gpu.now = &evaluation;
            int started = 0, finished = 0, relearns = 0;
            IdleFrame(run, ms);
            RequestStart(run, 3);
            drive(run, gpu, evaluation, ms, 60, true, started, finished, relearns, ok);
            RequestCancel(run);
            drive(run, gpu, evaluation, ms, 40, true, started, finished, relearns, ok);
            CHECK(run.sweep.AbortReason() == Abort::Cancelled && finished == 1);
            CHECK(gpu.releasedInFlight == 0 && !gpu.held && !run.active.load());

            RequestStart(run, 3);
            drive(run, gpu, evaluation, ms, 60, true, started, finished, relearns, ok);
            CHECK(gpu.held);
            Shutdown(run, gpu);
            CHECK(!gpu.held && !run.active.load() && run.frameWhitePoint == 0.0f && FreeSlot(run) == 0);
        }
    }

    // Measure detail's sweep: one step at the current value, the white point left alone, 8 evaluations to fill the
    // copies and then 60 measured, the means of them as the result, nothing scored or chosen.
    {
        Sweep s;
        s.Start(1.25f, MeasureSettings(3), kCtx);
        CHECK(s.Running() && s.StepCount() == 1 && Near(s.Steps()[0].ev, 1.25f));
        int measured = 0, settled = 0, n = 0;
        while (s.Running() && n < 1000)
        {
            const Frame f = s.NextFrame(kCtx);
            CHECK(!f.override && f.capture);
            if (f.measure)
            {
                Stats st {};
                st.detailRaw = 0.2f;
                st.detailBand = 0.1f + 0.01f * (float) (measured % 2); // a computed/reused alternation
                st.inputBand = 0.05f;
                st.outputChange = 0.003f;
                st.inputChange = 0.0002f;
                s.AddStats(f.ticket, st);
                ++measured;
            }
            else if (measured == 0)
                ++settled;
            ++n;
        }
        CHECK(s.Finished() && !s.Changed() && !s.Unsure());
        CHECK(settled == 8 && measured == (int) kMeasureEvaluations);
        const StepResult& r = s.Steps()[0];
        CHECK(r.samples == kMeasureEvaluations && Near(r.detailBand, 0.105f) && Near(r.DetailOf(Detail::BandPass), 0.055f));
        CHECK(Near(r.Flicker(), 0.0028f));

        // Game exposure measures at its own scale, as its sweep does.
        Sweep g;
        g.Start(0.0f, MeasureSettings(1), kCtx);
        CHECK(g.Config().neutralTrim == kGameExposureNeutralTrim && g.Config().measureOnly);

        // Fewer than half the evaluations back (failed copies, a full ring): not a result.
        Sweep f;
        f.Start(0.0f, MeasureSettings(3), kCtx);
        int given = 0;
        Run(f, [&given](float) {
            Stats st {};
            st.detailRaw = st.detailBand = 0.1f;
            st.inputBand = 0.05f;
            if (given++ % 3 != 0)
                st.detailBand = NAN; // two in three dropped
            return st;
        });
        CHECK(!f.Finished() && f.AbortReason() == Abort::TooFewMeasured);
        CHECK(ResultLines(f).size() == 1 && ResultLines(f)[0].find("DLSS-NR measure: stopped: too few") == 0);

        // Movement is measured again, as in a sweep, and the note says it is a measure.
        Sweep m;
        m.Start(0.0f, MeasureSettings(3), kCtx);
        int calls = 0;
        Run(m, [&calls](float) {
            Stats st {};
            st.detailRaw = st.detailBand = 0.1f;
            st.inputBand = 0.05f;
            st.inputChange = ++calls == 10 ? 0.01f : 0.0f;
            return st;
        });
        CHECK(m.Finished() && m.Steps()[0].samples == kMeasureEvaluations);
        const std::vector<Note> moved = m.TakeNotes();
        CHECK(moved.size() == 1 && NoteText(moved[0], true).find("DLSS-NR measure: the picture moved") == 0);
        CHECK(NoteText(moved[0]).find("DLSS-NR calibrate:") == 0);

        // Trim anchors stop nothing: a Tune's result becomes a point on the table.
        Situation a {};
        a.source = 3;
        a.hdr = true;
        a.autoRunning = true;
        CHECK(Availability(a) == Blocker::None);
    }

    // NR's output noisier than the game's input (Reuse alternating, a noisy still), no real trend, the input bit-identical
    // at every step: the step means' own spread keeps it unsure instead of offering a random value.
    {
        int changed = 0, unsure = 0;
        for (uint32_t seed = 1; seed <= 200; ++seed)
        {
            uint32_t state = seed * 2654435761u;
            auto noise = [&state]() { state = state * 1664525u + 1013904223u; return ((state >> 8) & 0xFFFF) / 32767.5f - 1.0f; };
            Sweep s;
            s.Start(1.0f, OnePass(), kCtx);
            Run(s, [&noise](float) {
                Stats st {};
                st.inputBand = 0.01f;
                st.detailBand = st.inputBand + 0.003f * (1.0f + 0.03f * noise()); // +-3% of the added detail
                st.detailRaw = 0.02f * (1.0f + 0.03f * noise());
                st.outputChange = 0.0008f;
                st.inputChange = 0.0002f;
                return st;
            });
            changed += s.Changed() ? 1 : 0;
            unsure += s.Unsure() ? 1 : 0;
        }
        printf("output noise +-3%%, no trend: %d of 200 changed, %d unsure\n", changed, unsure);
        CHECK(changed <= 4 && unsure >= 180);
    }

    // A late moved result for the last step, arriving after the sweep is back at the current value, sends it back more
    // than a step: that step settles long (firstSettle), as the first one does.
    {
        Sweep s;
        s.Start(1.5f, OnePass(), kCtx);
        std::vector<std::pair<int, Ticket>> pending; // due evaluation, ticket
        int n = 0, settleAfterRewind = -1, counting = -1;
        bool rewound = false;
        for (; n < 2000 && s.Running(); ++n)
        {
            const Frame f = s.NextFrame(kCtx);
            if (counting >= 0)
            {
                if (f.measure) { settleAfterRewind = counting; counting = -1; }
                else ++counting;
            }
            if (f.measure)
                pending.push_back({ n + 7, f.ticket });
            for (size_t i = 0; i < pending.size();)
            {
                if (pending[i].first <= n)
                {
                    Stats st = Peaked(f.ev, 1.0f);
                    const bool last = pending[i].second.step == s.StepCount() - 1;
                    if (last && !rewound && s.StepIndex() >= s.StepCount())
                    {
                        st.inputChange = 0.01f; // the late moved one
                        rewound = true;
                        counting = 0;
                    }
                    s.AddStats(pending[i].second, st);
                    pending.erase(pending.begin() + (std::ptrdiff_t) i);
                }
                else
                    ++i;
            }
        }
        CHECK(rewound && settleAfterRewind == (int) OnePass().firstSettle);
    }

    // Abandoned from outside, then cleared; Clear does nothing to a running sweep.
    {
        Sweep s;
        s.Start(0.0f, OnePass(), kCtx);
        s.Clear();
        CHECK(s.Running());
        s.Abandon(Abort::NrOff);
        CHECK(!s.Running() && s.AbortReason() == Abort::NrOff);
        s.Clear();
        CHECK(!s.Running() && !s.Finished() && s.AbortReason() == Abort::None && s.Steps().empty());
        CHECK(!s.NextFrame(kCtx).override);
    }

    // Not running: the frame asks for nothing and keeps the current EV.
    {
        Sweep s;
        CHECK(!s.Running());
        const Frame f = s.NextFrame(kCtx);
        CHECK(!f.override && !f.measure);
    }

    // Non-finite stats are dropped, not averaged in.
    {
        Sweep s;
        s.Start(0.0f, OnePass(), kCtx);
        Run(s, [](float ev) {
            Stats st = Peaked(ev, 0.5f);
            if (Near(ev, -3.0f))
                st.detailBand = NAN;
            return st;
        });
        CHECK(s.Finished());
        CHECK(Near(s.ResultEv(), 0.5f));
        CHECK(s.Steps()[0].samples == 0);
    }

    // Colour: of two steps with the same detail, the one whose output keeps the game's saturation wins; 10% more or less
    // chroma costs 0.05 at the default weight, either way.
    {
        Sweep s;
        s.Start(1.5f, OnePass(), kCtx);
        Run(s, [](float ev) {
            Stats st = Peaked(ev, -1.0f);
            st.chromaIn = 0.05f;
            // Flat detail from -1.5 to -0.5; -1.0 recolours by 30%, its neighbours by 2%.
            if (ev >= -1.5f - 1e-4f && ev <= -0.5f + 1e-4f)
                st.detailRaw = st.detailBand = 1.0f;
            st.chromaOut = 0.05f * (Near(ev, -1.0f) ? 1.3f : 1.02f);
            return st;
        });
        CHECK(s.Finished() && Near(s.ResultEv(), -1.5f)); // the first plateau step that does not recolour

        // The other way round: the recolouring step is the one a tie would hand it to, so only the colour term can move
        // the answer off it. Under-saturating costs the same as over-saturating.
        Sweep under;
        under.Start(1.5f, OnePass(), kCtx);
        Run(under, [](float ev) {
            Stats st = Peaked(ev, -1.0f);
            st.chromaIn = 0.05f;
            if (ev >= -1.5f - 1e-4f && ev <= -0.5f + 1e-4f)
                st.detailRaw = st.detailBand = 1.0f;
            st.chromaOut = 0.05f * (Near(ev, -1.5f) ? 0.7f : 0.98f); // -1.5 EV loses 30% of the chroma
            return st;
        });
        CHECK(under.Finished() && !Near(under.ResultEv(), -1.5f));

        // What the weight is worth: 30% off the chroma costs 0.15, so a step must bring more detail than that to win.
        const auto tradeColour = [](float advantage) {
            Sweep s;
            s.Start(1.5f, OnePass(), kCtx);
            Run(s, [&](float ev) {
                Stats st = Peaked(ev, -1.0f);
                st.chromaIn = 0.05f;
                if (ev >= -1.5f - 1e-4f && ev <= -0.5f + 1e-4f)
                    st.detailRaw = st.detailBand = 1.0f;
                st.chromaOut = 0.05f;
                if (Near(ev, -1.5f))
                {
                    st.detailRaw = st.detailBand = 1.0f + advantage;
                    st.chromaOut = 0.05f * 1.3f;
                }
                return st;
            });
            return s;
        };
        CHECK(!Near(tradeColour(0.05f).ResultEv(), -1.5f));
        CHECK(Near(tradeColour(0.25f).ResultEv(), -1.5f));

        // A picture with no colour in it is not compared at all: the same 30% swing on a near-grey frame scores nothing,
        // so the plateau's first step wins. (chromaIn below StepResult::kGreyChroma.)
        Sweep grey;
        grey.Start(1.5f, OnePass(), kCtx);
        Run(grey, [](float ev) {
            Stats st = Peaked(ev, -1.0f);
            st.chromaIn = 2e-6f;
            if (ev >= -1.5f - 1e-4f && ev <= -0.5f + 1e-4f)
                st.detailRaw = st.detailBand = 1.0f;
            st.chromaOut = st.chromaIn * (Near(ev, -1.5f) ? 20.0f : 1.0f);
            return st;
        });
        CHECK(grey.Finished() && Near(grey.ResultEv(), -1.5f));
        StepResult r;
        r.chromaIn = 0.05f;
        r.chromaOut = 0.045f;
        CHECK(Near(r.Saturation(), -0.1f));
        r.shadowIn = 0.02f;
        r.shadowOut = 0.015f;
        CHECK(Near(r.ShadowDarkening(), 0.25f));
    }

    // Shadows: of steps with the same detail, one that crushes 2% of the picture or darkens its shadows by 30% loses;
    // lifting them is free.
    {
        const auto flatWith = [](auto mark, float share = 0.4f) {
            Sweep s;
            s.Start(1.5f, OnePass(), kCtx);
            Run(s, [&](float ev) {
                Stats st = Peaked(ev, -1.0f);
                if (ev >= -1.5f - 1e-4f && ev <= -0.5f + 1e-4f)
                    st.detailRaw = st.detailBand = 1.0f;
                st.shadowShare = share;
                st.shadowIn = 0.02f;
                st.shadowOut = 0.02f;
                mark(ev, st);
                return st;
            });
            return s;
        };
        const Sweep crushed = flatWith([](float ev, Stats& st) {
            if (Near(ev, -1.5f))
                st.crushed = 0.02f;
        });
        CHECK(crushed.Finished() && Near(crushed.ResultEv(), -1.0f));
        const Sweep darkened = flatWith([](float ev, Stats& st) {
            if (Near(ev, -1.5f))
                st.shadowOut = 0.014f;
        });
        CHECK(darkened.Finished() && Near(darkened.ResultEv(), -1.0f));

        // Lifting is free, and it is not the tie-break saying so: the lifted step has less detail than the plateau, so
        // a term that paid for lifting would hand it the win.
        const Sweep lifted = flatWith([](float ev, Stats& st) {
            if (Near(ev, -1.5f))
            {
                st.shadowOut = 0.03f;
                st.detailRaw = st.detailBand = 0.99f;
            }
        });
        CHECK(lifted.Finished() && !Near(lifted.ResultEv(), -1.5f));

        // Hardly any shadow in the picture cannot veto a step: the same darkening over 0.2% of the frame is not scored,
        // so the plateau's first step wins as it would with no shadows at all.
        const Sweep slivers = flatWith(
            [](float ev, Stats& st) {
                if (Near(ev, -1.5f))
                    st.shadowOut = 0.002f; // 90% darker, but of almost nothing
            },
            0.002f);
        CHECK(slivers.Finished() && Near(slivers.ResultEv(), -1.5f));

        // The weights are worth what they say. Half the picture in shadow left 20% darker costs 0.1 at shadowWeight 1,
        // so a step has to bring more than that in detail to win; 2% of the picture crushed costs 0.2 at crushWeight 10.
        const auto tradeShadow = [](float advantage) {
            Sweep s;
            s.Start(1.5f, OnePass(), kCtx);
            Run(s, [&](float ev) {
                Stats st = Peaked(ev, -1.0f);
                if (ev >= -1.5f - 1e-4f && ev <= -0.5f + 1e-4f)
                    st.detailRaw = st.detailBand = 1.0f;
                st.shadowShare = 0.5f;
                st.shadowIn = 0.02f;
                st.shadowOut = 0.02f;
                if (Near(ev, -1.5f))
                {
                    st.detailRaw = st.detailBand = 1.0f + advantage;
                    st.shadowOut = 0.016f; // 20% darker over half the picture: 0.1
                }
                return st;
            });
            return s;
        };
        CHECK(!Near(tradeShadow(0.05f).ResultEv(), -1.5f)); // 0.05 of detail does not buy off 0.1 of shadows
        CHECK(Near(tradeShadow(0.15f).ResultEv(), -1.5f));  // 0.15 does

        const auto tradeCrush = [](float advantage) {
            Sweep s;
            s.Start(1.5f, OnePass(), kCtx);
            Run(s, [&](float ev) {
                Stats st = Peaked(ev, -1.0f);
                if (ev >= -1.5f - 1e-4f && ev <= -0.5f + 1e-4f)
                    st.detailRaw = st.detailBand = 1.0f;
                if (Near(ev, -1.5f))
                {
                    st.detailRaw = st.detailBand = 1.0f + advantage;
                    st.crushed = 0.02f; // 0.2 at crushWeight 10
                }
                return st;
            });
            return s;
        };
        CHECK(!Near(tradeCrush(0.1f).ResultEv(), -1.5f));
        CHECK(Near(tradeCrush(0.3f).ResultEv(), -1.5f));
    }

    // Measure detail in words.
    {
        StepResult r;
        r.detailBand = 0.015f;
        r.inputBand = 0.010f;
        r.outputChange = 0.0006f;
        r.inputChange = 0.0002f;
        r.chromaIn = 0.05f;
        r.chromaOut = 0.044f;
        r.warmth = -0.004f;
        r.shadowShare = 0.3f;
        r.shadowIn = 0.02f;
        r.shadowOut = 0.024f;
        r.crushed = 0.0f;
        CHECK(DetailWords(r) == "Detail: 50% more than the game's own");
        CHECK(FlickerWords(r) == "Flicker: the output moves 3.0x as much as the game's frame between frames (1x = none added)");
        CHECK(ColourWords(r) == "Colour: 12% less saturated than the game's, slightly cooler");
        CHECK(ShadowWords(r) == "Shadows (30% of the picture): 20% lifted, nothing crushed");
        StepResult before = r;
        before.detailBand = 0.014f;   // added 0.004 -> 0.005: +25%
        before.outputChange = 0.0006f; // the same flicker
        before.crushed = 0.004f;
        CHECK(CompareWords(r, before) == "more detail (+25%), flicker about the same, less crushed (-0.4 points)");
        StepResult smooth = r;
        smooth.detailBand = 0.008f;
        smooth.inputChange = 0.0f;
        CHECK(DetailWords(smooth) == "Detail: 20% less than the game's own (NR smooths here)");
        CHECK(FlickerWords(smooth).find("held still") != std::string::npos);
        // A frame that only looks still (a few millionths of change) must not print a multiple of hundreds.
        StepResult nearlyStill = r;
        nearlyStill.inputChange = 3e-6f;
        nearlyStill.outputChange = 2e-3f;
        CHECK(FlickerWords(nearlyStill).find("held still") != std::string::npos);
        // A comparison against nothing says so, rather than "about the same".
        StepResult wasStill = r, nowFlickers = r;
        wasStill.outputChange = wasStill.inputChange;   // Flicker() == 0
        nowFlickers.outputChange = nowFlickers.inputChange + 0.005f;
        CHECK(CompareWords(nowFlickers, wasStill).find("flicker was none, now") != std::string::npos);

        // Every clause of the words, not just the ones the first case happened to take.
        StepResult warm = r, cool = r;
        warm.warmth = 0.02f;
        cool.warmth = -0.02f;
        CHECK(ColourWords(warm).find(", warmer") != std::string::npos);
        CHECK(ColourWords(cool).find(", cooler") != std::string::npos);

        StepResult crushedALot = r;
        crushedALot.crushed = 0.004f;
        CHECK(ShadowWords(crushedALot).find("0.4% of the picture crushed") != std::string::npos);
        CHECK(ShadowWords(r).find("nothing crushed") != std::string::npos);

        // CompareWords' own saturation and shadow clauses, each on its own.
        StepResult moreColour = r;
        moreColour.chromaOut = r.chromaIn * 1.2f; // r is 12% less saturated, this is 20% more
        CHECK(CompareWords(moreColour, r).find("more saturated") != std::string::npos);
        CHECK(CompareWords(r, moreColour).find("less saturated") != std::string::npos);

        StepResult darkerShadows = r;
        darkerShadows.shadowOut = r.shadowIn * 0.5f; // r lifts its shadows; this darkens them
        CHECK(CompareWords(darkerShadows, r).find("shadows darker") != std::string::npos);
        CHECK(CompareWords(r, darkerShadows).find("shadows lighter") != std::string::npos);

        // The measured line reads the chroma the right way round: r's output has less chroma than its input.
        const std::string measured = MeasureText(r);
        CHECK(measured.find("saturation -12.0% (chroma out 0.0440 in 0.0500)") != std::string::npos);
    }

    // Two steps apart is not agreement: the window is one step, and these two peaks have no flat top to overlap.
    {
        Sweep s;
        s.Start(0.0f, Settings(), kCtx);
        Run(s, [&](float ev) { return Peaked(ev, s.Pass() == 0 ? -1.0f : -2.0f); });
        CHECK(s.Finished() && s.Unrepeated() && !s.Changed() && Near(s.ResultEv(), 0.0f));
    }

    // Two passes by default (Measure detail one): a run offers a change only when they agree.
    CHECK(Settings {}.passes == 2 && MeasureSettings(3).passes == 1 && MeasureSettings(1).passes == 1);

    // Both passes find -1.0 EV: offered. Twice the evaluations of one pass, and progress reaches 1 only at the end.
    {
        Sweep one, two;
        one.Start(1.5f, OnePass(), kCtx);
        two.Start(1.5f, Settings {}, kCtx);
        CHECK(two.Passes() == 2 && two.Pass() == 0);
        const int oneEvaluations = Run(one, [](float ev) { return Peaked(ev, -1.0f); });
        float lastProgress = 0.0f;
        bool monotonic = true, halfway = false;
        const int twoEvaluations = Run(two, [&](float ev) {
            monotonic = monotonic && two.Progress() + 1e-4f >= lastProgress;
            lastProgress = two.Progress();
            halfway = halfway || (two.Pass() == 1 && two.Progress() > 0.45f && two.Progress() < 0.6f);
            return Peaked(ev, -1.0f);
        });
        CHECK(two.Finished() && !two.Unrepeated() && Near(two.ResultEv(), -1.0f) && two.Changed());
        CHECK(Near(two.FirstPass().result, -1.0f) && Near(two.LastPass().result, -1.0f));
        CHECK(two.FirstPassSteps().size() == two.StepCount());
        CHECK(twoEvaluations > 2 * oneEvaluations - 20 && twoEvaluations < 2 * oneEvaluations + 20);
        CHECK(monotonic && halfway);
        CHECK(Near(two.Progress(), 1.0f) || two.Progress() > 0.99f);

        // The log names each pass and what it concluded.
        bool pass1 = false, pass2 = false, verdict1 = false;
        for (const std::string& line : ResultLines(two))
        {
            pass1 = pass1 || line.find("DLSS-NR calibrate: pass 1: -3.0 EV (trim ") == 0;
            pass2 = pass2 || line.find("DLSS-NR calibrate: pass 2: -3.0 EV (trim ") == 0;
            verdict1 = verdict1 || line.find("DLSS-NR calibrate: pass 1: best raw -1.0 EV, best band -1.0 EV, result "
                                             "-1.00 EV") == 0;
        }
        CHECK(pass1 && pass2 && verdict1);
    }

    // The passes disagree (+2.0 EV, then -1.0 EV -- a run that jumped, as NBA 2K27 and RDR2 did): nothing is offered.
    {
        Sweep s;
        s.Start(0.5f, Settings {}, kCtx);
        Run(s, [&](float ev) { return Peaked(ev, s.Pass() == 0 ? 2.0f : -1.0f); });
        CHECK(s.Finished() && s.Unrepeated() && !s.Unsure() && !s.Changed() && Near(s.ResultEv(), 0.5f));
        CHECK(Near(s.FirstPass().result, 2.0f) && Near(s.LastPass().result, -1.0f));
        bool said = false;
        for (const std::string& line : ResultLines(s))
            said = said || line.find("(did not repeat: pass 1 gave +2.0 EV, pass 2 -1.0 EV, keeps the current value)") !=
                               std::string::npos;
        CHECK(said);
    }

    // One step apart: they agree, and the smaller move is offered (the one nearer the current value).
    {
        Sweep s;
        s.Start(1.5f, Settings {}, kCtx);
        Run(s, [&](float ev) { return Peaked(ev, s.Pass() == 0 ? -1.0f : -0.5f); });
        CHECK(s.Finished() && !s.Unrepeated() && Near(s.ResultEv(), -0.5f) && s.Changed());
    }

    // A pass that keeps the current value (flat) against one that moves: that is disagreement too.
    {
        Sweep s;
        s.Start(2.3f, Settings {}, kCtx);
        Run(s, [&](float ev) {
            if (s.Pass() == 0)
            {
                Stats st {};
                st.detailRaw = st.detailBand = 1.0f + (Near(ev, 0.0f) ? 0.01f : 0.0f);
                st.inputBand = 0.1f;
                return st;
            }
            return Peaked(ev, -1.0f);
        });
        CHECK(s.Finished() && s.Unrepeated() && Near(s.ResultEv(), 2.3f) && !s.Changed());
    }

    // A flat top: -2.0 .. -1.0 EV score alike, and the passes take its two ends (as The Witcher 3 did). They agree; the
    // result is the end nearer the current value. From on the top, each pass keeps the current value, and so does the run.
    {
        const auto plateau = [](float start) {
            Sweep s;
            s.Start(start, Settings {}, kCtx);
            Run(s, [&](float ev) {
                Stats st = Peaked(ev, -1.5f);
                if (ev >= -2.0f - 1e-4f && ev <= -1.0f + 1e-4f)
                    st.detailRaw = st.detailBand = 1.0f + (s.Pass() == 0 ? 0.005f : -0.005f) * (ev + 1.5f);
                return st;
            });
            return s;
        };
        const Sweep below = plateau(-2.5f), inside = plateau(-1.5f), above = plateau(1.5f);
        CHECK(Near(below.FirstPass().result, -1.0f) && Near(below.LastPass().result, -2.0f));
        CHECK(below.Finished() && !below.Unrepeated() && Near(below.ResultEv(), -2.0f) && below.Changed());
        CHECK(inside.Finished() && !inside.Unrepeated() && !inside.Changed());
        CHECK(above.Finished() && !above.Unrepeated() && Near(above.ResultEv(), -1.0f));
    }

    // An unsure pass makes the run unsure.
    {
        Sweep s;
        s.Start(1.5f, Settings {}, kCtx);
        Run(s, [&](float ev) {
            if (s.Pass() == 0)
                return Peaked(ev, -1.0f);
            Stats st {};
            st.detailRaw = st.detailBand = 0.5f;
            st.inputBand = 0.1f;
            return st;
        });
        CHECK(s.Finished() && s.Unsure() && !s.Unrepeated() && !s.Changed());
    }

    // The view changes during the second pass: both passes start over (the first measured the other view).
    {
        Sweep s;
        s.Start(1.5f, Settings {}, kCtx);
        bool sawPass2 = false, backToPass1 = false;
        int evaluations = 0;
        Run(s, [&](float ev) {
            sawPass2 = sawPass2 || s.Pass() == 1;
            backToPass1 = backToPass1 || (sawPass2 && s.Pass() == 0);
            Stats st = Peaked(ev, -1.0f);
            // From the middle of pass 2 on the camera rests elsewhere.
            st.inputBand = sawPass2 && ++evaluations > 20 ? 0.2f : 0.1f;
            return st;
        });
        CHECK(sawPass2 && backToPass1);
        CHECK(s.Finished() && Near(s.ResultEv(), -1.0f));
    }

    // Tune target Natural (Settings::floorBelowGameEv): no step darker than 1.5 EV under the game's own exposure is
    // swept or offered, on a scene whose detail keeps rising toward the dark end (what the score does when nothing
    // clips).
    {

        // Game exposure: its slider's 0 EV is the game's exposure, so the floor is -1.5 EV. A current value under it
        // (NBA 2K27's tuned -3.5 EV) is moved up to the best step allowed.
        Settings g = GameExposureSettings();
        g.passes = 1;
        g.floorBelowGameEv = kNaturalFloorEv;
        const Context game { 2560, 1440, 1, true, 0.75f, Blocker::None, 0.75f };
        Sweep s;
        s.Start(-3.5f, g, game);
        CHECK(s.HasFloor() && Near(s.FloorEv(), -1.5f));
        CHECK(s.StepCount() == 8 && Near(s.Steps().front().ev, -1.5f));
        Run(s, DarkFavoured, 4, game);
        CHECK(s.Finished() && s.Changed() && Near(s.ResultEv(), -1.5f));
        CHECK(ResultLines(s).back().find("target Natural, no darker than -1.50 EV") != std::string::npos);

        // Max detail on the same scene: no floor, the dark end as before.
        g.floorBelowGameEv = 0.0f;
        Sweep m;
        m.Start(-3.5f, g, game);
        CHECK(!m.HasFloor() && m.StepCount() == 16);
        Run(m, DarkFavoured, 4, game);
        CHECK(m.Finished() && Near(m.ResultEv(), -5.5f));

        // Two passes, the shipped default: both land on the floor and agree.
        g.floorBelowGameEv = kNaturalFloorEv;
        g.passes = 2;
        Sweep two;
        two.Start(-3.5f, g, game);
        Run(two, DarkFavoured, 4, game);
        CHECK(two.Finished() && !two.Unrepeated() && Near(two.ResultEv(), -1.5f));
    }
    {
        // Automatic: the floor goes on its slider through the two bases. Game white point 2, Automatic's 1 at its 5x
        // neutral: the game's exposure sits at -log2(2 / 5) = +1.32 EV there, the floor at -0.18 EV, so the first
        // step on the usual grid is 0.0 EV.
        Settings a = OnePass();
        a.floorBelowGameEv = kNaturalFloorEv;
        const Context autoCtx { 2560, 1440, 3, true, 1.0f, Blocker::None, 2.0f };
        Sweep s;
        s.Start(1.5f, a, autoCtx);
        CHECK(s.HasFloor() && Near(s.FloorEv(), -std::log2(2.0f / 5.0f) - 1.5f));
        CHECK(Near(s.Steps().front().ev, 0.0f) && s.StepCount() == 9);
        Run(s, DarkFavoured, 4, autoCtx);
        CHECK(s.Finished() && Near(s.ResultEv(), 0.0f));

        // A best above the floor is not touched by it.
        Sweep above;
        above.Start(1.5f, a, autoCtx);
        Run(above, [](float ev) { return Peaked(ev, 2.0f); }, 4, autoCtx);
        CHECK(above.Finished() && Near(above.ResultEv(), 2.0f));

        // The game gives no exposure: Natural has nothing to put the floor on and sweeps like Max detail.
        Context none = autoCtx;
        none.gameBaseWhitePoint = 0.0f;
        Sweep n;
        n.Start(1.5f, a, none);
        CHECK(!n.HasFloor() && n.StepCount() == 15);
        Run(n, DarkFavoured, 4, none);
        CHECK(n.Finished() && Near(n.ResultEv(), -3.0f));
        CHECK(n.NaturalWanted() && ResultLines(n).back().find("tuned as Max detail") != std::string::npos);

        // A game exposure far brighter than Automatic's would put the floor past the range: two steps are left.
        Context bright = autoCtx;
        bright.gameBaseWhitePoint = 0.1f;
        Sweep b;
        b.Start(1.5f, a, bright);
        CHECK(b.HasFloor() && b.StepCount() == 2 && Near(b.Steps().front().ev, 3.5f));

        // A run that gives no result keeps the current value, even under the floor: only a result moves it.
        Settings g = GameExposureSettings();
        g.passes = 1;
        g.floorBelowGameEv = kNaturalFloorEv;
        const Context game { 2560, 1440, 1, true, 0.75f, Blocker::None, 0.75f };
        Sweep flat;
        flat.Start(-3.5f, g, game);
        Run(
            flat,
            [](float)
            {
                Stats st {};
                st.detailRaw = st.detailBand = 0.5f;
                st.inputBand = 0.1f;
                return st;
            },
            4, game);
        CHECK(flat.Finished() && flat.Unsure() && !flat.Changed() && Near(flat.ResultEv(), -3.5f));

        // Measure detail ignores the floor: it measures the current value as it is.
        Settings ms = MeasureSettings(1);
        ms.floorBelowGameEv = kNaturalFloorEv;
        Sweep me;
        me.Start(-3.5f, ms, game);
        CHECK(!me.HasFloor() && me.StepCount() == 1 && Near(me.Steps().front().ev, -3.5f));
    }

    if (fails == 0)
        printf("all passed\n");
    return fails == 0 ? 0 : 1;
}
