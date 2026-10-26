// Host test for the multi-view detector (OptiScaler/shaders/dlssnr/DlssNr_Views.h). No D3D12, no game.
// Build: cl /nologo /std:c++20 /EHsc tests\nr_views_smoke.cpp /Fe:nr_views_smoke.exe
#include <cstdio>
#include "../OptiScaler/shaders/dlssnr/DlssNr_Views.h"

using namespace DlssNr::Views;

static int g_fail = 0;
static void Check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    g_fail += ok ? 0 : 1;
}

// A triple-screen rig: one 5120x960 colour allocation, three screens side by side and the combined feature.
static Seen Screen(int i)
{
    const unsigned int x[] = { 0, 1706, 3413 };
    const unsigned int w[] = { 1706, 1707, 1707 };
    return { 5120, 960, 28, { x[i], 0, w[i], 960 } };
}
static Seen Whole() { return { 5120, 960, 28, { 0, 0, 5120, 960 } }; }
static Seen Single(unsigned int w, unsigned int h) { return { 2560, 1440, 28, { 0, 0, w, h } }; }

constexpr uint32_t kLeft = 1u << 0, kMiddle = 1u << 1, kRight = 1u << 2, kWhole = 1u << kWholeBit;

struct FrameResult
{
    Detector::Result screen[3];
    Detector::Result whole;
};

// One frame: the three screens (each evaluate twice: the before-SR and the after-SR seam), the combined feature if asked.
static FrameResult Frame(Detector& d, unsigned long long epoch, uint32_t mask, bool withWhole = false,
                         bool screensFirst = true)
{
    FrameResult r;
    if (withWhole && !screensFirst)
    {
        d.Note(epoch, Whole(), mask);
        r.whole = d.Note(epoch, Whole(), mask);
    }
    for (int i = 0; i < 3; ++i)
    {
        d.Note(epoch, Screen(i), mask);
        r.screen[i] = d.Note(epoch, Screen(i), mask);
    }
    if (withWhole && screensFirst)
    {
        d.Note(epoch, Whole(), mask);
        r.whole = d.Note(epoch, Whole(), mask);
    }
    return r;
}

