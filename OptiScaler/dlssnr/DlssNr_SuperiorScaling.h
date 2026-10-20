#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace DlssNr::SuperiorScaling
{
std::string Status();
constexpr uint32_t Transfer = 11u;
constexpr uint32_t ShaderFlag = 65536u;
constexpr uint32_t LowGroupHeight = 16u;
inline bool Enabled(uint32_t transfer) { return transfer == Transfer; }
template<class C> inline bool Enabled(const C& cfg) { return Enabled(cfg.DlssNrTransfer.value_or_default()); }
inline int Preset(int preset) { return preset == 11 ? 11 : 1; } // K or A; no default/latest ambiguity.
inline float High(unsigned style, unsigned nrPreset = 1u)
{
    constexpr float main[] { .8f, .75f, .95f }, small[] { .950f, .925f, .955f };
    return (nrPreset == 2u ? small : main)[std::min(style, 2u)];
}
inline float Low(unsigned style, unsigned nrPreset = 1u)
{
    constexpr float main[] { .661f, .787f, .784f }, small[] { .861f, .838f, .861f };
    return (nrPreset == 2u ? small : main)[std::min(style, 2u)];
}

template<class C> inline bool InputEncode(const C& c, uint64_t sourceWidth, uint32_t sourceHeight)
{
    return c.Mode == 0u && c.DirectResolveFlags == ShaderFlag && c.Passthrough <= 1u &&
           c.ReversibleMode <= 4u && c.Width != 0u && c.Height != 0u &&
           sourceWidth == c.Width && sourceHeight == c.Height;
}

// Fixed 64x64 meter and 1x1 white reduction, only for Superior's automatic HDR exposure.
template<class C> inline uint32_t ExposureStage(const C& c, uint64_t sourceWidth, uint32_t sourceHeight)
{
    if (c.DirectResolveFlags != ShaderFlag || c.ExposureSourceWidth == 0u || c.ExposureSourceHeight == 0u)
        return 0u;
    if (c.Mode == 3u && c.Width == 64u && c.Height == 64u &&
        sourceWidth == c.ExposureSourceWidth && sourceHeight == c.ExposureSourceHeight) return 1u;
    if (c.Mode == 11u && c.Width == 1u && c.Height == 1u && sourceWidth == 64u && sourceHeight == 64u)
        return 2u;
    return 0u;
}

// Area at the model input only, with the materialized native proxy retained.
template<class C> inline bool AreaDownsample(const C& c, uint64_t sourceWidth, uint32_t sourceHeight)
{
    return c.Mode == 2u && c.Transfer == 0u && c.DirectResolveFlags == ShaderFlag &&
           c.Width != 0u && c.Height != 0u && sourceWidth >= c.Width && sourceHeight >= c.Height &&
           (sourceWidth != c.Width || sourceHeight != c.Height);
}

// Final low field of a decoded signed residual, at the existing ceil(work/8) dimensions.
template<class C> inline bool LowResidual(const C& c, uint64_t sourceWidth, uint32_t sourceHeight)
{
    return c.Mode == 18u && c.Transfer == 1u && c.DirectResolveFlags == ShaderFlag &&
           sourceWidth != 0u && sourceHeight != 0u &&
           c.Width == (sourceWidth - 1u) / 8u + 1u && c.Height == (sourceHeight - 1u) / 8u + 1u;
}

// Reuse only the identical current-frame R32 point-sampled depth, already in SRV state.
template<class R> inline bool ReuseDepth(bool sameSource, bool r32, uint64_t width, uint32_t height,
                                        uint32_t workWidth, uint32_t workHeight, const R& nr, const R& dlaa)
{
    return sameSource && r32 && workWidth != 0u && workHeight != 0u &&
           width == workWidth && height == workHeight && nr.valid() && dlaa.valid() &&
           nr.x == dlaa.x && nr.y == dlaa.y && nr.width == dlaa.width && nr.height == dlaa.height;
}

// Mode 1 / signed Transfer 9 only. The optimized PSO fixes inspector switches and actual reduction.
template<class C> inline bool NormalResolve(const C& c, uint64_t sourceWidth, uint32_t sourceHeight)
{
    return c.Mode == 1u && c.ResidualHistoryValid == 1u && c.Transfer == 9u &&
           c.DebugView == 0u && c.CompareMode == 0u && c.ShowSkinMask == 0u && c.ApplyModel != 0u &&
           c.DirectResolveFlags == ShaderFlag && sourceWidth != 0u && sourceHeight != 0u &&
           sourceWidth <= c.Width && sourceHeight <= c.Height &&
           (sourceWidth != c.Width || sourceHeight != c.Height);
}

// Called after NormalResolve eligibility; only HDR soft knee has this literal PSO.
template<class C> inline bool SoftKneeResolve(const C& c)
{
    return c.Passthrough == 0u && c.ReversibleMode == 0u;
}

// Called after NormalResolve eligibility. Zero retains the general normal PSO.
template<class C> inline uint32_t NormalColorProfile(const C& c)
{
    // Retain v50 rounding in the optional replace-mode native-detail restoration.
    if ((c.ReversibleMode == 2u || c.ReversibleMode == 4u) && c.ModelWorkScale > 0.0f &&
        c.ModelWorkScale < .999f && c.ReplaceDetailStrength > 0.0f) return 0u;
    if (c.ReversibleMode > 4u) return 0u;
    if (c.Passthrough == 1u) return 1u; // SDR; its mapping selector remains dynamic.
    if (c.Passthrough != 0u) return 0u;
    // HDR soft knee is tried separately, with this general PSO retained as fallback.
    return c.ReversibleMode == 2u ? 2u : c.ReversibleMode == 4u ? 3u : 0u;
}
template<class C> inline uint32_t InterPassMode(const C& cfg)
{ return Enabled(cfg) ? 8u : std::min(cfg.DlssNrInterPassReconstruction.value_or_default(),8u); }
template<class C> inline uint32_t DownscaleFilter(const C& cfg)
{ return Enabled(cfg) ? 0u : cfg.DlssNrProxyDownscaleFilter.value_or_default(); }
template<class C> inline uint32_t Radius(const C& cfg)
{ return Enabled(cfg) ? 1u : std::clamp(cfg.DlssNrGuidedResidualRadius.value_or_default(),1u,3u); }
template<class C> inline float RangeSigma(const C& cfg)
{ const float v=Enabled(cfg)? .015f:cfg.DlssNrGuidedResidualRangeSigma.value_or_default(); return std::isfinite(v)?v:.015f; }
template<class C> inline float SpatialSigma(const C& cfg)
{ const float v=Enabled(cfg)?3.0f:cfg.DlssNrGuidedResidualSpatialSigma.value_or_default(); return std::isfinite(v)?v:1.2f; }
template<class C> inline float GuideStrength(const C& cfg)
{ const float v=Enabled(cfg)?1.0f:cfg.DlssNrGuidedResidualGuideStrength.value_or_default(); return std::isfinite(v)?v:.75f; }
template<class C> inline uint32_t ShapingMode(const C& cfg)
{ return Enabled(cfg)?2u:std::min(cfg.DlssNrGuidedResidualShaping.value_or_default(),2u); }
} // namespace DlssNr::SuperiorScaling
