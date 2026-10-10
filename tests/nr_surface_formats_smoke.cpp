// Host check of shaders/dlssnr/DlssNr_SurfaceFormats.h: when a format change makes NR's surfaces stale.
// cl /std:c++20 /EHsc /W4 tests/nr_surface_formats_smoke.cpp
#include "../OptiScaler/shaders/dlssnr/DlssNr_SurfaceFormats.h"

#include <cstdio>

int main()
{
    constexpr int kRgba8 = 28, kRgba16f = 10;
    int fails = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok)
        {
            std::printf("FAIL: %s\n", what);
            ++fails;
        }
    };
    using DlssNr::SurfacesStale;

    check(!SurfacesStale(0, kRgba8, 0, kRgba8), "nothing made yet: nothing stale");
    check(!SurfacesStale(kRgba8, kRgba8, kRgba8, kRgba8), "same formats: kept");
    check(SurfacesStale(kRgba8, kRgba16f, kRgba8, kRgba8), "model format changed (compression switched on)");
    // The crash: compression on, so the model is RGBA16F before and after; only the game's colour moved.
    check(SurfacesStale(kRgba16f, kRgba16f, kRgba8, kRgba16f), "only the colour format changed, model format fixed");
    check(SurfacesStale(kRgba8, kRgba16f, kRgba8, kRgba16f), "both changed");
    check(!SurfacesStale(kRgba16f, kRgba16f, kRgba16f, kRgba16f), "compression on, nothing changed: kept");
    check(SurfacesStale(kRgba16f, kRgba8, kRgba8, kRgba8), "compression switched off");

    if (fails == 0)
        std::puts("PASS: nr_surface_formats_smoke");
    return fails != 0;
}
