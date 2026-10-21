#pragma once

// Win32 only (no OptiScaler headers), so tests/sdr_to_hdr_dx11_gpu.cpp builds it alone.
#include <windows.h>

namespace SdrToHdr
{
// Exclusive full screen kept borderless, for a swap chain that was made flip model for HDR output (an older, bitblt one).
//
// A flip model chain that goes into exclusive full screen is invalid until the game resizes its buffers (Present returns
// DXGI_ERROR_INVALID_CALL for good; read from DXGI on a hardware adapter, tests/sdr_to_hdr_dx11_gpu.cpp), and a bitblt-era game
// does not resize after Alt+Enter. So such a chain never enters it: DXGI's own switch is switched off (MakeWindowAssociation
// NO_ALT_ENTER) and the game's SetFullscreenState(TRUE) is answered here with a borderless window over the monitor, like
// FGHooks::hkSetFullscreenState with FGXeFGForceBorderless (hooks/FG_Hooks.cpp) -- the same popup style, applied on the calling
// thread, as DXGI's own switch does. SetFullscreenState(FALSE) puts the window's style and rectangle back exactly.
class BorderlessFullscreen
{
  public:
    // Covers `monitor` (full monitor rectangle) with a popup window. Remembers the style and rectangle it replaces. Does nothing
    // (returns true) when already entered.
    bool Enter(HWND window, const RECT& monitor);

    // Puts the window as it was before Enter. Does nothing (returns true) when not entered.
    bool Leave(HWND window);

    bool Active() const { return _active; }

  private:
    bool _active = false;
    LONG_PTR _style = 0;
    LONG_PTR _exStyle = 0;
    RECT _rect {};
};
} // namespace SdrToHdr
