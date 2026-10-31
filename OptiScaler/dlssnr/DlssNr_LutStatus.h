#pragma once

// The LUT-apply pass's last frame (dlssnr-lut-apply epic, Story 4), for the menu's status line. Every
// backend that runs DispatchLut (currently D3D12 only; Vulkan joins in Story 3) reports here once per
// frame, whether a LUT was wanted, loaded, or failed; the menu reads it back. Mirrors
// DlssNr_ColourEncodingStatus.h's split (report from the render thread, read from the menu).

#include <string>

namespace DlssNr
{
// Render thread. Cheap when nothing changed: one lock and a comparison, no allocation.
void ReportLutStatus(bool wanted, const std::string& loadedPath, int size, bool failed, const std::string& error,
                     const std::string& attemptedPath, float strength);

struct LutStatus
{
    bool seen = false;   // at least one frame has reported
    bool wanted = false; // LutFile was non-empty on the last reporting frame
    bool loaded = false; // a LUT is actually active (wanted and parsed successfully at some point)
    std::string loadedPath;
    int size = 0;
    bool failed = false; // attemptedPath (below) failed to parse -- independent of loaded/loadedPath,
                         // same contract as DlssNr_LutState
    std::string error;
    std::string attemptedPath;
    float strength = 1.0f;
};

// The post-NR LUT stack's last frame, for its own card. Same split as above.
struct PostLutStatus
{
    bool seen = false;   // at least one frame has reported
    bool wanted = false; // the stack held at least one file
    bool loaded = false; // a chain is active
    std::string chain;   // DlssNr_DescribeChain
    std::string failedPath;
    std::string error;
    std::string note; // why it is not applied when it is wanted (a mode that does not run it)
};

// Render thread. A frame where the stack is empty reports that too, so the card never shows a stale chain.
void ReportPostLutStatus(const PostLutStatus& status);

// Menu thread.
LutStatus ReadLutStatus();
PostLutStatus ReadPostLutStatus();
} // namespace DlssNr