int main()
{
    std::printf("A single view is left alone:\n");
    {
        Detector d;
        bool allRun = true, mainSlot = true;
        for (unsigned long long e = 1; e <= 200; ++e)
        {
            // Dynamic resolution moves the one rectangle from frame to frame.
            const auto r = d.Note(e, Single(1700 + (unsigned int) (e % 7) * 10, 950), 0);
            allRun = allRun && r.verdict == Verdict::Run;
            mainSlot = mainSlot && r.slot == -1;
        }
        Check(allRun, "every evaluate runs");
        Check(mainSlot, "on the single view's own state (slot -1)");
        Check(!d.Status().active, "never latches (drifting size is not several views)");
    }

    std::printf("Two sizes in two different textures are not the atlas layout:\n");
    {
        Detector d;
        const Seen main = Single(1706, 960);
        const Seen mirror = { 640, 360, 28, { 0, 0, 640, 360 } };
        bool run = true;
        for (unsigned long long e = 1; e <= 50; ++e)
        {
            run = run && d.Note(e, main, 0).verdict == Verdict::Run;
            run = run && d.Note(e, mirror, 0).verdict == Verdict::Run;
        }
        Check(run && !d.Status().active, "a mirror or portrait texture does not stand NR down");
    }

    std::printf("Three screens, nothing ticked:\n");
    {
        Detector d;
        auto f = Frame(d, 1, 0);
        Check(d.Status().active, "found within the first frame");
        Check(f.screen[1].verdict == Verdict::StandDown && f.screen[2].verdict == Verdict::StandDown,
              "the later screens stand down");
        f = Frame(d, 2, 0);
        Check(f.screen[0].verdict == Verdict::StandDown && f.screen[1].verdict == Verdict::StandDown &&
                  f.screen[2].verdict == Verdict::StandDown,
              "from the next frame on, all of them");
        const auto s = d.Status();
        Check(s.count == 3 && s.screens == 3, "three screens listed");
        Check(s.views[0].rect.x == 0 && s.views[1].rect.x == 1706 && s.views[2].rect.x == 3413, "left to right");
        Check(s.views[0].slot == 0 && s.views[1].slot == 1 && s.views[2].slot == 2, "numbered 1, 2, 3 (slots 0, 1, 2)");
        Check(s.skippedEvaluates > 0, "the skipped evaluates are counted");
    }

    std::printf("All three screens ticked: each runs on its own slot:\n");
    {
        Detector d;
        Frame(d, 1, kLeft | kMiddle | kRight);
        const auto f = Frame(d, 2, kLeft | kMiddle | kRight);
        bool run = true;
        for (int i = 0; i < 3; ++i)
            run = run && f.screen[i].verdict == Verdict::Run && f.screen[i].slot == i;
        Check(run, "screen i runs on slot i");
        Check(f.screen[0].needsBeforeSr && f.screen[2].needsBeforeSr, "one screen of a shared output needs before SR");
        Check(!f.screen[0].offOrigin && f.screen[1].offOrigin && f.screen[2].offOrigin, "off the origin: screens 2, 3");
    }

    std::printf("Middle and right only:\n");
    {
        Detector d;
        Frame(d, 1, kMiddle | kRight);
        const auto f = Frame(d, 2, kMiddle | kRight);
        Check(f.screen[0].verdict == Verdict::OtherView, "the left screen stays as the game made it");
        Check(f.screen[1].verdict == Verdict::Run && f.screen[2].verdict == Verdict::Run, "the middle and right run");
        const auto s = d.Status();
        Check(!s.views[0].runs && s.views[1].runs && s.views[2].runs, "the menu shows which run");
    }

    std::printf("The whole picture:\n");
    {
        Detector d;
        Frame(d, 1, kWhole, true);
        const auto f = Frame(d, 2, kWhole, true);
        Check(f.whole.verdict == Verdict::Run && f.whole.slot == (int) kWholeBit, "the combined feature runs on its slot");
        Check(!f.whole.needsBeforeSr, "the whole picture may run after SR too");
        Check(f.screen[0].verdict == Verdict::OtherView && f.screen[1].verdict == Verdict::OtherView, "the screens do not");
        const auto s = d.Status();
        Check(s.screens == 3 && s.count == 4 && s.views[3].slot == kWholeBit, "listed after the screens");
    }

    std::printf("The screens keep their numbers whether or not the whole picture is drawn too:\n");
    {
        Detector d;
        Frame(d, 1, kRight, true, false);
        const auto withWhole = Frame(d, 2, kRight, true, false);
        Check(withWhole.screen[2].verdict == Verdict::Run && withWhole.screen[2].slot == 2, "right = slot 2 with the whole");
        // Some stretches evaluate only the screens.
        const auto without = Frame(d, 3, kRight);
        Check(without.screen[2].verdict == Verdict::Run && without.screen[2].slot == 2, "right = slot 2 without it");
    }

    std::printf("The layout goes away only after a long quiet time:\n");
    {
        Detector d;
        Frame(d, 1, 0);
        bool stood = true;
        for (unsigned long long e = 2; e < 2 + kQuietEpochs - 2; ++e)
            stood = stood && d.Note(e, Screen(0), 0).verdict == Verdict::StandDown;
        Check(stood && d.Status().active, "one view per frame for a while (a menu, a loading screen) does not release it");
        bool released = false;
        for (unsigned long long e = 2 + kQuietEpochs - 2; e < 2 + kQuietEpochs + 5; ++e)
            released = released || d.Note(e, Screen(0), 0).unlatchedNow;
        Check(released && !d.Status().active, "released after kQuietEpochs frames with one view");
        const auto r = d.Note(1000, Screen(0), 0);
        Check(r.verdict == Verdict::Run && r.slot == -1, "the single view runs again on its own state");
    }

    std::printf("An outside camera (the whole picture alone) releases the layout quickly:\n");
    {
        Detector d;
        Frame(d, 1, kLeft | kMiddle | kRight);
        Frame(d, 2, kLeft | kMiddle | kRight);
        bool released = false;
        unsigned long long e = 3;
        for (; e < 3 + kWholeOnlyEpochs + 2 && !released; ++e)
        {
            const auto r = d.Note(e, Whole(), kLeft | kMiddle | kRight);
            released = r.unlatchedNow && r.quickRelease;
        }
        Check(released && !d.Status().active, "released within kWholeOnlyEpochs frames");
        const auto r = d.Note(e, Whole(), kLeft | kMiddle | kRight);
        Check(r.verdict == Verdict::Run && r.slot == -1, "the wide view runs on the single view's state");

        // Back to the cockpit: the left screen alone is recognised as the layout seen before, on its first evaluate.
        const auto first = d.Note(e + 1, Screen(0), kLeft | kMiddle | kRight);
        Check(first.latchedNow && first.recalled, "the first screen back brings the layout back");
        Check(first.verdict == Verdict::Run && first.slot == 0, "and runs on its own slot at once");
        const auto right = d.Note(e + 1, Screen(2), kLeft | kMiddle | kRight);
        Check(right.slot == 2 && !right.latchedNow, "the other screens keep their numbers");

        // Out again and back again: every switch is quick.
        bool releasedAgain = false;
        for (unsigned long long k = e + 2; k < e + 2 + kWholeOnlyEpochs + 2 && !releasedAgain; ++k)
            releasedAgain = d.Note(k, Whole(), kLeft | kMiddle | kRight).quickRelease;
        Check(releasedAgain, "the next outside camera is quick too");
    }

    std::printf("A single-view game is never recalled into a layout:\n");
    {
        Detector d;
        bool never = true;
        for (unsigned long long e = 1; e < 50; ++e)
            never = never && !d.Note(e, Screen(0), kLeft).latchedNow;
        Check(never, "nothing remembered, nothing recalled");
    }

    std::printf("Cockpit frames where a present falls between the screens do not count as the whole picture alone:\n");
    {
        Detector d;
        Frame(d, 1, kLeft);
        bool kept = true;
        for (unsigned long long e = 2; e < 2 + kWholeOnlyEpochs * 3; ++e)
        {
            kept = kept && !d.Note(e * 2, Screen(0), kLeft).unlatchedNow;
            kept = kept && !d.Note(e * 2 + 1, Screen(1), kLeft).unlatchedNow;
        }
        Check(kept && d.Status().active, "one screen per present keeps the layout");
    }

    std::printf("A resolution change leaves no stale view:\n");
    {
        Detector d;
        Frame(d, 1, kLeft | kRight);
        for (unsigned long long e = 100; e < 110; ++e)
        {
            d.Note(e, { 3840, 720, 28, { 0, 0, 1280, 720 } }, kLeft | kRight);
            d.Note(e, { 3840, 720, 28, { 1280, 0, 1280, 720 } }, kLeft | kRight);
            d.Note(e, { 3840, 720, 28, { 2560, 0, 1280, 720 } }, kLeft | kRight);
        }
        const auto s = d.Status();
        Check(s.screens == 3 && s.views[2].rect.x == 2560, "only the new rectangles are listed, still three screens");
        const auto r = d.Note(110, { 3840, 720, 28, { 2560, 0, 1280, 720 } }, kLeft | kRight);
        Check(r.verdict == Verdict::Run && r.slot == 2, "the new right screen runs on slot 2");
    }

    std::printf("Mask helpers:\n");
    Check(CountRuns(kLeft | kRight | kWhole) == 3 && Runs(kWhole, kWholeBit) && !Runs(kLeft, 1), "bits");

    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
