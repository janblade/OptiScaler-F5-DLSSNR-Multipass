#pragma once

// The luma of one colour, shared by the optical flow (motion/OpticalFlow_Hlsl.h) and the scene-cut detector
// (motion/SceneCut_Hlsl.h): HLSL text, pasted into each source between its own string literals. It reads `whiteNits`,
// which every source that includes it declares in its constants, and LUMA_MODE (0 when not defined).

// clang-format off
#define F5LOW_LUMA_HLSL R"HLSL(// PQ (SMPTE ST 2084) to linear light, 1.0 being 10000 nits.
float3 PqToLinear(float3 v)
{
    const float3 p = pow(saturate(v), 1.0 / 78.84375);
    return pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125);
}

// The luma of one colour for the match, in one of four ways (LUMA_MODE, a different pipeline for each). 0, the old one: Rec.601
// weights and l / (1 + l) on the values as they are. 1, a gamma-encoded SDR colour: already a perceptual one, so it is
// used as it is (the same weights, without the compression). 2 and 3, a linear scRGB or a PQ colour: made linear light
// relative to the white (its Rec.709 or Rec.2020 luminance over whiteNits) and put through the CIE lightness curve, 0..1
// from black to white and above 1 for highlights. Equal steps of it look alike, so the match weighs a dark detail like a
// bright one.
// The match's thresholds (lambda, the confidence knee) are in these units, and 1, 2 and 3 reach white at 1.0, twice the
// old one's 0.5. Much of what the newer ones gain comes from that: the old luma doubled matches as well on a dark HDR
// pan, and mode 1 halved loses nearly all of the grain tally. Mode 1's gain on thin bright lines is its own (no
// compression).
#ifndef LUMA_MODE
#define LUMA_MODE 0
#endif

float LumaOf(float3 c)
{
    c = max(c, 0.0);

#if LUMA_MODE == 0
    const float l = dot(c, float3(0.299, 0.587, 0.114));
    return l / (1.0 + l);
#elif LUMA_MODE == 1
    return dot(min(c, 4.0), float3(0.299, 0.587, 0.114));
#else
#if LUMA_MODE == 2
    const float y = dot(c, float3(0.2126, 0.7152, 0.0722)) * (80.0 / whiteNits);
#else
    const float y = dot(PqToLinear(c), float3(0.2627, 0.6780, 0.0593)) * (10000.0 / whiteNits);
#endif
    const float yc = min(y, 64.0);
    return 0.01 * (yc <= 216.0 / 24389.0 ? yc * (24389.0 / 27.0) : 116.0 * pow(yc, 1.0 / 3.0) - 16.0);
#endif
}

)HLSL"
// clang-format on
