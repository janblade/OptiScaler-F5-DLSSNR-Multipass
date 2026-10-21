#pragma once

#include "SdrToHdr_ProxyBase.h"

#include <d3d11.h>

namespace SdrToHdr
{
// A Direct3D 11 game's conversion pass has to run between two things that both live inside MenuOverlayDx::Present: the
// motion step that reads and rewrites the game's picture (NativeMotionDx11::OnPresent, which must see the proxy and leave
// its result in it) and the menu, which draws on the real HDR buffer. LocalPresent arms the pass before it calls the menu
// overlay; the overlay runs it right after its motion step, and LocalPresent runs it after the overlay returns if nothing
// did (a menu path that stopped early draws nothing, so running it then is still before any menu pixel). Exactly once per
// present, on the presenting thread.
void ArmDx11Pass(ProxyChainBase* proxy, IDXGISwapChain3* real, ID3D11DeviceContext* context, const PassConstants& constants);

// Runs the armed pass, if one is armed. Logs a failure once per chain.
void RunPendingDx11Pass();
} // namespace SdrToHdr
