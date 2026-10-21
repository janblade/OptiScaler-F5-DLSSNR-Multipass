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

    // Watch Dogs: Legion swaps its output RGBA8 <-> BGRA8 (27 <-> 90) between frames: one family, nothing stale.
    using DlssNr::CanonicalColourFormat;
    using DlssNr::SameColourFamily;
    constexpr int kRgba8Typeless = 27, kRgba8Srgb = 29, kBgra8 = 87, kBgra8Typeless = 90, kBgra8Srgb = 91;
    constexpr int kRgb10a2 = 24, kR11g11b10 = 26, kBgrx8 = 88;
    check(CanonicalColourFormat(kBgra8Typeless) == kRgba8Typeless, "BGRA8 typeless -> RGBA8 typeless");
    check(CanonicalColourFormat(kBgra8) == kRgba8, "BGRA8 UNORM -> RGBA8 UNORM");
    check(CanonicalColourFormat(kBgra8Srgb) == kRgba8Srgb, "BGRA8 sRGB -> RGBA8 sRGB");
    const int others[] = { kRgba8Typeless, kRgba8, kRgba8Srgb, kRgba16f, kRgb10a2, kR11g11b10, kBgrx8 };
    for (const int f : others)
        check(CanonicalColourFormat(f) == f, "everything else keeps its format");
    check(SameColourFamily(kRgba8Typeless, kBgra8Typeless), "27 and 90 are one family");
    check(SameColourFamily(kBgra8, kRgba8), "87 and 28 are one family");
    check(!SameColourFamily(kRgba8, kRgba8Srgb), "sRGB is its own family");
    check(!SameColourFamily(kBgra8Typeless, kRgba16f), "BGRA8 and RGBA16F are not");
    check(!SameColourFamily(kRgba8, kRgb10a2), "RGBA8 and RGB10A2 are not");
    const int was = CanonicalColourFormat(kRgba8Typeless), now = CanonicalColourFormat(kBgra8Typeless);
    check(!SurfacesStale(was, now, was, now), "the swap 27 -> 90: kept");
    check(SurfacesStale(was, kRgba16f, was, kRgba16f), "27 -> RGBA16F: rebuilt");

    // The reviewer's probe: the ordinary size moved 960x544 -> 976x544 while the packed one did not.
    check(DlssNr::SizeStale(960, 544, 976, 544), "ordinary size moved, packed size did not");
    check(DlssNr::SizeStale(976, 544, 976, 560), "height moved");
    check(!DlssNr::SizeStale(976, 544, 976, 544), "same ordinary size: kept");
    check(!DlssNr::SizeStale(0, 0, 976, 544), "no surface yet");

    if (fails == 0)
        std::puts("PASS: nr_surface_formats_smoke");
    return fails != 0;
}
