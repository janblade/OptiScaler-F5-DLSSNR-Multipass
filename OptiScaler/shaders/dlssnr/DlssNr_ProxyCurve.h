#pragma once

// The proxy curves: [DlssNr] ReversibleMode, dlssnr.hlsl's gReversibleMode and ProxyCurve. How the frame's light is
// mapped into the picture the model is shown, and (2, 4) whether its answer replaces the frame. The mode numbers, which
// of them the menu offers, when Linear bands, and Tune's damage thresholds per curve live here, pure, so
// tests/nr_proxy_curve_smoke.cpp checks them on the host.

#include <cmath>
#include <cstdint>
#include <dxgiformat.h>

namespace DlssNrProxyCurve
{
constexpr uint32_t kSoftKnee = 0;
constexpr uint32_t kNeutwoComposed = 1;
constexpr uint32_t kNeutwoReplace = 2;
constexpr uint32_t kBalancedComposed = 3;
constexpr uint32_t kBalancedReplace = 4;
constexpr uint32_t kHlg = 5;    // ITU-R BT.2100 HLG, white at 0.75 (ITU-R BT.2408)
constexpr uint32_t kPq = 6;     // SMPTE ST 2084, white (203 nits) at 0.58
// What game integrations hand the model: each channel's light as it is, clipped at white (dlssnr.hlsl SignalEncode).
// Set in the ini only, never offered in the menu.
constexpr uint32_t kLinear = 7;
constexpr uint32_t kCount = 8;
constexpr uint32_t kPickable = kLinear; // the menu offers 0 .. kPickable - 1

constexpr bool Valid(uint32_t mode) { return mode < kCount; }
constexpr bool IsReplace(uint32_t mode) { return mode == kNeutwoReplace || mode == kBalancedReplace; }
constexpr bool IsSignalCurve(uint32_t mode) { return mode == kHlg || mode == kPq || mode == kLinear; }

// Linear stores the light as it is, and the D3D12 proxy takes the colour's own format: in 8 or 10 bits a shadow sits
// on a handful of codes and bands. Float formats keep their precision relative, and the Vulkan proxy is always FP16.
constexpr bool LinearBandsIn(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return true;
    default:
        return false;
    }
}

// The value a signal curve stores for the peak channel of normalized light n (1 = the white point): dlssnr.hlsl's
// HlgSignal / PqSignal / linear, on the host.
inline float SignalOf(uint32_t mode, float n)
{
    n = std::fmax(n, 0.0f);

    if (mode == kHlg)
    {
        const float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
        const float e = n * 0.26496256f; // scene light at which the signal is 0.75
        const float signal = e <= 1.0f / 12.0f ? std::sqrt(3.0f * e) : a * std::log(12.0f * e - b) + c;
        return std::fmin(signal, 1.0f);
    }

    if (mode == kPq)
    {
        const float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 32.0f;
        const float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 128.0f, c3 = 2392.0f / 128.0f;
        const float y = std::pow(std::fmin(n * (203.0f / 10000.0f), 1.0f), m1);
        return std::pow((c1 + c2 * y) / (1.0f + c3 * y), m2);
    }

    return std::fmin(n, 1.0f);
}

// Tune's damage thresholds (dlssnr_detail_stats.hlsl): the share of the model's picture whose stored peak channel sits
// above the shoulder or below the floor.
//
// They were set on the soft knee, 0.95 and 0.02, which is 0.95x and 0.0015x the white point in the frame's own light.
// The signal curves store those same levels far lower -- PQ keeps white at 0.58 -- so read against 0.95 and 0.02 the
// penalty would only fire at 30x white and in near-black, and Tune would pick an exposure with nothing weighing
// against it. They take the thresholds at the knee's scene levels instead, so Tune weighs the same highlights and
// shadows on every curve and its answers compare. Neutwo and the hybrid keep 0.95 and 0.02, as they were tuned with.
struct TuneThresholds
{
    float shoulder;
    float floor;
};

constexpr float kKneeShoulder = 0.95f;
constexpr float kKneeFloor = 0.02f;
constexpr float kShoulderScene = 0.95526f;           // the soft knee stores 0.95 here (grey)
constexpr float kFloorScene = kKneeFloor / 12.92f;   // below the knee, sRGB's linear toe: 0.02 is 0.00155 of light

inline TuneThresholds Thresholds(uint32_t mode)
{
    if (!IsSignalCurve(mode))
        return { kKneeShoulder, kKneeFloor };

    return { SignalOf(mode, kShoulderScene), SignalOf(mode, kFloorScene) };
}
} // namespace DlssNrProxyCurve
