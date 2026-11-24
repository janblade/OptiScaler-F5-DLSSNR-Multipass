#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "Constants.h"
#include "../DlssNr_Common.h"

// Superior passes reuse constant-buffer fields named for other modes (the v59 shader ABI and the shared resolve's
// cbuffer). Every such reuse goes through one of these named setters, and the offsets the shaders read are pinned
// below, so a layout change fails to compile instead of silently moving a value into another field.
namespace DlssNr::Superior::Fields
{
// Final and inter-pass frequency shaping: the high and low band gains travel in MvScaleX / MvScaleY
// (HostResolve.hlsli: gMvScaleX * edit + (gMvScaleY - gMvScaleX) * low). Both constant structs.
template <class C> inline void SetBandGains(C& c, float high, float low)
{
    c.MvScaleX = high;
    c.MvScaleY = low;
}
template <class C> inline float HighGain(const C& c) { return c.MvScaleX; }
template <class C> inline float LowGain(const C& c) { return c.MvScaleY; }

// Host resolve flags (HostResolve.hlsli): GuideWidth != 0 binds the 1/8 low field (or answer_work in Direct SR) at t5;
// GuideHeight: 0 = decoded signed DLSS residual, 1 = raw NR answer (non-temporal), 2 = Direct DLSS SR native C1 carrier.
template <class C> inline void SetResolveFlags(C& c, bool lowField, bool rawAnswer)
{
    c.GuideWidth = lowField ? 1u : 0u;
    c.GuideHeight = rawAnswer ? 1u : 0u;
}
template <class C>
inline void SetDirectSrResolve(C& c, bool haveWorkAnswer, float nativeDetail, bool fastResolve = false,
                               float nrSharpness = 0.65f)
{
    c.MvScaleX = (nativeDetail > 0.0f && nativeDetail <= 1.0f) ? nativeDetail : (nativeDetail > 1.0f ? 1.0f : 0.0f);
    c.MvScaleY = fastResolve ? 0.0f : 1.0f;
    c.GuideWidth = (!fastResolve && haveWorkAnswer) ? 1u : 0u;
    c.GuideHeight = 2u;
    const float clampedSharp = (nrSharpness > 0.0f && nrSharpness <= 1.0f) ? nrSharpness : (nrSharpness > 1.0f ? 1.0f : 0.0f);
    std::memcpy(&c.ExposureSourceWidth, &clampedSharp, sizeof(clampedSharp));
}
inline void SetOracleDecomp(DlssNrConstants& c, bool packed, float jitterX, float jitterY)
{
    c.Mode = packed ? DlssNrMode_OracleDecompPacked : DlssNrMode_OracleDecomp;
    c.CompareSplit = jitterX;
    c.CompareZoom = jitterY;
}
inline void SetOracleTemporal(DlssNrConstants& c, bool reset, bool haveDepth, bool depthInverted, float jitterX,
                              float jitterY)
{
    c.Mode = DlssNrMode_OracleTemporal;
    c.Passthrough = (depthInverted ? 1u : 0u) | (reset ? 2u : 0u) | (haveDepth ? 4u : 0u);
    c.CompareSplit = jitterX;
    c.CompareZoom = jitterY;
}
inline void SetOracleJbu(DlssNrConstants& c, uint32_t midW, uint32_t midH, bool haveDepth, bool depthInverted,
                         uint32_t depthX, uint32_t depthY, uint32_t depthW, uint32_t depthH)
{
    c.Mode = DlssNrMode_OracleJbu;
    c.Width = midW;
    c.Height = midH;
    c.GuideWidth = (haveDepth ? 1u : 0u) | (depthInverted ? 2u : 0u);
    c.ExposureSourceWidth = (depthX & 0xFFFFu) | ((depthY & 0xFFFFu) << 16u);
    c.ExposureSourceHeight = (depthW & 0xFFFFu) | ((depthH & 0xFFFFu) << 16u);
}
template <class C>
inline void SetOracleResolve(C& c, bool haveDepth, bool depthInverted, uint32_t depthX, uint32_t depthY,
                             uint32_t depthW, uint32_t depthH, float nrSharp, float nativeDetail,
                             bool preJbu = false, bool haveWorkDepth = false)
{
    c.GuideHeight = 3u;
    c.GuideWidth = (haveDepth ? 1u : 0u) | (depthInverted ? 2u : 0u) | (preJbu ? 4u : 0u) | (haveWorkDepth ? 8u : 0u);
    c.MvScaleX = (nrSharp > 0.0f && nrSharp <= 1.0f) ? nrSharp : (nrSharp > 1.0f ? 1.0f : 0.0f);
    c.MvScaleY = (nativeDetail > 0.0f && nativeDetail <= 1.0f) ? nativeDetail : (nativeDetail > 1.0f ? 1.0f : 0.0f);
    c.ExposureSourceWidth = (depthX & 0xFFFFu) | ((depthY & 0xFFFFu) << 16u);
    c.ExposureSourceHeight = (depthW & 0xFFFFu) | ((depthH & 0xFFFFu) << 16u);
}
inline void SetPrivateDetailRecovery(DlssNrConstants& c, float recovery)
{
    std::memcpy(&c.ExposureSourceWidth, &recovery, sizeof(recovery));
}

// Inter-pass shaping (v59 mode 36): GuideWidth 1 = a low field is bound.
inline void SetInterPassLowField(DlssNrConstants& c, bool lowField) { c.GuideWidth = lowField ? 1u : 0u; }
inline bool InterPassLowField(const DlssNrConstants& c) { return c.GuideWidth != 0u; }

// v59 guide resize / carrier modes (10, 39): the depth and motion regions travel in the comparison fields.
//   depth:  size GuideWidth/GuideHeight, origin DebugView/CompareMode
//   motion: size TransferStrength/ColourStrength (as floats), origin CompareSwap/Transfer
inline void SetDepthRegion(DlssNrConstants& c, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    c.GuideWidth = w;
    c.GuideHeight = h;
    c.DebugView = x;
    c.CompareMode = y;
}
inline void SetMotionRegion(DlssNrConstants& c, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    c.TransferStrength = float(w);
    c.ColourStrength = float(h);
    c.CompareSwap = x;
    c.Transfer = y;
}

// Private motion uses the active source MV rectangle, in 1080p-height pixel units.
inline void SetPrivateMotion(DlssNrConstants& c, unsigned x, unsigned y, unsigned w, unsigned h, float scaleX,
                             float scaleY)
{
    c.Mode = DlssNrMode_PrivateMotion;
    c.Width = c.Height = 1;
    SetDepthRegion(c, x, y, w, h);
    c.MvScaleX = scaleX;
    c.MvScaleY = scaleY;
}
inline void SetPrivateFade(DlssNrConstants& c, float rawWeight)
{
    c.Mode = DlssNrMode_PrivateFade;
    std::memcpy(&c.ExposurePadding, &rawWeight, sizeof(rawWeight));
}
static_assert(offsetof(DlssNrConstants, ExposurePadding) == 172);

// Compact visibility and reference history share the first 52 bytes.
inline void SetPrivateGhost(DlssNrConstants& c, unsigned w, unsigned h, bool inverted, bool previous,
                            float depthPrecision)
{
    c.Width = (w + 3) / 4;
    c.Height = (h + 3) / 4;
    c.GuideWidth = w;
    c.GuideHeight = h;
    c.WhitePoint = 18;
    c.TransferStrength = 6;
    c.ColourStrength = .05f;
    c.MaxRatio = 2.5f;
    c.Passthrough = inverted;
    c.DebugView = previous;
    c.MvScaleX = depthPrecision;
}
inline void SetPrivateRings(DlssNrConstants& c, unsigned w, unsigned h, bool inverted, float depthPrecision, bool apply)
{
    c = {};
    c.Mode = apply ? DlssNrMode_PrivateGhostFeather : DlssNrMode_PrivateGhostRings;
    c.Width = apply ? w : (w + 3) / 4;
    c.Height = apply ? h : (h + 3) / 4;
    c.ColourStrength = .05f;
    c.DebugView = inverted;
    c.MaxRatio = depthPrecision;
}

// The fields above, at the offsets both cbuffers (superior/precompile/dlssnr.hlsl and precompile/dlssnr.hlsl,
// "cbuffer Params") declare them. Changing either struct or cbuffer requires changing these and rebuilding blobs.
#define DLSSNR_SUPERIOR_PIN(Struct)                                                                                    \
    static_assert(offsetof(Struct, Mode) == 0 && offsetof(Struct, Width) == 8 && offsetof(Struct, Height) == 12);      \
    static_assert(offsetof(Struct, TransferStrength) == 16 && offsetof(Struct, ColourStrength) == 20);                 \
    static_assert(offsetof(Struct, DebugView) == 24 && offsetof(Struct, Passthrough) == 32);                           \
    static_assert(offsetof(Struct, MvScaleX) == 36 && offsetof(Struct, MvScaleY) == 40);                               \
    static_assert(offsetof(Struct, GuideWidth) == 44 && offsetof(Struct, GuideHeight) == 48);                          \
    static_assert(offsetof(Struct, CompareMode) == 52 && offsetof(Struct, CompareSplit) == 56);                          \
    static_assert(offsetof(Struct, CompareZoom) == 60 && offsetof(Struct, CompareSwap) == 64);                         \
    static_assert(offsetof(Struct, Transfer) == 68);
DLSSNR_SUPERIOR_PIN(DlssNr::Superior::DlssNrConstants)
DLSSNR_SUPERIOR_PIN(::DlssNrConstants)
// Each cbuffer declares ExposureSourceWidth / ExposureSourceHeight at its own struct offset (matching superior/precompile/dlssnr.hlsl vs precompile/dlssnr.hlsl).
static_assert(offsetof(DlssNr::Superior::DlssNrConstants, ExposureSourceWidth) == 164 &&
              offsetof(DlssNr::Superior::DlssNrConstants, ExposureSourceHeight) == 168);
static_assert(offsetof(::DlssNrConstants, ExposureSourceWidth) == 128 &&
              offsetof(::DlssNrConstants, ExposureSourceHeight) == 132);
// The Superior blobs read the first 256 bytes of whatever buffer they are given; the live struct is longer (the depth
// fade) and keeps the same first 256 bytes.
static_assert(sizeof(DlssNr::Superior::DlssNrConstants) == 256 && sizeof(::DlssNrConstants) >= 256);
#undef DLSSNR_SUPERIOR_PIN
} // namespace DlssNr::Superior::Fields
