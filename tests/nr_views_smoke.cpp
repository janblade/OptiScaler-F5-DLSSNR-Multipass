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

// One frame of the three screens (each evaluate twice: the before-SR and the after-SR seam).
static void ThreeScreens(Detector& d, unsigned long long epoch, Pick pick, Verdict out[3], bool withWhole = false)
{
    for (int i = 0; i < 3; ++i)
    {
        d.Note(epoch, Screen(i), pick);
        out[i] = d.Note(epoch, Screen(i), pick).verdict;
    }
    if (withWhole)
        d.Note(epoch, Whole(), pick);
}

int main()
{
    std::printf("A single view is left alone:\n");
    {
        Detector d;
        bool allRun = true;
        for (unsigned long long e = 1; e <= 200; ++e)
        {
            // Dynamic resolution moves the one rectangle from frame to frame.
            const auto r = d.Note(e, Single(1700 + (unsigned int) (e % 7) * 10, 950), Pick::Off);
            allRun = allRun && r.verdict == Verdict::Run;
        }
        Check(allRun, "every evaluate runs");
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
            run = run && d.Note(e, main, Pick::Off).verdict == Verdict::Run;
            run = run && d.Note(e, mirror, Pick::Off).verdict == Verdict::Run;
        }
        Check(run && !d.Status().active, "a mirror or portrait texture does not stand NR down");
    }

    std::printf("Three screens in one allocation, nothing picked:\n");
    {
        Detector d;
        Verdict v[3];
        ThreeScreens(d, 1, Pick::Off, v);
        Check(d.Status().active, "found within the first frame");
        Check(v[1] == Verdict::StandDown && v[2] == Verdict::StandDown, "the later screens stand down");
        ThreeScreens(d, 2, Pick::Off, v);
        Check(v[0] == Verdict::StandDown && v[1] == Verdict::StandDown && v[2] == Verdict::StandDown,
              "from the next frame on, all of them");
        Check(d.Status().count == 3, "three candidates");
        Check(d.Status().rects[0].w == 1706 && d.Status().rects[1].x == 1706 && d.Status().rects[2].x == 3413,
              "listed left to right");
        Check(!d.Status().hasSelected, "nothing selected");
        Check(d.Status().standDownEvaluates > 0, "the stood-down evaluates are counted");
    }

    std::printf("Pick the smallest (the first screen):\n");
    {
        Detector d;
        Verdict v[3];
        ThreeScreens(d, 1, Pick::Smallest, v, true);
        ThreeScreens(d, 2, Pick::Smallest, v, true);
        Check(v[0] == Verdict::Run, "screen 1 (at the origin) runs");
        Check(v[1] == Verdict::OtherView && v[2] == Verdict::OtherView, "screens 2 and 3 do not");
        Check(d.Note(2, Whole(), Pick::Smallest).verdict == Verdict::OtherView, "the combined feature does not");
        const auto s = d.Status();
        Check(s.hasSelected && s.selected.w == 1706 && s.selected.x == 0, "selected = 1706x960 at 0,0");
        Check(s.selectedNeedsBeforeSr, "a single screen of a shared output must run before SR");
        Check(s.count == 4, "the combined feature is listed too");
    }

    std::printf("Pick the largest (the whole picture):\n");
    {
        Detector d;
        Verdict v[3];
        ThreeScreens(d, 1, Pick::Largest, v, true);
        ThreeScreens(d, 2, Pick::Largest, v, true);
        Check(d.Note(2, Whole(), Pick::Largest).verdict == Verdict::Run, "the combined feature runs");
        Check(v[0] == Verdict::OtherView && v[1] == Verdict::OtherView, "the screens do not");
        Check(!d.Status().selectedNeedsBeforeSr, "the whole picture may run after SR too");
    }

    std::printf("Largest without a combined feature falls on the first screen, never on one off the origin:\n");
    {
        Detector d;
        Verdict v[3];
        ThreeScreens(d, 1, Pick::Largest, v);
        ThreeScreens(d, 2, Pick::Largest, v);
        Check(v[0] == Verdict::Run && v[1] == Verdict::OtherView && v[2] == Verdict::OtherView,
              "screen 1 (1707 wide screens start off the origin and are not chosen)");
    }

    std::printf("Only views that start away from the origin: nothing can be picked:\n");
    {
        Detector d;
        for (unsigned long long e = 1; e <= 3; ++e)
        {
            d.Note(e, Screen(1), Pick::Largest);
            d.Note(e, Screen(2), Pick::Largest);
        }
        Check(d.Note(4, Screen(1), Pick::Largest).verdict == Verdict::StandDown, "stands down");
        Check(!d.Status().hasSelected, "no selection");
    }

    std::printf("The layout goes away only after a long quiet time:\n");
    {
        Detector d;
        Verdict v[3];
        ThreeScreens(d, 1, Pick::Off, v);
        bool stood = true;
        for (unsigned long long e = 2; e < 2 + kQuietEpochs - 2; ++e)
            stood = stood && d.Note(e, Screen(0), Pick::Off).verdict == Verdict::StandDown;
        Check(stood && d.Status().active, "one view per frame for a while (a menu, a loading screen) does not release it");
        bool released = false;
        for (unsigned long long e = 2 + kQuietEpochs - 2; e < 2 + kQuietEpochs + 5; ++e)
            released = released || d.Note(e, Screen(0), Pick::Off).unlatchedNow;
        Check(released && !d.Status().active, "released after kQuietEpochs frames with one view");
        Check(d.Note(1000, Screen(0), Pick::Off).verdict == Verdict::Run, "the single view runs again");
    }

    std::printf("A resolution change leaves no stale candidate:\n");
    {
        Detector d;
        Verdict v[3];
        ThreeScreens(d, 1, Pick::Smallest, v);
        // The rig is switched to a smaller size: new rectangles, the old ones stop being drawn.
        for (unsigned long long e = 100; e < 110; ++e)
        {
            d.Note(e, { 3840, 720, 28, { 0, 0, 1280, 720 } }, Pick::Smallest);
            d.Note(e, { 3840, 720, 28, { 1280, 0, 1280, 720 } }, Pick::Smallest);
            d.Note(e, { 3840, 720, 28, { 2560, 0, 1280, 720 } }, Pick::Smallest);
        }
        const auto s = d.Status();
        Check(s.hasSelected && s.selected.w == 1280, "the selected view is the new first screen");
        Check(s.count == 3, "only the new rectangles are listed");
    }

    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
