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
    // What the GAME asked for, before any site edit (V-Sync override, borderless): a chain the game asked to be bitblt is an
    // "upgraded" one (it keeps being told so) even when an earlier edit already made it flip model
    bool hasOriginal = false;
    DXGI_SWAP_EFFECT originalSwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    UINT originalBufferCount = 2;
    UINT originalFlags = 0;
    bool frameGen = false;             // this is a frame generation library's own real chain (_skipFGSwapChainCreation)
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
    // The frame generation library's own chain (CreateRequest::frameGen): the wrapper keeps asking for it at every resize
    bool fgInner = false;
    // ... and the library asked for exclusive full screen: an HDR chain is made windowed instead (a real exclusive HDR chain was
    // refused by DXGI, or never came back from the library's destroy), and the wrapper keeps the window borderless when the
    // library calls SetFullscreenState(TRUE)
    bool exclusiveToWindowed = false;
    bool displayKnown = false;

    // An older (bitblt) chain made flip model for HDR output: the edit for the site to apply to its desc, and what the game
    // asked for, which the wrapper keeps reporting
    bool upgraded = false;   // the game asked for bitblt: the wrapper reports the game's values and the chain keeps its proxy
    bool editDesc = false;   // ... and this plan edits the desc (false when an earlier edit, the V-Sync override, made it flip)
    DXGI_SWAP_EFFECT swapEffect = DXGI_SWAP_EFFECT_DISCARD; // the new desc
    UINT bufferCount = 0;
    UINT flags = 0;
    DXGI_SWAP_EFFECT restoreSwapEffect = DXGI_SWAP_EFFECT_DISCARD; // the desc as the site had it before the edit
    UINT restoreBufferCount = 0;
    UINT restoreFlags = 0;
    DXGI_SWAP_EFFECT gameSwapEffect = DXGI_SWAP_EFFECT_DISCARD; // what the GAME asked for
    UINT gameBufferCount = 0;
    UINT gameFlags = 0;
};

// Fills the upgrade part of a plan from the request and Decide's verdict (host-testable: no Config, no logging).
inline void MakeUpgradePlan(const CreateRequest& request, bool decisionUpgrade, Plan& plan)
{
    const DXGI_SWAP_EFFECT original = request.hasOriginal ? request.originalSwapEffect : request.swapEffect;
    const UINT originalCount = request.hasOriginal ? request.originalBufferCount : request.bufferCount;
    const UINT originalFlags = request.hasOriginal ? request.originalFlags : request.flags;
    const bool askedBitBlt = original == DXGI_SWAP_EFFECT_DISCARD || original == DXGI_SWAP_EFFECT_SEQUENTIAL;

    if (decisionUpgrade)
    {
        const SwapDescEdit edit = UpgradeSwapDesc(request.swapEffect, request.bufferCount, request.flags);
        plan.upgraded = true;
        plan.editDesc = true;
        plan.restoreSwapEffect = request.swapEffect;
        plan.restoreBufferCount = request.bufferCount;
        plan.restoreFlags = request.flags;
        plan.gameSwapEffect = original;
        plan.gameBufferCount = originalCount;
        plan.gameFlags = originalFlags;
        plan.swapEffect = edit.swapEffect;
        plan.bufferCount = edit.bufferCount;
        plan.flags = edit.flags;
    }
    else if (askedBitBlt && request.flipModel && request.windowed && request.sampleCount <= 1)
    {
        // An earlier site edit (the V-Sync override) already made it flip model: nothing to edit, but it is an upgraded chain
        // all the same -- the wrapper tells the game what it asked for and keeps the proxy
        plan.upgraded = true;
        plan.editDesc = false;
        plan.gameSwapEffect = original;
        plan.gameBufferCount = originalCount;
        plan.gameFlags = originalFlags;
    }
}

// For a site that has just got a plan: the library's exclusive full screen request becomes a windowed chain
inline void ApplyWindowed(const Plan& plan, BOOL& windowed)
{
    if (plan.exclusiveToWindowed)
        windowed = TRUE;
}

// ... and for the HWND variants: whether the fullscreen desc is passed on (it is not when the chain is made windowed)
inline bool KeepFullscreenDesc(const Plan& plan, bool hasDesc) { return hasDesc && !plan.exclusiveToWindowed; }

// For a site that has just got a plan: applies the swap chain edit of an upgrade to its desc fields; nothing otherwise.
inline void ApplySwapDesc(const Plan& plan, DXGI_SWAP_EFFECT& swapEffect, UINT& bufferCount, UINT& flags)
{
    if (!plan.editDesc)
        return;

    swapEffect = plan.swapEffect;
    bufferCount = plan.bufferCount;
    flags = plan.flags;
}

// The desc edit a site puts back when the create with the HDR desc failed (before Unproxy): the desc exactly as the site had it.
inline void RestoreSwapDesc(const Plan& plan, DXGI_SWAP_EFFECT& swapEffect, UINT& bufferCount, UINT& flags)
{
    if (!plan.editDesc)
        return;

    swapEffect = plan.restoreSwapEffect;
    bufferCount = plan.restoreBufferCount;
    flags = plan.restoreFlags;
}

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

// A note over the active line (Status::endsAtResize): something happened that this chain keeps converting through.
void RecordNote(Reason note);

// Records a decision (creation, resize, a failed start). `enabled` is [HDR] Enabled when it was made.
void RecordStatus(Reason reason, bool active, bool hdr10, bool enabled, float displayMaxLuminance);

// Per present of a proxied chain: the game turned on its own Streamline DLSS-G since the chain was proxied. Logs once per
// chain and records a status note that HDR output stops at the next resolution / window change (at the next start, for a chain
// made flip model for HDR output: `upgraded`). It does not stop converting: the real chain is HDR, so a pass-through would
// show the game's SDR picture as scRGB. The next ResizeBuffers decides again and leaves it.
void CheckLateStandDown(ProxyChainBase& proxy, bool upgraded);

// A frame generation swap chain was made for the game (it returns before any wrapper exists): HDR output stands aside.
// Logs and records it when HDR output is on.
void NoteFrameGenChain();

// What the environment says about frame generation, dxvk and Streamline DLSS-G right now (Decide inputs shared by the
// creation sites and every resize).
void FillEnvironment(Inputs& inputs);
} // namespace SdrToHdr
