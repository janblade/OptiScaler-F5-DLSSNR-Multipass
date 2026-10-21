#pragma once

#include "SdrToHdr_Common.h"
#include "SdrToHdr_Display.h"
#include "SdrToHdr_Status.h"

#include <dxgi.h>

namespace SdrToHdr
{
// What the swap chain creation sites know about the chain they are about to make. Filled after the site's own desc edits
// (borderless, OverrideVsync), so flipModel is the final swap effect.
struct CreateRequest
{
    IUnknown* device = nullptr;        // the queue (D3D12) or the device (D3D11) the game passed
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool flipModel = false;
    bool frameGen = false;             // this is a frame generation library's own inner chain (_skipFGSwapChainCreation)
    bool composition = false;
    bool multiNode = false;
    HWND hwnd = nullptr;               // for the display the window is on (null: unknown)
    IDXGIFactory* factory = nullptr;   // for finding that display's output
};

// What was decided for one swap chain, handed on to the wrapper.
struct Plan
{
    bool proxied = false;
    Reason reason = Reason::Off;
    DXGI_FORMAT gameFormat = DXGI_FORMAT_UNKNOWN; // what the game asked for
    DXGI_FORMAT realFormat = DXGI_FORMAT_UNKNOWN; // what the chain is made as when proxied
    bool hdr10 = false;
    DisplayInfo display;
    bool displayKnown = false;
};

// Decides for a swap chain about to be created. With [HDR] Enabled off it returns at once: the plan is unproxied, `format` is
// untouched, nothing is logged. Otherwise exactly one log line (ReasonText), and when the plan is proxied `format` is
// changed to the real HDR format.
Plan PrepareCreate(const CreateRequest& request, DXGI_FORMAT& format);

// The create with the HDR format failed: puts the game's format back (the caller retries once) and logs it.
void Unproxy(Plan& plan, DXGI_FORMAT& format, HRESULT failure);

// For the sites that never proxy (Streamline's DLSS-G twins): logs the stand-down when HDR output is on.
void NoteStandDown(Reason reason);

// The last decision made for the game's swap chain, for the menu (thread safe).
StatusSnapshot Status();

// Records a decision (creation, resize, a failed start). `enabled` is [HDR] Enabled when it was made.
void RecordStatus(Reason reason, bool active, bool hdr10, bool enabled, float displayMaxLuminance);

// A frame generation swap chain was made for the game (it returns before any wrapper exists): HDR output stands aside.
// Logs and records it when HDR output is on.
void NoteFrameGenChain();

// What the environment says about frame generation, dxvk and Streamline DLSS-G right now (Decide inputs shared by the
// creation sites and every resize).
void FillEnvironment(Inputs& inputs);
} // namespace SdrToHdr
