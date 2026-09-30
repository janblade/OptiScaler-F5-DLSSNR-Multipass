#pragma once

// The Colour encoding override's last frame, for the menu and the log. Every path that runs the NR pass (D3D12
// evaluate, deferred SR, Finished Picture, Vulkan) reports its choice here once per frame; the menu reads it back.
// The choice itself is decided by shaders/dlssnr/DlssNr_ColourEncoding.h.

#include <shaders/dlssnr/DlssNr_ColourEncoding.h>

#include <string>

namespace DlssNr
{
// Render thread. Remembers the choice and, when it changed and a forced choice does not fit the buffer, logs the
// warning once. path names the caller in that line ("", "Vulkan", "finished picture"). Cheap when nothing changed:
// one lock and a comparison, no allocation.
void ReportColourEncoding(const DlssNrColourEncoding::Choice& choice, const char* formatName, const char* path);

struct ColourEncodingStatus
{
    bool seen = false;
    std::string line;    // DlssNrColourEncoding::Describe
    std::string warning; // DlssNrColourEncoding::MismatchWarning, empty when the choice fits
};

// Menu thread.
ColourEncodingStatus ReadColourEncodingStatus();
} // namespace DlssNr
