#pragma once

// Constants of precompile/dlssnr_detail_reuse.hlsl (reuse detail between frames, dlssnr/DlssNrDetailReuse.h). No dependencies, so
// tests/nr_detail_reuse_shader_smoke.cpp can fill it too. Same 256-byte slot as DlssNrConstants, so it goes through
// DlssNr_Dx12's constant-buffer ring.

#include <cstdint>

enum DlssNrDetailReuseMode : uint32_t
{
    DlssNrDetailReuse_Capture = 0,    // [work]   t0 input, t1 answer, t2 depth guide -> u0 detail, u1 colour + depth
    DlssNrDetailReuse_Reproject = 1,  // [work]   t0 input, t1 detail, t2 colour + depth, t3 motion, t4 depth guide,
                                      //          t5 history distrust (HistoryDistrust) -> u0 answer = input + moved
                                      //          detail * trust
    DlssNrDetailReuse_SaveMotion = 2, // [work]   t3 motion guide -> u0 this frame's vectors as uv displacement
    DlssNrDetailReuse_Compose = 3,    // [motion] t3 motion guide, t4 saved vectors -> u0 raw vectors over two frames
    DlssNrDetailReuse_Steady = 4,     // [work]   t0 input, t1 answer, t2 estimate -> u0 steadied answer
    DlssNrDetailReuse_Estimate = 5,   // [work]   as Reproject -> u0 moved detail (rgb) and its trust (a)
    DlssNrDetailReuse_Fill = 6,       // [work]   t0 input, t1 estimate, t4 depth guide -> u0 answer, where dropped
                                      //          detail is filled from trusted neighbours on the same surface
    DlssNrDetailReuse_Coverage = 7,   // [grid]   t1 estimate -> u0 a kCoverage x kCoverage grid, one texel per tile:
                                      //          (sum of 1 - trust, pixels measured, 0, 0). Dispatched over
                                      //          kDlssNrDetailReuseCoverageTiles * 8 in each direction, so there is one
                                      //          8x8 thread group per tile.
};

// The curve a Replace answer is decoded through (dlssnr.hlsl: Neutwo + replace, balanced + replace). None for the
// Composed modes and for passthrough frames, which land a moved change as the difference in the model's own values.
// Which proxy curve gives which: DlssNrDetailReuse::ReplaceCurveFor (dlssnr/DlssNrDetailReuseHost.h).
enum DlssNrReplaceCurve : uint32_t
{
    DlssNrReplaceCurve_None = 0,
    DlssNrReplaceCurve_Neutwo = 1,
    DlssNrReplaceCurve_Hybrid = 2,
};

struct alignas(256) DlssNrDetailReuseConstants
{
    uint32_t Mode;
    uint32_t WorkWidth;
    uint32_t WorkHeight;
    uint32_t MotionWidth;
    uint32_t MotionHeight;
    uint32_t MotionBaseX;
    uint32_t MotionBaseY;
    uint32_t DepthWidth;
    uint32_t DepthHeight;
    uint32_t DepthBaseX;
    uint32_t DepthBaseY;
    uint32_t DepthInverted;
    float MvScaleX; // the game's scale: raw vectors -> pixels of the motion subrect
    float MvScaleY;
    float DepthTolerance; // relative depth outside the saved 2x2 range still trusted fully; trust is gone at twice it
    float ClipGamma;      // half-width of the colour box around the current 3x3 mean, in standard deviations
    float ClipFalloff;    // distance outside the box, in standard deviations, over which trust fades to zero
    float SigmaFloor;     // smallest standard deviation used (proxy units), so flat areas do not reject on noise
    float Steady;         // full frames: how far the new detail is pulled toward the moved previous detail (0..1)
    uint32_t DebugView;   // Reproject / Fill: paint dropped detail magenta (Fill: filled detail cyan)
    float FillStrength;   // Fill: how much of the neighbours' detail goes into dropped pixels (0..1)
    float FillRadius;     // Fill: outer radius of the neighbour search, in working-size pixels
    // Which curve decodes a Replace answer (DlssNrReplaceCurve). Not None: a moved change lands as the difference or as
    // the ratio of light it made at its source, whichever moves the pixel less, since the decode's inverse amplifies a
    // moved difference of proxy values without bound near white.
    uint32_t ReplaceCurve;
    // A moved sample is distrusted where the current vectors at the place it came from differ from this pixel's own by
    // more than this share of (its length + 1), in working-size pixels: something else moves there now. 0 is off.
    float MotionReject;
    // Steady: a difference between the full frame and the moved detail smaller than this (proxy units; about one 8-bit step) is
    // taken as none, so rounding does not show the two apart. 0 is off.
    float SteadyDeadZone;
    // Reproject / Estimate: t5 holds an outside opinion of each pixel's history, distrust 0..1 in .r at any size
    // (DlssNrFrameInfo::HistoryDistrust; Optical F5Low's trust mask on native input), which scales the moved detail's
    // trust. 0: none, and t5 is a stand-in that is not read.
    uint32_t HistoryDistrust;
};
static_assert(sizeof(DlssNrDetailReuseConstants) == 256);

// The defaults behind the DetailReuse* config entries of the same name. These were starting values, never
// measured against a game, until DepthTolerance was (see its note below); the other three still are not.
// Raised from 0.02 on 2026-10-01. At 0.02 the depth half of a moved sample's trust rejects samples the
// reprojection got right, and a reused frame then keeps none of its own moved detail there, so full and reused
// frames differ and the picture alternates between them. Seen on water at a distance in The Witcher 3 and in
// RDR2, which is why this is a default rather than a per-game value. Measured in the Witcher (paused lake):
// blinks at 0.02, still blinks at 0.030-0.040, clean from 0.051 up; no ghosting at 0.05 in NBA 2K27's fast
// breaks, the case most likely to show a loose depth test, nor anywhere in the Witcher at 0.25.
// That fixes the SIZE of the depth disagreement, about 0.05, and not its cause. Two fit and the measurement
// does not separate them: a depth guide that describes something other than the surface the colour and the
// vectors come from (for shallow water, the bed), or a surface seen near edge-on, where the test's sub-pixel
// slack becomes a large relative difference on the right surface. 0.051 carries no margin above the one value
// measured, so a game needing more sets DetailReuseDepthTolerance itself.
// NOTE: Fill weights its neighbour taps over three times this, so this also widens how far across a depth step
// Fill may borrow detail. The measurement above exercised the trust test; the Fill side was not isolated.
constexpr float kDlssNrDetailReuseDepthTolerance = 0.051f;
constexpr float kDlssNrDetailReuseClipGamma = 1.25f;
constexpr float kDlssNrDetailReuseClipFalloff = 1.0f;
constexpr float kDlssNrDetailReuseSigmaFloor = 0.01f;
constexpr float kDlssNrDetailReuseFillRadius1080p = 24.0f; // scaled with the working size
constexpr float kDlssNrDetailReuseMotionReject = 0.5f;
constexpr float kDlssNrDetailReuseSteadyDeadZone = 1.0f / 255.0f;

// The Coverage grid: one texel per tile, kept small because it is read back to the CPU every measured frame. 32x32
// tiles are enough for a share of the frame, and let the host see where the picture was dropped if that is ever wanted.
// The shader has the same number (kCoverageTiles in dlssnr_detail_reuse.hlsl); the WARP test checks the two agree.
constexpr unsigned int kDlssNrDetailReuseCoverageTiles = 32;
