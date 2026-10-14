#pragma once

// The exposure scan's recording: every candidate's value next to what is known about the frame's brightness, written
// as CSV beside the log while the scan runs ([DlssNr] ScanExposure). It is the data the matcher is built and tested
// on (plan 2026-10-14-exposure-scan-all-apis, story 2): which candidate follows the scene's light, in what units, and
// whether the game's exposure is already in the picture.
//
// One row per kEveryFrames NR frames:
//   seconds,frame,meter,game,pre,c1,c2,...
// - meter: Automatic exposure's raw reading of the frame (0 unless Automatic is the white point source, the only time
//   it runs). Bigger = a darker frame (it is an exposure: what the frame is multiplied by).
// - game: the exposure the game handed to the upscaler (0 when it handed none, or when the source neither reads it
//   nor pairs it).
// - pre: the game's DLSS.Pre.Exposure (1 when it sent none).
// - cN: candidate N's latest value (0 before its first read).
// A line starting with '#' names the candidates whenever the list changes; "(handed)" marks the one that is the game's
// own exposure texture, the known answer the matcher is checked against.
//
// Header-only, no D3D types: tests/nr_exposure_scan_record_smoke.cpp.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace DlssNrExposureScanRecord
{
// About 10 rows a second at 60 fps: enough to follow eye adaptation, small enough to leave on for a session
inline constexpr uint64_t kEveryFrames = 6;

inline bool Due(uint64_t frame) { return frame % kEveryFrames == 0; }

struct Named
{
    std::string shape;
    bool handed = false;
};

struct Sample
{
    double seconds = 0.0;
    uint64_t frame = 0;
    float meter = 0.0f;
    float game = 0.0f;
    float pre = 1.0f;
    std::vector<float> values; // one per candidate, in list order
};

inline std::string NamesLine(const std::vector<Named>& names)
{
    std::string out = "# candidates:";

    for (size_t i = 0; i < names.size(); ++i)
    {
        out += (i == 0 ? " " : "; ") + std::to_string(i + 1) + "=" + names[i].shape;

        if (names[i].handed)
            out += " (handed)";
    }

    return out;
}

inline std::string ColumnsLine(size_t candidates)
{
    std::string out = "seconds,frame,meter,game,pre";

    for (size_t i = 0; i < candidates; ++i)
        out += ",c" + std::to_string(i + 1);

    return out;
}

inline std::string Row(const Sample& s)
{
    char buf[48];
    std::string out;
    out.reserve(32 + s.values.size() * 12);

    snprintf(buf, sizeof(buf), "%.3f,%llu", s.seconds, (unsigned long long) s.frame);
    out += buf;

    for (float v : { s.meter, s.game, s.pre })
    {
        snprintf(buf, sizeof(buf), ",%.6g", v);
        out += buf;
    }

    for (float v : s.values)
    {
        snprintf(buf, sizeof(buf), ",%.6g", v);
        out += buf;
    }

    return out;
}
} // namespace DlssNrExposureScanRecord
