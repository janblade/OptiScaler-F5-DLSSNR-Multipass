#pragma once

// What the HDR output menu section says about the swap chain right now: the last decision made for it, and the pure function
// that turns that and the settings into the status line. No Direct3D, so tests/sdr_to_hdr_rules_smoke.cpp builds it alone.
#include "SdrToHdr_Common.h"

#include <cstdio>
#include <string>

namespace SdrToHdr
{
// The last decision made for the game's swap chain (at creation, at every resize). Written by the swap chain code, read by
// the menu through SdrToHdr::Status() (output/SdrToHdr_Create.h).
struct StatusSnapshot
{
    bool decided = false;           // a swap chain has been decided on at all
    bool enabledAtDecision = false; // [HDR] Enabled when it was decided: what the chain did
    Reason reason = Reason::Off;
    bool active = false;            // the chain is proxied and converted
    bool hdr10 = false;             // the signal the active chain was made with
    float displayMaxLuminance = 0.0f;
};

enum class StatusColour
{
    None,  // no line
    Amber, // not (yet) doing what was asked
    Green, // active
};

struct StatusLineResult
{
    std::string text;
    StatusColour colour = StatusColour::None;
    std::string signalHint; // a second line when the signal setting differs from the active chain's; empty otherwise
};

// The settings the line mentions, read live from the config by the menu.
struct StatusSettings
{
    bool enabled = false;       // [HDR] Enabled now
    bool hdr10 = false;         // [HDR] UseHDR10 now
    float gameNits = 203.0f;    // clamped
    float configuredPeak = 0.0f; // [HDR] PeakNits (0 = the display's)
};

inline const char* AppliesLaterText()
{
    return "HDR output: applies at the next start or resolution / window change";
}

// Off: no line. A change of [HDR] Enabled the chain has not picked up yet: amber, in both directions. Active: green with the
// signal, white and peak (and where the peak comes from). Every stand-down: amber, with ReasonText's wording.
inline StatusLineResult StatusLine(const StatusSnapshot& status, const StatusSettings& settings)
{
    StatusLineResult result;
    const bool didEnabled = status.decided && status.enabledAtDecision;

    if (settings.enabled != didEnabled)
    {
        result.text = AppliesLaterText();
        result.colour = StatusColour::Amber;
        return result;
    }

    if (!settings.enabled)
        return result;

    if (!status.active)
    {
        result.text = ReasonText(status.reason);
        result.colour = StatusColour::Amber;
        return result;
    }

    const float peak = ResolvePeakNits(settings.configuredPeak, status.displayMaxLuminance);
    const char* source = settings.configuredPeak > 0.0f      ? "ini"
                         : status.displayMaxLuminance > 0.0f ? "display"
                                                             : "default";
    char line[160];
    std::snprintf(line, sizeof(line), "HDR output: active, %s, game white %.0f nits, peak %.0f nits (%s)",
                  status.hdr10 ? "HDR10" : "scRGB", ClampGameNits(settings.gameNits), peak, source);
    result.text = line;
    result.colour = StatusColour::Green;

    if (settings.hdr10 != status.hdr10)
        result.signalHint = std::string("The signal changes to ") + (settings.hdr10 ? "HDR10" : "scRGB") +
                            " at the next resolution / window change";

    return result;
}
} // namespace SdrToHdr
