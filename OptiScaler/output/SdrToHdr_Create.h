#pragma once

#include "SdrToHdr_Common.h"
#include "SdrToHdr_Display.h"
#include "SdrToHdr_ProxyBase.h"
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
    bool windowed = true;              // not exclusive full screen
    DXGI_SWAP_EFFECT swapEffect = DXGI_SWAP_EFFECT_DISCARD; // as the game asked, after the sites' own edits
    UINT bufferCount = 2;
    UINT flags = 0;
    UINT sampleCount = 1;              // the desc's SampleDesc.Count
    bool frameGen = false;             // this is a frame generation library's own inner chain (_skipFGSwapChainCreation)
    bool composition = false;
    bool coreWindow = false;
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

    // An older (bitblt) chain made flip model for HDR output: the edit for the site to apply to its desc, and what the game
    // asked for, which the wrapper keeps reporting
    bool upgraded = false;
    DXGI_SWAP_EFFECT swapEffect = DXGI_SWAP_EFFECT_DISCARD; // the new desc
    UINT bufferCount = 0;
    UINT flags = 0;
    DXGI_SWAP_EFFECT gameSwapEffect = DXGI_SWAP_EFFECT_DISCARD; // the game's
    UINT gameBufferCount = 0;
    UINT gameFlags = 0;
};

// The desc edit a site puts back when the create with the HDR desc failed (before Unproxy); nothing when not upgraded.
void RestoreSwapDesc(const Plan& plan, DXGI_SWAP_EFFECT& swapEffect, UINT& bufferCount, UINT& flags);

// For a site that has just got a plan: applies the swap chain edit of an upgrade to its desc fields; nothing when not upgraded.
void ApplySwapDesc(const Plan& plan, DXGI_SWAP_EFFECT& swapEffect, UINT& bufferCount, UINT& flags);

// Decides for a swap chain about to be created. With [HDR] Enabled off it returns at once: the plan is unproxied, `format` is
// untouched, nothing is logged. Otherwise exactly one log line (ReasonText), and when the plan is proxied `format` is
// changed to the real HDR format.
Plan PrepareCreate(const CreateRequest& request, DXGI_FORMAT& format);

// While a proxied chain is being created, creation hooks nested inside that call (another layer of OptiScaler's own hooks
// seeing the HDR format the outer one asked for) pass through: the outer decision is the only one.
class ScopedProxiedCreate
{
  public:
    explicit ScopedProxiedCreate(bool on);
    ~ScopedProxiedCreate();
    ScopedProxiedCreate(const ScopedProxiedCreate&) = delete;
    ScopedProxiedCreate& operator=(const ScopedProxiedCreate&) = delete;

  private:
    bool _on;
};

// The create with the HDR format failed: puts the game's format back (the caller retries once) and logs it.
void Unproxy(Plan& plan, DXGI_FORMAT& format, HRESULT failure);

// For the sites that never proxy (Streamline's DLSS-G twins): logs the stand-down when HDR output is on.
void NoteStandDown(Reason reason);

// The last decision made for the game's swap chain, for the menu (thread safe).
StatusSnapshot Status();

// The chain being decided on is an older one made flip model for HDR output (set at creation, kept through resizes).
void RecordUpgraded(bool upgraded);

// Records a decision (creation, resize, a failed start). `enabled` is [HDR] Enabled when it was made.
void RecordStatus(Reason reason, bool active, bool hdr10, bool enabled, float displayMaxLuminance);

// Per present of a proxied chain: the game turned on its own Streamline DLSS-G since the chain was proxied. Logs once per
// chain and records a status note that HDR output stops at the next resolution / window change. It does not stop converting:
// the real chain is HDR, so a pass-through would show the game's SDR picture as scRGB. The next ResizeBuffers decides again
// and leaves it.
void CheckLateStandDown(ProxyChainBase& proxy);

// A frame generation swap chain was made for the game (it returns before any wrapper exists): HDR output stands aside.
// Logs and records it when HDR output is on.
void NoteFrameGenChain();

// What the environment says about frame generation, dxvk and Streamline DLSS-G right now (Decide inputs shared by the
// creation sites and every resize).
void FillEnvironment(Inputs& inputs);
} // namespace SdrToHdr
