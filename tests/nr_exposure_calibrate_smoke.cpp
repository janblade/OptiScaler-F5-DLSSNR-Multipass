// Host check of DlssNr_ExposureCalibrate.h: the "Tune for this scene" sweep over the model input brightness. No GPU
// and no game needed.
// cl /std:c++20 /EHsc /W4 tests/nr_exposure_calibrate_smoke.cpp
#include "../OptiScaler/shaders/dlssnr/DlssNr_ExposureCalibrate.h"
#include "../OptiScaler/shaders/dlssnr/DlssNr_ExposureCalibrate_Run.h"

#include <cmath>
#include <cstdio>
#include <functional>
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
        s.Start(1.5f, Settings {}, kCtx);
        CHECK(s.Running());
        CHECK(s.StepCount() == 15);

        std::vector<float> evs;
        std::vector<int> measured;
        int n = 0;

        const int total = 15 * 12 + (int) (Settings {}.firstSettle - Settings {}.settle);
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
        t.Start(0.0f, Settings {}, kCtx);
        for (unsigned i = 0; i < Settings {}.firstSettle + Settings {}.measure; ++i)
            t.NextFrame(kCtx);
        for (int i = 0; i < 8; ++i)
            CHECK(!t.NextFrame(kCtx).measure);
        CHECK(t.NextFrame(kCtx).measure);
    }

    // The measured count is even, so Reuse bottleneck's computed/reused alternation is covered equally.
    CHECK(Settings {}.measure % 2 == 0);

    // Picks the highest score, with results arriving four evaluations late.
    {
        Sweep s;
        s.Start(1.5f, Settings {}, kCtx);
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
        s.Start(0.0f, Settings {}, kCtx);
        Run(s, [](float ev) { return Peaked(ev, -1.0f); }, lag);
        CHECK(s.Finished());
        CHECK(Near(s.ResultEv(), -1.0f));
    }

    // A step with more detail that flickers loses to a stable one.
    {
        Sweep s;
        s.Start(0.0f, Settings {}, kCtx);
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
        Settings loose;
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
        s.Start(0.0f, Settings {}, kCtx);
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
        Settings raw;
        raw.detail = Detail::Raw;
        Settings band;
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

    // Camera motion aborts.
    {
        Sweep s;
        s.Start(0.0f, Settings {}, kCtx);
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
        s.Start(0.0f, Settings {}, kCtx);
        s.NextFrame(kCtx);
        s.NextFrame(c);
        CHECK(s.AbortReason() == Abort::Resolution);

        c = kCtx;
        c.nrEnabled = false;
        s.Start(0.0f, Settings {}, kCtx);
        s.NextFrame(c);
        CHECK(s.AbortReason() == Abort::NrOff);

        c = kCtx;
        c.whitePointSource = 1;
        s.Start(0.0f, Settings {}, kCtx);
        s.NextFrame(c);
        CHECK(s.AbortReason() == Abort::SourceChanged);

        s.Start(0.0f, Settings {}, kCtx);
        s.Cancel();
        CHECK(!s.Running() && s.AbortReason() == Abort::Cancelled);
    }

    // The base white point (the exposure) is pinned for the run: a move of more than 0.25 EV aborts, a smaller one does
    // not, and an unknown base (0) is never compared.
    {
        Context c = kCtx;
        c.baseWhitePoint = 2.0f;
        Sweep s;
        s.Start(0.0f, Settings {}, c);
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
        u.Start(0.0f, Settings {}, c);
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
        left.Start(-3.3f, Settings {}, c);
        right.Start(4.3f, Settings {}, c);
        CHECK(Near(left.MeasureWhitePoint(), right.MeasureWhitePoint()));
        CHECK(Near(left.MeasureWhitePoint(), 0.39f * TrimForEv(0.0f)));
    }

    // Game exposure has its own slider: 0 EV is Trim 1 (the game's exposure as is), and the sweep runs -5.5 .. +2 EV,
    // inside that slider's range, measured at the game's own exposure.
    {
        const Settings g = GameExposureSettings();
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
        CHECK(Near(Settings {}.neutralTrim, 5.0f) && Near(TrimForEv(0.0f), 5.0f));
    }

    // The white point of a step: the frozen base times that step's Trim, within the pass's clamp.
    {
        Context c = kCtx;
        c.baseWhitePoint = 2.0f;
        Sweep s;
        s.Start(0.0f, Settings {}, c);
        const Frame f = s.NextFrame(c);
        CHECK(Near(f.ev, -3.0f));
        CHECK(Near(s.WhitePointFor(f.ev), 2.0f * TrimForEv(-3.0f)));
        Context huge = kCtx;
        huge.baseWhitePoint = 1e6f;
        Sweep h;
        h.Start(0.0f, Settings {}, huge);
        CHECK(Near(h.WhitePointFor(-3.0f), 4096.0f));
    }

    // Stats that arrive after an abort, or with a ticket from an earlier run, are ignored.
    {
        Sweep s;
        s.Start(0.0f, Settings {}, kCtx);
        Frame f {};
        for (unsigned i = 0; i <= Settings {}.firstSettle; ++i)
            f = s.NextFrame(kCtx);
        CHECK(f.measure);
        s.Start(0.0f, Settings {}, kCtx);
        s.AddStats(f.ticket, Peaked(-3.0f, -3.0f));
        CHECK(s.Steps()[0].samples == 0);
    }

    // A flat curve keeps the current value: the best is within the tolerance of the step nearest to it.
    {
        Sweep s;
        s.Start(2.3f, Settings {}, kCtx);
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
        s.Start(2.3f, Settings {}, kCtx);
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

        Settings wide;
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
        s.Start(1.5f, Settings {}, kCtx);
        Run(s, replay(kPaused));
        CHECK(s.Finished());
        CHECK(!s.Unsure());
        CHECK(s.ResultEv() >= -1.0f - 1e-4f && s.ResultEv() <= 0.0f + 1e-4f);
        CHECK(Near(s.BestEv(Detail::Raw), 0.0f));
        CHECK(s.Changed());
    }

    // A run whose best is the first or last step (NBA 2K27 with Follow-game's stale -3.42 EV calibration, every step
    // 4.3 EV brighter than its label): the real best may lie beyond the range, so nothing is offered.
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
        s.Start(-1.2f, Settings {}, kCtx);
        Run(s, replay(kEdge));
        CHECK(s.Finished());
        CHECK(s.AtEdge());
        CHECK(Near(s.ResultEv(), -1.2f));
        CHECK(!s.Changed());
    }

    // Not at the edge: the still, paused and synthetic peaks inside the range.
    {
        Sweep s;
        s.Start(1.5f, Settings {}, kCtx);
        Run(s, replay(kStill));
        CHECK(!s.AtEdge());
        Sweep p;
        p.Start(2.0f, Settings {}, kCtx);
        Run(p, [](float ev) { return Peaked(ev, 4.0f); });
        CHECK(p.AtEdge()); // the top step is the edge too
    }

    // Follow-game's base against Automatic's own, in EV; unknown (0) never disagrees.
    CHECK(Near(BaseDisagreementEv(0.0708f, 1.361f), std::log2(0.0708f / 1.361f)));
    CHECK(std::fabs(BaseDisagreementEv(0.0708f, 1.361f)) > kFollowDisagreementLimitEv);
    CHECK(std::fabs(BaseDisagreementEv(1.2f, 1.361f)) < kFollowDisagreementLimitEv);
    CHECK(BaseDisagreementEv(0.0f, 1.361f) == 0.0f && BaseDisagreementEv(1.0f, 0.0f) == 0.0f);

    // The first step settles longer: it is the one big jump, from the current value to the bottom of the sweep.
    {
        Sweep s;
        s.Start(1.5f, Settings {}, kCtx);
        for (unsigned i = 0; i < Settings {}.firstSettle; ++i)
            CHECK(!s.NextFrame(kCtx).measure);
        CHECK(s.NextFrame(kCtx).measure);
    }

    // The still run: band-pass +0.0 EV, raw +0.5 EV, confidently, from the +1.5 EV default.
    {
        Sweep s;
        s.Start(1.5f, Settings {}, kCtx);
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
        s.Start(0.0f, Settings {}, kCtx);
        Run(s, replay(kMoving));
        CHECK(!s.Finished() && s.AbortReason() == Abort::Motion);
    }

    // ...and with the limit lifted it is unsure (flicker swings more than detail does), so it keeps the current value.
    {
        Settings loose;
        loose.motionLimit = 1.0f;
        Sweep s;
        s.Start(0.0f, loose, kCtx);
        Run(s, replay(kMoving));
        CHECK(s.Finished());
        CHECK(s.Unsure());
        CHECK(Near(s.ResultEv(), 0.0f));
        CHECK(!s.Changed());
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
        s.Start(1.5f, Settings {}, kCtx);
        Run(s, replay(kCyberpunk));
        CHECK(s.Finished());
        CHECK(!s.Unsure());
        CHECK(!s.AtEdge());
        CHECK(Near(s.ResultEv(), -2.0f));
        CHECK(s.Changed());
    }

    // Its run 3 stays unsure: a flicker spike at -2.5 EV next to a near-flat bottom, too little detail spread to call.
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
        s.Start(1.5f, Settings {}, kCtx);
        Run(s, replay(kCyberpunkNoisy));
        CHECK(s.Finished());
        CHECK(s.Unsure());
        CHECK(!s.Changed());
    }

    // The step on screen, for the menu's progress text.
    {
        Sweep s;
        s.Start(0.0f, Settings {}, kCtx);
        CHECK(s.StepIndex() == 0 && Near(s.StepEv(), -3.0f));
        for (unsigned i = 0; i < Settings {}.firstSettle + Settings {}.measure; ++i)
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
        s.Start(1.5f, Settings {}, kCtx);
        Run(s, [](float) {
            Stats none {};
            none.detailBand = NAN;
            return none;
        });
        CHECK(!s.Running() && !s.Finished());
        CHECK(s.AbortReason() == Abort::NothingMeasured);
    }

    // The edge is judged over the measured steps: with the first step lost, a best at the second is still the edge.
    {
        Sweep s;
        s.Start(0.0f, Settings {}, kCtx);
        Run(s, [](float ev) {
            Stats st = Peaked(ev, -2.5f);
            if (Near(ev, -3.0f))
                st.detailBand = NAN;
            return st;
        });
        CHECK(s.Finished() && s.AtEdge());
        CHECK(Near(s.ResultEv(), 0.0f));
    }

    // A brief spell of unavailability (the game dropping its exposure texture for a frame) holds the run on the same
    // step without measuring, and does not trip the base check while it lasts; a long one aborts with its reason.
    {
        Context c = kCtx;
        c.baseWhitePoint = 2.0f;
        Sweep s;
        s.Start(0.0f, Settings {}, c);
        Context gone = c;
        gone.blocker = Blocker::NoGameExposure;
        gone.baseWhitePoint = 8.0f; // the fallback base jumps meanwhile
        for (unsigned i = 0; i < Settings {}.unavailableTolerance; ++i)
        {
            const Frame f = s.NextFrame(gone);
            CHECK(s.Running());
            CHECK(f.override && !f.measure && Near(f.ev, -3.0f));
        }
        CHECK(s.StepIndex() == 0);
        CHECK(s.NextFrame(c).override && s.Running()); // back: carries on
        for (unsigned i = 0; i <= Settings {}.unavailableTolerance; ++i)
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
        t = autoOk;
        t.anchors = true;
        CHECK(Availability(t) == Blocker::Anchors);
        t = gameOk;
        t.gameExposureNow = false;
        CHECK(Availability(t) == Blocker::NoGameExposure);
        t = gameOk;
        t.gameExposureReading = false;
        CHECK(Availability(t) == Blocker::NoGameExposure);
        t = gameOk;
        t.anchors = true;
        CHECK(Availability(t) == Blocker::Anchors);
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

        // Drives `evaluations` NR evaluations from `evaluation`, 16 ms apart; `submit` submits every measured one.
        const auto drive = [&](RunState& run, FakeGpu& gpu, unsigned long long& evaluation, unsigned long long& ms,
                               int evaluations, bool submit, int& started, int& finished, int& relearns,
                               const Situation& s) {
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
                if (Active(run, gpu))
                    CHECK(run.frameWhitePoint > 0.0f);
                if (submit && Active(run, gpu) && run.measurePending)
                {
                    const unsigned int slot = FreeSlot(run);
                    CHECK(slot < kRing);
                    if (slot < kRing)
                    {
                        gpu.submittedAt[slot] = evaluation;
                        Submitted(run, slot, evaluation);
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
            drive(run, gpu, evaluation, ms, 400, true, started, finished, relearns, ok);
            CHECK(started == 1 && finished == 1 && relearns == 0);
            CHECK(run.sweep.Finished() && run.sweep.AbortReason() == Abort::None);
            CHECK(gpu.creates == 1 && gpu.releases == 1);
            CHECK(gpu.early == 0 && gpu.reduces > 0);
            CHECK(gpu.releasedInFlight == 0);
            CHECK(!gpu.held && !run.active.load());
            CHECK(run.frameWhitePoint == 0.0f && !Active(run, gpu));
            CHECK(!ResultLines(run.sweep).empty());
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
            blocked.anchors = true;
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

    // Abandoned from outside, then cleared; Clear does nothing to a running sweep.
    {
        Sweep s;
        s.Start(0.0f, Settings {}, kCtx);
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
        s.Start(0.0f, Settings {}, kCtx);
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

    if (fails == 0)
        printf("all passed\n");
    return fails == 0 ? 0 : 1;
}
