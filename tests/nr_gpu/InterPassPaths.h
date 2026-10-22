#pragma once
// Test-only: the inter-pass path catalogue and selection the v59 port was measured with (RTX 4090 timings,
// docs/validation/README.md). Production Superior always runs the Fast cached Low36 + Cross5 path; it uses only
// InterPass::Eligible from DlssNr_InterPassPolicy.h. Kept so the policy/benchmark tests can still check the choice.
#include <cstdint>
#include "../../OptiScaler/dlssnr/DlssNr_InterPassPolicy.h"

namespace DlssNr::InterPass
{
inline uint32_t PendingPasses(uint32_t completed, uint32_t covered)
{
    return completed > covered ? completed - covered : 1u;
}
enum class Path : uint32_t
{
    Off,
    ClassicReference,
    FusedReference,
    ClassicOptimized,
    FusedOptimized,
    Rgb20,
    Rgb16,
    FastGuided,
    FastCombinedCross5 = 10, // Retain the persisted/report identity of the old Cross5 path.
    FastCachedLow36 = 11     // Opt-in shared samples; the original Fast remains the control.
};
inline const char* Name(Path path)
{
    switch (path)
    {
    case Path::Off:
        return "Off";
    case Path::ClassicReference:
        return "Classic reference";
    case Path::FusedReference:
        return "Fused reference";
    case Path::ClassicOptimized:
        return "Classic optimized";
    case Path::FusedOptimized:
        return "Fused optimized P50/general";
    case Path::Rgb20:
        return "Fused RGB20";
    case Path::Rgb16:
        return "Fused RGB16";
    case Path::FastGuided:
        return "Fast + Low36 (approximate)";
    case Path::FastCombinedCross5:
        return "Fast + Low36 + Cross5 (approximate)";
    case Path::FastCachedLow36:
        return "Fast + cached Low36 + Cross5 (experimental)";
    }
    return "Unknown";
}
inline bool Fast(Path path)
{
    return path == Path::FastGuided || path == Path::FastCombinedCross5 || path == Path::FastCachedLow36;
}
inline bool Fused(Path path)
{
    return path == Path::FusedReference || path == Path::FusedOptimized || path == Path::Rgb20 || path == Path::Rgb16 ||
           Fast(path);
}
inline bool Optimized(Path path) { return path >= Path::ClassicOptimized; }
// Select using actual texture geometry, not rounded slider labels. Measurements:
// RTX 4090, TLOU Part I, Area/radius-one. Other settings retain exact classical filters.
inline Path Select(uint32_t mode, uint32_t nativeW, uint32_t nativeH, uint32_t workW, uint32_t workH, uint32_t filter,
                   uint32_t radius, float confidence)
{
    if (mode == 0)
        return Path::Off;
    if (mode == 1)
        return Path::ClassicReference;
    if (mode == 2)
        return Path::FusedReference;
    if (filter == 0 && radius == 1 && confidence > 0 && nativeW && nativeH && workW && workH)
    {
        // Opt-in quality tradeoff: commute native guided reconstruction with Area reduction.
        // Other filters/radii/scales retain the exact Optimized policy below.
        if (mode >= 4 && mode <= 8 && nativeW >= workW && nativeH >= workH && (nativeW > workW || nativeH > workH))
        {
            // Approved Cross5 is part of Fast throughout its eligible range, including P50.
            // FastGuided remains only as a shader regression/diagnostic control without Cross5.
            if (mode == 8)
                return Path::FastCachedLow36;
            return Path::FastCombinedCross5;
        }
        if (uint64_t(workW) * 2 == nativeW && uint64_t(workH) * 2 == nativeH)
            return Path::FusedOptimized;
        // v18/v19 whole-NR timings: aligned P40 and P42/P45/P49 favour RGB20;
        // P33/P41 favour Classic. Exactly P40 has integral 20-texel group edges.
        const bool alignedP40 =
            uint64_t(workW) * 5 == uint64_t(nativeW) * 2 && uint64_t(workH) * 5 == uint64_t(nativeH) * 2;
        // Half a working texel admits the rounded P42 dimensions on both axes
        // (3840x2160 -> 1613x907). Fractional scales use this conservative cutoff,
        // not a claim of a measured crossover at every intermediate percentage.
        const bool atLeastP42 = uint64_t(workW) * 100 + 50 >= uint64_t(nativeW) * 42 &&
                                uint64_t(workH) * 100 + 50 >= uint64_t(nativeH) * 42;
        if (nativeW > workW && nativeH > workH && (alignedP40 || atLeastP42) &&
            uint64_t(workW) * 10 <= uint64_t(nativeW) * 9 && uint64_t(workH) * 10 <= uint64_t(nativeH) * 9)
        {
            const bool compact =
                uint64_t(nativeW) * 3 <= uint64_t(workW) * 5 && uint64_t(nativeH) * 3 <= uint64_t(workH) * 5;
            return compact ? Path::Rgb16 : Path::Rgb20;
        }
    }
    return Path::ClassicOptimized;
}
} // namespace DlssNr::InterPass
