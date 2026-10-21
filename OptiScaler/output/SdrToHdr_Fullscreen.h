#pragma once

// Win32 only (no OptiScaler headers), so tests/sdr_to_hdr_dx11_gpu.cpp builds it alone.
#include <windows.h>

#include <atomic>

namespace SdrToHdr
{
// Exclusive full screen kept borderless, for a swap chain that was made flip model for HDR output (an older, bitblt one).
//
// A flip model chain that goes into exclusive full screen is invalid until the game resizes its buffers (Present returns
// DXGI_ERROR_INVALID_CALL for good; read from DXGI on a hardware adapter, tests/sdr_to_hdr_dx11_gpu.cpp), and a bitblt-era game
// does not resize after Alt+Enter. So such a chain never enters it: DXGI's own switch is switched off (MakeWindowAssociation
// NO_ALT_ENTER) and the game's SetFullscreenState(TRUE) is answered here with a borderless window over the monitor, like
// FGHooks::hkSetFullscreenState with FGXeFGForceBorderless (hooks/FG_Hooks.cpp) -- the same popup style, applied on the calling
// thread, as DXGI's own switch does. SetFullscreenState(FALSE) puts the window's style and placement back.
//
// The window changes send messages to the game's window procedure, which may call back into Enter / Leave from inside them: such
// a call during a transition does nothing and returns true (the outer call finishes the transition). Active is atomic, for the
// getters that run on other threads; the window itself is only changed by the thread that calls Enter / Leave.
class BorderlessFullscreen
{
  public:
    // Covers `monitor` (full monitor rectangle) with a popup window. Remembers the style and the window placement (so a minimised
    // or maximised window comes back as it was). Returns false, changing nothing, for an empty rectangle or a window that is gone.
    // True when entered, and when already entered or in a transition.
    bool Enter(HWND window, const RECT& monitor);

    // Puts the window as it was before Enter. True when left, when not entered and when in a transition.
    bool Leave(HWND window);

    bool Active() const { return _active.load(std::memory_order_acquire); }

  private:
    std::atomic<bool> _active { false };
    std::atomic<bool> _transition { false };
    LONG_PTR _style = 0;
    LONG_PTR _exStyle = 0;
    WINDOWPLACEMENT _placement {};
};
} // namespace SdrToHdr
