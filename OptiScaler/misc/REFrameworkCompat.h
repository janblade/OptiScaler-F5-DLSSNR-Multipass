#pragma once

// OptiScaler next to the REFramework fork that draws on XeFG's swapchain (github.com/onehoon/REFramework, dinput8.dll)
// in Capcom's RE Engine games. With [Hotfix] REFrameworkCompat on:
// - XeFG's swapchain is never torn down under REFramework: it is asked to let go first (XeFG_Dx12::ReleaseSwapchain);
// - OptiScaler releases only its own references when the FG swapchain goes (FGHooks::hkFGRelease), not REFramework's;
// - the wrapped swapchain's final release cannot run twice at once when REFramework's letting go calls back into it;
// - the old overlay menu quirk is not applied (it switches frame generation off) and the menu key defaults to Home
//   (REFramework uses Insert).
// Off, every one of those paths is what it was before. misc/REFrameworkCompatCore.h has the parts tested alone.

#include <windows.h>

namespace REFrameworkCompat
{

// While the quirks are applied at startup (dllmain.cpp): decides the switch from the ini, the game's quirk and the
// dinput8.dll beside the exe, and logs why.
void Init(bool quirk);

// The switch is on. In auto, a game with the quirk whose REFramework was not found at startup is checked again when
// asked (at most every few seconds; only teardown paths ask), so a REFramework loaded under another name still counts.
bool Active();

// Before XeFG's swapchain is torn down, outside frame generation's lock: REFramework lets go of it. Never stops the
// teardown (a "blocked" answer is logged), and does nothing while the process is shutting down or when no REFramework
// that follows XeFG is loaded.
void BeforeXeFGSwapchainRetire(IUnknown* publicProxy, void* xefgContext, HWND hwnd);

} // namespace REFrameworkCompat
