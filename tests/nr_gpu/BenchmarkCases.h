#pragma once
#include "InterPassPaths.h"
#include <array>
namespace DlssNr
{
struct InterPassBenchmarkScale { int percent; InterPass::Path expected; double historicalGainVsFusedMs; };
// Last supplied RTX4090/4K/six-pass/guide-one measurements, not a guarantee.
inline constexpr std::array<InterPassBenchmarkScale, 3> kInterPassBenchmarkScales {{
    {50, InterPass::Path::FusedOptimized, 1.74439},
    {59, InterPass::Path::Rgb20, 3.53894},
    {65, InterPass::Path::Rgb16, 4.35814},
}};
// Independent forced controls from supplied v18/v19 whole-NR measurements.
// P45 uses the same retained RGB20 path as P49; omit its redundant rerun.
inline constexpr std::array<InterPassBenchmarkScale, 6> kInterPassMeasuredLowScales {{
    {33, InterPass::Path::ClassicOptimized, 1.77459},
    {40, InterPass::Path::Rgb20, 1.55955},
    {41, InterPass::Path::ClassicOptimized, 2.63322},
    {42, InterPass::Path::Rgb20, 3.24813},
    {45, InterPass::Path::Rgb20, 3.68077},
    {49, InterPass::Path::Rgb20, 3.07046},
}};
inline constexpr std::array<int, 5> kInterPassLowVerificationScales {33, 40, 41, 42, 49};
// Historical three-candidate component fixture, outside the in-game verification.
inline constexpr std::array<int, 4> kInterPassLowBenchmarkScales {33, 40, 45, 49};
// Resolve partial RGB20 tile coverage; do not repeat already measured scales.
inline constexpr std::array<int, 2> kInterPassBoundaryBenchmarkScales {41, 42};
// A/B/C/C/B/A gives every candidate the same mean position within the sweep.
inline constexpr std::array<InterPass::Path, 6> kInterPassLowBenchmarkOrder {
    InterPass::Path::ClassicOptimized, InterPass::Path::FusedOptimized, InterPass::Path::Rgb20,
    InterPass::Path::Rgb20, InterPass::Path::FusedOptimized, InterPass::Path::ClassicOptimized
};
}
