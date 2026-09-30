#pragma once

// Constants of precompile/dlssnr_detail_reuse.hlsl (reuse detail between frames, dlssnr/DlssNrDetailReuse.h). No dependencies, so
// tests/nr_detail_reuse_shader_smoke.cpp can fill it too. Same 256-byte slot as DlssNrConstants, so it goes through
// DlssNr_Dx12's constant-buffer ring.

#include <cstdint>

enum DlssNrDetailReuseMode : uint32_t
{
    DlssNrDetailReuse_Capture = 0,    // [work]   t0 input, t1 answer, t2 depth guide -> u0 detail, u1 colour + depth
    DlssNrDetailReuse_Reproject = 1,  // [work]   t0 input, t1 detail, t2 colour + depth, t3 motion, t4 depth guide
                                      //          -> u0 answer = input + moved detail * trust
    DlssNrDetailReuse_SaveMotion = 2, // [work]   t3 motion guide -> u0 this frame's vectors as uv displacement
    DlssNrDetailReuse_Compose = 3,    // [motion] t3 motion guide, t4 saved vectors -> u0 raw vectors over two frames
    DlssNrDetailReuse_Steady = 4,     // [work]   t0 input, t1 answer, t2 estimate -> u0 steadied answer
    DlssNrDetailReuse_Estimate = 5,   // [work]   as Reproject -> u0 moved detail (rgb) and its trust (a)
    DlssNrDetailReuse_Fill = 6,       // [work]   t0 input, t1 estimate, t4 depth guide -> u0 answer, where dropped
                                      //          detail is filled from trusted neighbours on the same surface
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
};
static_assert(sizeof(DlssNrDetailReuseConstants) == 256);

// Starting values, to be set from in-game tests with the debug view.
constexpr float kDlssNrDetailReuseDepthTolerance = 0.02f;
constexpr float kDlssNrDetailReuseClipGamma = 1.25f;
constexpr float kDlssNrDetailReuseClipFalloff = 1.0f;
constexpr float kDlssNrDetailReuseSigmaFloor = 0.01f;
constexpr float kDlssNrDetailReuseFillRadius1080p = 24.0f; // scaled with the working size
