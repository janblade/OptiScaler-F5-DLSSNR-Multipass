#include "pch.h"
#include "../../OptiScaler/dlssnr/DlssNr_SuperiorPrivatePolicy.h"
#include "../../OptiScaler/dlssnr/DlssNr_SuperiorScaling.h"
#include "../../OptiScaler/dlssnr/DlssNr_InterPassSchedule.h"
#include "../../OptiScaler/shaders/dlssnr/superior/Constants.h"
using namespace DlssNr::Superior;
static_assert(!DlssNr::SuperiorScaling::NeedsGhostField(DlssNr::SuperiorScaling::NoPrivateDlss));
static_assert(!DlssNr::SuperiorScaling::NeedsGhostField(DlssNr::SuperiorScaling::CnnD));
static_assert(!DlssNr::SuperiorScaling::NeedsGhostField(DlssNr::SuperiorScaling::CnnE));
static_assert(!DlssNr::SuperiorScaling::NeedsGhostField(0)); // Runtime default after a rejected preset hint.
static_assert(DlssNr::SuperiorScaling::NeedsGhostField(DlssNr::SuperiorScaling::TransformerK));
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Guides.h"
#include <optional>

template<class T> struct Setting { T value; T value_or_default() const { return value; } };
struct GainSelections
{
    Setting<unsigned> DlssNrPreset { 0 }, DlssNrStyle { 0 };
    std::optional<unsigned> DlssNrPass2Preset, DlssNrPass3Preset, DlssNrPass2Style, DlssNrPass3Style;
    struct Extra
    {
        std::optional<unsigned> style;
    };
    std::array<Extra, 27> DlssNrExtraPasses {};
};
struct SavedSettings {
    Setting<uint32_t> DlssNrTransfer {11};
};
void require(bool condition, int line)
{
    if (!condition)
        throw std::runtime_error("Superior policy check failed at line " + std::to_string(line));
}
#define require(condition) require(condition, __LINE__)
int main() {
    try {
        using namespace DlssNr::SuperiorScaling;
        SavedSettings cfg;
        require(Enabled(cfg));
        for (bool packed : { false, true })
            for (unsigned mapping = 0; mapping < 12; ++mapping)
                for (unsigned pass : { 0u, 1u })
                {
                    const int normal = NormalResolveVariant(false, packed, pass, mapping, 0, 0, 1);
                    const bool supported =
                        pass ? mapping == 0 : mapping == 0 || mapping == 2 || mapping == 4 || mapping == 6;
                    require((normal >= 0) == supported);
                    if (supported)
                    {
                        require(normal == (pass ? 0 : int(mapping / 2 + 1)) + (packed ? 5 : 0));
                        require(NormalResolveVariant(true, packed, pass, mapping, 0, 0, 1) == -1);
                        require(NormalResolveVariant(false, packed, pass, mapping, 1, 0, 1) == -1);
                        require(NormalResolveVariant(false, packed, pass, mapping, 0, 1, 1) == -1);
                        require(NormalResolveVariant(false, packed, pass, mapping, 0, 0, 0) == -1);
                    }
                }
        require(High(0)==.8f && Low(0)==.661f && High(1)==.75f && Low(1)==.787f);
        require(High(2) == .95f && Low(2) == .784f && High(99) == .950f && Low(99) == .850f);
        require(High(0,2)==.950f && Low(0,2)==.861f);
        require(High(1, 2) == .900f && Low(1, 2) == .838f);
        require(High(2,2)==.955f && Low(2,2)==.861f);
        require(MainOnlyRuntime(310, 8) && !MainOnlyRuntime(310, 9));
        require(!MainOnlyRuntime(310, 10) && !MainOnlyRuntime(311, 0) && !MainOnlyRuntime(0, 0));
        const float mainHigh[] { .8f, .75f, .95f }, mainLow[] { .661f, .787f, .784f };
        const float smallHigh[] { .950f, .900f, .955f }, smallLow[] { .861f, .838f, .861f };
        for (unsigned requested : { 0u, 1u, 2u, 3u })
            for (unsigned style = 0; style < 3; ++style)
            {
                const bool useSmallModel = requested != 1u;
                require(EffectiveNrPreset(requested, false) == (useSmallModel ? 2u : 1u));
                require(High(style, requested) == (useSmallModel ? smallHigh[style] : mainHigh[style]));
                require(Low(style, requested) == (useSmallModel ? smallLow[style] : mainLow[style]));
                // 310.8 ignores every requested preset and always runs Main.
                require(EffectiveNrPreset(requested, true) == 1u);
                require(High(style, requested, true) == mainHigh[style]);
                require(Low(style, requested, true) == mainLow[style]);
                for (float scale : { .5f, .65f, 1.0f })
                {
                    require(EffectiveBand(High(style, 0), scale, .75f, .6f) ==
                            EffectiveBand(High(style, 2), scale, .75f, .6f));
                    require(EffectiveBand(Low(style, 0), scale, .75f, .6f) ==
                            EffectiveBand(Low(style, 2), scale, .75f, .6f));
                }
            }
        for (unsigned newStyle : { 3u, 4u, 99u, UINT32_MAX })
            for (unsigned preset : { 0u, 1u, 2u, 3u, 4u, 99u, UINT32_MAX })
                for (bool mainOnly : { false, true })
                    require(High(newStyle, preset, mainOnly) == .950f && Low(newStyle, preset, mainOnly) == .850f);
        for (unsigned newPreset : { 4u, 5u, 99u, UINT32_MAX })
            for (unsigned style : { 0u, 1u, 2u })
                for (bool mainOnly : { false, true })
                    require(High(style, newPreset, mainOnly) == .950f && Low(style, newPreset, mainOnly) == .850f);
        GainSelections future;
        future.DlssNrPreset.value = 7;
        future.DlssNrStyle.value = 4;
        future.DlssNrPass2Preset = 0;
        future.DlssNrPass2Style = 1;
        future.DlssNrPass3Preset = 1;
        future.DlssNrPass3Style = 9;
        future.DlssNrExtraPasses[0].style = 2;
        future.DlssNrExtraPasses[26].style = 8;
        // Out-of-range ini values are clamped exactly like PassPreset/PassStyle, so the gains follow the running model.
        require(GainPresetSelection(future, 0) == 3 && GainStyleSelection(future, 0) == 2);
        require(GainPresetSelection(future, 1) == 0 && GainStyleSelection(future, 1) == 1);
        require(High(GainStyleSelection(future, 1), GainPresetSelection(future, 1)) == .900f);
        require(GainPresetSelection(future, 2) == 1 && GainStyleSelection(future, 2) == 2);
        require(GainStyleSelection(future, 3) == 2 && GainPresetSelection(future, 3) == 3);
        require(GainStyleSelection(future, 29) == 2 && GainStyleSelection(future, 30) == 2);
        const auto futureSchedule =
            DlssNr::InterPass::MakeGainSchedule(3, true, true, false,
                                                [&](unsigned pass) -> DlssNr::InterPass::ScheduleGains
                                                {
                                                    const auto style = GainStyleSelection(future, pass),
                                                               preset = GainPresetSelection(future, pass);
                                                    return { High(style, preset), Low(style, preset) };
                                                });
        require(std::abs(futureSchedule.Block(0, 3).high - .955f * .900f * .95f) < 1e-6f);
        require(std::abs(futureSchedule.Block(0, 3).low - (.861f + .838f + .784f) / 3) < 1e-6f);
        require(EffectiveBand(.8f,.5f,1,1)==.8f);
        require(EffectiveBand(.8f,1,1,1)==1 && EffectiveBand(.8f,.5f,.75f,0)==1);
        require(std::abs(EffectiveBand(.8f,.5f,.5f,.5f)-.95f)<1e-6f);
        // Mixed NR models: each pass uses its own table, then block high is a product
        // and low an arithmetic mean. SR A/K never participates in this calculation.
        const auto mixed = DlssNr::InterPass::MakeGainSchedule(3, true, true, false,
            [](unsigned pass) -> DlssNr::InterPass::ScheduleGains {
                const unsigned style = pass == 2 ? 2u : 1u;
                const unsigned model = pass == 1 ? 2u : 1u;
                return { EffectiveBand(High(style,model),.5f,1,1),
                         EffectiveBand(Low(style,model),.5f,1,1) };
            });
        require(std::abs(mixed.Block(0, 2).high - .75f * .900f) < 1e-6f);
        require(std::abs(mixed.Block(0,2).low - (.787f+.838f)/2)<1e-6f);
        require(mixed.expectedMask == 2u); // .900 Small Natural crosses .15 before the third mixed-model pass
        const auto mainNatural = DlssNr::InterPass::MakeGainSchedule(3, true, true, false,
            [](unsigned) -> DlssNr::InterPass::ScheduleGains { return { High(1),Low(1) }; });
        require(mainNatural.expectedMask == 3u); // .787 - .75*.75 > .15 before either next pass
        require(mainNatural.Block(2,3).low == Low(1)); // final low covers only the tail
        const auto defaultNatural =
            DlssNr::InterPass::MakeGainSchedule(3, true, true, false,
                                                [](unsigned) -> DlssNr::InterPass::ScheduleGains {
                                                    return { High(1, 0), Low(1, 0) };
                                                });
        require(defaultNatural.expectedMask == 0u); // 310.9 Default runs Small, not Main
        require(std::abs(defaultNatural.Block(0, 3).high - .900f * .900f * .900f) < 1e-6f);
        require(std::abs(defaultNatural.Block(0, 3).low - .838f) < 1e-6f);
        const auto oldDefaultNatural =
            DlssNr::InterPass::MakeGainSchedule(3, true, true, false,
                                                [](unsigned) -> DlssNr::InterPass::ScheduleGains {
                                                    return { High(1, 0, true), Low(1, 0, true) };
                                                });
        require(oldDefaultNatural.expectedMask == mainNatural.expectedMask);
        require(WorkingExtent(3840,.65f)==2496 && WorkingExtent(2160,.65f)==1404);
        require(WorkingExtent(3840,.59f)==2266 && WorkingExtent(2160,.59f)==1274);
        require(WorkingExtent(1919,1.0f)==1919 && WorkingExtent(1079,1.0f)==1079);
        require(WorkingExtent(1919,.65f)==1247 && WorkingExtent(1079,.65f)==701);
        {
            // Auto scale hysteresis: the exact raster only moves with the ordinary 16-pixel cell or the frame size.
            ScaleHold hold;
            require(hold.Hold(3840, 2160, 2496, 1408, .650f) == .650f);
            require(hold.Hold(3840, 2160, 2496, 1408, .652f) == .650f); // same cells: no new raster
            require(hold.Hold(3840, 2160, 2512, 1408, .654f) == .654f); // width cell moved
            require(hold.Hold(3840, 2160, 2512, 1424, .656f) == .656f); // height cell moved
            require(hold.Hold(2560, 1440, 2512, 1424, .657f) == .657f); // frame size changed
            require(WorkingExtent(3840, .650f) != WorkingExtent(3840, .652f)); // what the hold saves a rebuild for
        }
        require(MotionScale(1920,1920,1920)==1920); // native NR, game uses render-size vectors
        require(MotionScale(1920,2496,1920)==2496); // P65 of 3840, matched guides
        require(MotionScale(3840,2496,3840)==2496); // high-resolution motion texture
        require(MotionScale(-1,1274,1440)==-1*(1274.f/1440.f));
        require(MotionScale(1, 1274, 0, 1440) ==
                1274.f / 1440.f); // missing declared reference uses valid source, not allocation/depth
        require(MotionScale(1920, 2496, 0, 1920) == 2496);
        require(MotionScale(1,1274,0)==1); // unspecified game reference
        for (unsigned i = 0; i < 30; ++i)
            require(Preset(i) == (i == 0 || i == 11 ? i : 1u));
        require(Preset(UINT32_MAX)==1u);
        require(std::abs(GuidedRange(.25f) - .042f) < 1e-6f);
        require(std::abs(GuidedRange(.5f) - .042f) < 1e-6f);
        require(std::abs(GuidedRange(.75f) - .039f) < 1e-6f);
        require(std::abs(GuidedRange(1.f) - .036f) < 1e-6f);
        require(GuidedRange(1.5f) == GuidedRange(1.f));
        require(std::abs(GuidedRange(NAN) - .042f) < 1e-6f);
        float previousSigma = .042f;
        for (unsigned percent = 25; percent <= 200; ++percent)
        {
            const float scale = percent / 100.f;
            const float sigma = GuidedRange(scale);
            require(sigma <= previousSigma + 1e-7f && sigma > 0.f);
            previousSigma = sigma;
            require(PrivatePreset(1, scale, false) == (scale >= .85f ? CnnE : CnnD));
            require(PrivatePreset(1, scale, true) == CnnE);
            require(PrivatePreset(11, scale, false) == TransformerK);
            require(PrivatePreset(11, scale, true) == TransformerK);
            for (unsigned mode : { 0u, 1u, 2u, 11u })
                require(CanSelectUpscaleMode(mode, scale) == (mode == 11u || mode == 0u || percent < 100));
            DlssNrConstants resolve {};
            SetGuidedRange(resolve, scale);
            float inverse;
            std::memcpy(&inverse, &resolve.ExposureSourceWidth, 4);
            require(inverse == 1.f / (sigma * sigma));
        }
        require(PrivatePreset(1, std::nextafter(.85f, 0.f), false) == CnnD && PrivatePreset(1, .85f, false) == CnnE);
        require(PrivatePreset(1, .45f, true) == CnnE);
        require(PrivatePreset(0, 1.f, false) == NoPrivateDlss && PrivatePreset(0, .5f, true) == NoPrivateDlss);
        // Direct DLSS SR policy coverage: full-native PerfQuality mapping, Performance mode preset/ghost-skip, Halton jitter.
        {
            require(DirectSrPerfQuality(1536, 864, 3413, 1920) == 0);   // MaxPerf (0.45)
            require(DirectSrPerfQuality(1280, 720, 1920, 1080) == 2);   // MaxQuality (0.667)
            require(DirectSrPerfQuality(3413, 1920, 3413, 1920) == 5);  // DLAA
            require(DirectSrPerfQuality(1000, 562, 1920, 1080) == 0);   // MaxPerf (~0.52)
            require(DirectSrPerfQuality(1100, 618, 1920, 1080) == 1);   // Balanced (~0.57)
            require(DirectSrPerfQuality(640, 360, 1920, 1080) == 3);    // UltraPerformance (0.33)

            require(PrivatePreset(TransformerK, .45f, true, false) == TransformerK);
            require(PrivatePreset(TransformerK, .45f, true, true) == CnnD); // Performance mode forces fast CNN D
            require(PrivatePreset(CnnPreference, .65f, true, true) == CnnD);
            require(NeedsGhostField(TransformerK, false));
            require(!NeedsGhostField(TransformerK, true)); // Performance mode skips ghost field
            require(!NeedsGhostField(CnnD, false) && !NeedsGhostField(CnnE, true));

            const auto jOff = DirectSrJitter(false, false, false, 2496, 1404, 3840, 2160, 1);
            require(jOff.x == 0.f && jOff.y == 0.f);
            const auto jBefore = DirectSrJitter(true, true, false, 2496, 1404, 3840, 2160, 1);
            require(jBefore.x == 0.f && jBefore.y == 0.f);
            const auto jHold = DirectSrJitter(true, false, true, 2496, 1404, 3840, 2160, 1);
            require(jHold.x == 0.f && jHold.y == 0.f);
            const auto jNative = DirectSrJitter(true, false, false, 3840, 2160, 3840, 2160, 1);
            require(jNative.x == 0.f && jNative.y == 0.f);
            // Scale-adaptive Halton (2, 3) jitter: 0 strictly below 40% (< 0.399), smooth ramp from 0.35 at 40% to 1.0 at >=65%.
            bool anyNonZeroFull = false;
            bool anyNonZero45 = false;
            bool anyNonZero40 = false;
            for (uint64_t f = 0; f < 16; ++f)
            {
                const auto jLow = DirectSrJitter(true, false, false, 1498, 842, 3840, 2160, f);   // 39% (< 0.399)
                require(jLow.x == 0.f && jLow.y == 0.f);

                const auto j40 = DirectSrJitter(true, false, false, 1365, 768, 3413, 1920, f);    // exact 40% (1365/3413, amp ~0.35)
                const auto j45 = DirectSrJitter(true, false, false, 1536, 864, 3413, 1920, f);    // 45% (1536/3413, amp ~0.48)
                const auto jFull = DirectSrJitter(true, false, false, 2496, 1404, 3840, 2160, f); // 65% exact (amp 1.0)
                const auto jHigh = DirectSrJitter(true, false, false, 2688, 1512, 3840, 2160, f); // 70% (> 0.65, amp 1.0)
                const auto jWrap = DirectSrJitter(true, false, false, 2496, 1404, 3840, 2160, f + 16);
                require(jFull.x >= -0.5f && jFull.x < 0.5f && jFull.y >= -0.5f && jFull.y < 0.5f);
                require(jFull.x == jWrap.x && jFull.y == jWrap.y);
                require(jFull.x == jHigh.x && jFull.y == jHigh.y);
                require(std::abs(j40.x - jFull.x * 0.35f) < 1e-3f && std::abs(j40.y - jFull.y * 0.35f) < 1e-3f);
                const float amp45 = 0.35f + 0.65f * ((1536.f / 3413.f - 0.40f) / 0.25f);
                require(std::abs(j45.x - jFull.x * amp45) < 1e-4f && std::abs(j45.y - jFull.y * amp45) < 1e-4f);
                if (jFull.x != 0.f || jFull.y != 0.f)
                    anyNonZeroFull = true;
                if (j45.x != 0.f || j45.y != 0.f)
                    anyNonZero45 = true;
                if (j40.x != 0.f || j40.y != 0.f)
                    anyNonZero40 = true;
            }
            require(anyNonZeroFull && anyNonZero45 && anyNonZero40);

            // Superior mode selectors: ModeDirectSr, ModeResidualDlaa, ModeIntrinsicOracle are mutually exclusive.
            require(DirectSrSelected(ModeDirectSr) && !IntrinsicOracleSelected(ModeDirectSr));
            require(!DirectSrSelected(ModeResidualDlaa) && !IntrinsicOracleSelected(ModeResidualDlaa));
            require(IntrinsicOracleSelected(ModeIntrinsicOracle) && !DirectSrSelected(ModeIntrinsicOracle));
        }
        PrivatePresetChoice latch;
        require(latch.Select(CnnPreference, true, .65f, .65f, false) == CnnD);
        require(latch.Select(CnnPreference, true, .65f, .98f, false) == CnnD);
        require(latch.Select(CnnPreference, true, .65f, .65f, true) == CnnE); // switching to Direct SR selects CnnE
        require(latch.Select(TransformerK, true, .65f, .98f, false) == TransformerK);
        require(latch.Select(CnnPreference, true, .65f, .98f, false) == CnnE);
        require(latch.Select(NoPrivateDlss, true, .65f, .98f, false) == NoPrivateDlss);
        latch = {};
        require(latch.Select(CnnPreference, true, .65f, .65f, false) == CnnD);

        // Still below 288 / 576 moving samples, moving from 432; in between the last reading stands. A reading has to
        // hold 50 ms before the preset follows it (both ways), then a 0.3 s fade; only a switch resets the next one.
        using DlssNr::SuperiorPrivate::CnnMotion;
        {
            CnnMotion motion;
            motion.Observe(287, 1.0);
            require(!motion.Advance(1.0));
            motion.Observe(287, 1.04);
            require(!motion.Advance(1.04) && motion.fade.weight == 1.f);
            motion.Observe(287, 1.06);
            require(!motion.Advance(1.06) && motion.fade.weight == 1.f);
            require(!motion.Advance(1.2) && !motion.idle && motion.fade.weight < 1.f);
            require(!motion.Advance(1.35) && !motion.idle);
            require(motion.Advance(1.36) && motion.idle && motion.fade.weight == 1.f);
            require(!motion.Advance(1.37));
            // A single moving reading while on E does not switch back (R2-F4: symmetric dwell).
            motion.Observe(432, 1.39);
            require(!motion.Advance(1.39) && !motion.Advance(1.43) && motion.fade.weight == 1.f);
            motion.Observe(100, 1.42);
            require(!motion.Advance(1.6) && motion.idle && motion.fade.weight == 1.f);
            // Two moving readings 50 ms apart do: fade from then, switch 0.3 s later.
            motion.Observe(432, 1.62);
            motion.Observe(432, 1.68);
            require(!motion.Advance(1.68) && motion.fade.weight == 1.f);
            require(!motion.Advance(1.9) && motion.idle && motion.fade.weight < 1.f);
            require(motion.Advance(1.98) && !motion.idle);
            motion.Invalidate();
            require(!motion.Advance(2.0) && motion.fade.weight == 1.f);
        }
        {
            // Hysteresis: from still, counts between 288 and 431 keep it still; from moving, they keep it moving.
            CnnMotion motion;
            motion.Observe(431, 1.0); // no earlier reading: in-between counts as moving
            require(!motion.quiet);
            motion.Observe(100, 1.05);
            motion.Observe(431, 1.08);
            require(motion.quiet);
            motion.Observe(432, 1.1);
            motion.Observe(300, 1.12);
            require(!motion.quiet);
        }
        {
            // Time going backwards or standing still is ignored; a gap over 0.5 s restarts the dwell.
            CnnMotion motion;
            motion.Observe(0, 1.0);
            motion.Observe(0, 0.9);
            require(motion.observed == 1.0);
            motion.Observe(0, 1.0);
            require(motion.observed == 1.0 && motion.since == 1.0);
            motion.Observe(0, 1.7);
            require(motion.since == 1.7);
        }
        {
            // A stale reading (over 0.5 s old) drops E back to D through the fade.
            CnnMotion motion;
            motion.Observe(0, 1.0);
            motion.Observe(0, 1.06);
            require(!motion.Advance(1.06));
            require(motion.Advance(1.4) && motion.idle);
            require(!motion.Advance(1.6) && motion.idle && motion.fade.weight == 1.f); // still fresh at 1.56 s
            require(!motion.Advance(1.7) && motion.fade.weight < 1.f);
            require(motion.Advance(2.0) && !motion.idle);
        }
        {
            // Fade on its own: reversal mid-fade climbs back without a switch; a long gap jumps the whole way.
            DlssNr::SuperiorPrivate::Fade fade;
            require(fade.Advance(false, 1.0) == 1.f); // the starting frame takes no time
            require(fade.Advance(false, 1.15) < 1.f && fade.weight > 0.f);
            require(fade.Advance(true, 1.2) > .5f && fade.Advance(true, 1.6) == 1.f);
            DlssNr::SuperiorPrivate::Fade gap;
            gap.Advance(false, 1.0);
            gap.Advance(false, 1.01);
            require(gap.Advance(false, 5.0) == 0.f);
        }
        // Resolve PSO slots: every combination lands in the 9-slot table, guided raw always in 6..8.
        for (bool raw : { false, true })
            for (bool guided : { false, true })
                for (bool packed : { false, true })
                    for (bool cached : { false, true })
                        for (bool sdr : { false, true })
                        {
                            const unsigned i = ResolveIndex(raw, guided, packed, cached, sdr);
                            require(i < 9u);
                            require(!guided || (i == (packed ? (cached ? 8u : 7u) : 6u)));
                        }
        require(ResolveIndex(true, false, false, false, false) == 2u && ResolveIndex(true, false, false, false, true) == 2u);
        require(ResolveIndex(false, false, true, false, true) == 5u && ResolveIndex(false, false, false, false, false) == 0u);
        // Frame-rate independence: 0.1 px per 60 Hz frame whatever the frame time; nonsense frame times count as 60 Hz.
        {
            using DlssNr::SuperiorPrivate::PerSecondScale;
            require(PerSecondScale(1000.f / 60.f) == 1.f);
            require(std::abs(PerSecondScale(1000.f / 240.f) - 4.f) < 1e-4f);
            require(std::abs(PerSecondScale(1000.f / 30.f) - .5f) < 1e-4f);
            require(PerSecondScale(0.f) == 1.f && PerSecondScale(-5.f) == 1.f && PerSecondScale(NAN) == 1.f);
            require(PerSecondScale(1e-3f) == 10.f && PerSecondScale(1e5f) == .1f);
        }
        {
            // Retire wait: the full bound, then give up once; nothing pending resets it; a later wait gets the full
            // bound again.
            using namespace DlssNr::SuperiorPrivate;
            using Step = RetireWait::Step;
            RetireWait wait;
            require(wait.Next(false) == Step::Go);
            for (unsigned i = 0; i < RetireWaitFrames; ++i)
                require(wait.Next(true) == Step::Wait);
            require(wait.Next(true) == Step::GiveUp);
            require(wait.Next(true) == Step::Wait); // only reached if the caller kept the stuck ones: a fresh bound
            for (unsigned i = 0; i < 10; ++i)
                require(wait.Next(true) == Step::Wait);
            require(wait.Next(false) == Step::Go);
            for (unsigned i = 0; i < RetireWaitFrames; ++i)
                require(wait.Next(true) == Step::Wait);
            require(wait.Next(true) == Step::GiveUp);

            // Hook retry: first attempt at once; throttled while it fails; at once again after the hook came off.
            HookRetry hook;
            require(hook.Due(false));
            for (unsigned i = 0; i < HookRetryFrames; ++i)
                require(!hook.Due(false));
            require(hook.Due(false));
            require(!hook.Due(true) && !hook.Due(true));
            require(hook.Due(false)); // came off: no throttle
            require(!hook.Due(false));

            // Idle: only after IdleAfterCalls evaluates without a Superior frame, then on every one until it runs.
            IdleWatch idle;
            for (unsigned i = 1; i < IdleAfterCalls; ++i)
                require(!idle.Due());
            require(idle.Due() && idle.Due());
            idle.Ran();
            require(!idle.Due());

            // Creation: runtime/feature missing or too old latches for the session; the rest is per generation.
            require(ClassifyCreate(1u, true) == CreateVerdict::Ok);
            require(ClassifyCreate(1u, false) == CreateVerdict::Generation);
            for (unsigned r : { NgxFeatureNotSupported, NgxPlatformError, NgxFeatureNotFound, NgxOutOfDate })
                require(ClassifyCreate(r, false) == CreateVerdict::Session);
            for (unsigned r : { NgxFail, NgxFail | 5u /* InvalidParameter */, NgxFail | 13u /* OutOfGPUMemory */ })
                require(ClassifyCreate(r, false) == CreateVerdict::Generation);
        }

        // Native-scale fallback is tested by the host integration contract.
        DlssNrConstants input{};input.Mode=0;input.Width=1920;input.Height=1080;input.DirectResolveFlags=ShaderFlag;
        for(uint32_t pass : {0u,1u})for(uint32_t mode=0;mode<5;++mode) {
            input.Passthrough=pass;input.ReversibleMode=mode;require(InputEncode(input,1920,1080));
        }
        input.Mode=1;require(!InputEncode(input,1920,1080));input.Mode=2;require(!InputEncode(input,1920,1080));input.Mode=0;
        input.ReversibleMode=5;require(!InputEncode(input,1920,1080));input.ReversibleMode=0;
        input.Passthrough=2;require(!InputEncode(input,1920,1080));input.Passthrough=0;
        input.DirectResolveFlags=0;require(!InputEncode(input,1920,1080));input.DirectResolveFlags=ShaderFlag|4;
        require(!InputEncode(input,1920,1080));input.DirectResolveFlags=ShaderFlag;
        require(!InputEncode(input,1921,1080) && !InputEncode(input,1920,1081));
        require(!InputEncode(input,1919,1080) && !InputEncode(input,1920,1079));
        input.Width=0;require(!InputEncode(input,0,1080));input.Width=1920;
        input.Height=0;require(!InputEncode(input,1920,0));input.Width=input.Height=1;require(InputEncode(input,1,1));
        DlssNrConstants exposure{};exposure.Mode=3;exposure.Width=exposure.Height=64;
        exposure.ExposureSourceWidth=3840;exposure.ExposureSourceHeight=2160;exposure.DirectResolveFlags=ShaderFlag;
        require(ExposureStage(exposure,3840,2160)==1);
        require(ExposureStage(exposure,3841,2160)==0 && ExposureStage(exposure,3840,2161)==0);
        exposure.Width=63;require(ExposureStage(exposure,3840,2160)==0);exposure.Width=64;
        exposure.Mode=11;exposure.Width=exposure.Height=1;require(ExposureStage(exposure,64,64)==2);
        require(ExposureStage(exposure,63,64)==0 && ExposureStage(exposure,64,63)==0);
        exposure.Width=2;require(ExposureStage(exposure,64,64)==0);exposure.Width=1;
        exposure.Mode=0;require(ExposureStage(exposure,64,64)==0);exposure.Mode=11;
        exposure.ExposureSourceWidth=0;require(ExposureStage(exposure,64,64)==0);exposure.ExposureSourceWidth=3840;
        exposure.ExposureSourceHeight=0;require(ExposureStage(exposure,64,64)==0);exposure.ExposureSourceHeight=2160;
        exposure.DirectResolveFlags=0;require(ExposureStage(exposure,64,64)==0);exposure.DirectResolveFlags=ShaderFlag|4;
        require(ExposureStage(exposure,64,64)==0);
        DlssNrConstants area{};area.Mode=2;area.Width=2496;area.Height=1404;area.DirectResolveFlags=ShaderFlag;
        require(AreaDownsample(area,3840,2160) && AreaDownsample(area,2496,2160) && AreaDownsample(area,3840,1404));
        require(!AreaDownsample(area,2496,1404) && !AreaDownsample(area,1920,1080));
        require(!AreaDownsample(area,0,2160) && !AreaDownsample(area,3840,0));
        area.Transfer=1;require(!AreaDownsample(area,3840,2160));area.Transfer=0;
        area.Mode=32;require(!AreaDownsample(area,3840,2160));area.Mode=2;
        area.DirectResolveFlags=0;require(!AreaDownsample(area,3840,2160));area.DirectResolveFlags=ShaderFlag|4;
        require(!AreaDownsample(area,3840,2160));area.DirectResolveFlags=ShaderFlag;
        area.Width=0;require(!AreaDownsample(area,3840,2160));area.Width=1;area.Height=0;
        require(!AreaDownsample(area,3840,2160));area.Height=1;require(AreaDownsample(area,1,2));
        DlssNrConstants low{};low.Mode=18;low.Transfer=1;low.Width=312;low.Height=176;low.DirectResolveFlags=ShaderFlag;
        require(LowResidual(low,2496,1404));
        low.Mode=34;require(!LowResidual(low,2496,1404));low.Mode=18;
        low.Transfer=0;require(!LowResidual(low,2496,1404));low.Transfer=1;
        low.Transfer=9;require(!LowResidual(low,2496,1404));low.Transfer=1;
        low.DirectResolveFlags=0;require(!LowResidual(low,2496,1404));low.DirectResolveFlags=ShaderFlag|4;
        require(!LowResidual(low,2496,1404));low.DirectResolveFlags=ShaderFlag;
        require(!LowResidual(low,0,1404) && !LowResidual(low,2496,0));
        low.Width=313;require(LowResidual(low,2497,1404) && !LowResidual(low,2496,1404));
        low.Width=312;require(LowResidual(low,2496,1404)); // 2496 is exactly divisible by eight.
        low.Height=175;require(!LowResidual(low,2496,1404));low.Height=176;
        low.Width=311;require(!LowResidual(low,2496,1404));low.Width=0;require(!LowResidual(low,2496,1404));
        low.Width=low.Height=1;require(LowResidual(low,1,1) && LowResidual(low,8,8) && !LowResidual(low,9,8));
        DlssNr::GuideRegion region {3,5,2560,1440};
        require(ReuseDepth(true,true,1920,1080,1920,1080,region,region));
        require(!ReuseDepth(false,true,1920,1080,1920,1080,region,region));
        require(!ReuseDepth(true,false,1920,1080,1920,1080,region,region));
        require(!ReuseDepth(true,true,1921,1080,1920,1080,region,region));
        require(!ReuseDepth(true,true,1920,1081,1920,1080,region,region));
        require(!ReuseDepth(true,true,0,1080,0,1080,region,region));
        require(!ReuseDepth(true,true,1920,0,1920,0,region,region));
        for(auto field : {&DlssNr::GuideRegion::x,&DlssNr::GuideRegion::y,
                          &DlssNr::GuideRegion::width,&DlssNr::GuideRegion::height}) {
            auto changed=region;++(changed.*field);
            require(!ReuseDepth(true,true,1920,1080,1920,1080,changed,region));
        }
        region.width=0;require(!ReuseDepth(true,true,1920,1080,1920,1080,region,region));
        region.width=2560;region.height=0;
        require(!ReuseDepth(true,true,1920,1080,1920,1080,region,region));
        DlssNrConstants resolve{};resolve.Mode=1;resolve.Transfer=9;resolve.Width=3840;resolve.Height=2160;
        resolve.ResidualHistoryValid=1;resolve.ApplyModel=1;resolve.DirectResolveFlags=ShaderFlag;
        require(NormalResolve(resolve,1920,1080) && NormalResolve(resolve,3840,1080) && NormalResolve(resolve,1920,2160));
        require(!NormalResolve(resolve,3840,2160) && !NormalResolve(resolve,7680,4320));
        require(!NormalResolve(resolve,0,1080) && !NormalResolve(resolve,1920,0) && !NormalResolve(resolve,7680,1080));
        for(auto field : {&DlssNrConstants::DebugView,&DlssNrConstants::CompareMode,&DlssNrConstants::ShowSkinMask}) {
            resolve.*field=1;require(!NormalResolve(resolve,1920,1080));resolve.*field=0;
        }
        resolve.ApplyModel=0;require(!NormalResolve(resolve,1920,1080));resolve.ApplyModel=1;
        resolve.Transfer=8;require(!NormalResolve(resolve,1920,1080));resolve.Transfer=9;
        resolve.Mode=18;require(!NormalResolve(resolve,1920,1080));resolve.Mode=1;
        resolve.ResidualHistoryValid=2;require(!NormalResolve(resolve,1920,1080));resolve.ResidualHistoryValid=1;
        resolve.DirectResolveFlags|=4;require(!NormalResolve(resolve,1920,1080));resolve.DirectResolveFlags=ShaderFlag;
        for(uint32_t mode=0;mode<12;++mode) {
            resolve.ReversibleMode=mode;
            for(uint32_t pass : {0u,1u,2u}) {
                resolve.Passthrough=pass;require(SoftKneeResolve(resolve)==(pass==0 && mode==0));
            }
            resolve.ReversibleMode=mode;resolve.Passthrough=1;require(NormalColorProfile(resolve)==(mode<=4?1:0));
            resolve.Passthrough=0;require(NormalColorProfile(resolve)==(mode==2?2:mode==4?3:0));
            resolve.Passthrough=2;require(NormalColorProfile(resolve)==0);
        }
        for(uint32_t mode : {2u,4u}) {
            resolve.ReversibleMode=mode;resolve.Passthrough=0;resolve.ReplaceDetailStrength=.5f;
            resolve.ModelWorkScale=.5f;require(NormalColorProfile(resolve)==0);
            resolve.Passthrough=1;require(NormalColorProfile(resolve)==0);
            resolve.ModelWorkScale=.999f;require(NormalColorProfile(resolve)==1);
            resolve.ModelWorkScale=1;require(NormalColorProfile(resolve)==1);
            resolve.ModelWorkScale=0;require(NormalColorProfile(resolve)==1);
            resolve.ModelWorkScale=.5f;resolve.ReplaceDetailStrength=0;require(NormalColorProfile(resolve)==1);
        }
        resolve.Width=resolve.Height=1;require(!NormalResolve(resolve,1,1)); // A rounded 1x1 is not reduced.
        for(uint32_t mode=0;mode<=10;++mode) {
            cfg.DlssNrTransfer.value=mode;
            require(!Enabled(cfg));
        }
        std::cout << "Superior policy PASS: Main/Small/Default/legacy presets on 310.8 and 310.9, mixed gain blocks "
                     "and schedules, No DLSS / CNN D-E / Transformer K, guides and PSO guards\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
