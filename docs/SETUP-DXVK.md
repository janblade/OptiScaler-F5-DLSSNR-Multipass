# Setup: OptiScaler with dxvk (DirectX 11 games on Vulkan)

dxvk turns a DirectX 11 game into a Vulkan one. OptiScaler can then run **Optical F5Low** on dxvk's Vulkan calls, the
same way as in a native Vulkan game: DLSS-NR, and frame generation through the Vulkan bridge. Tested with Batman: Arkham
Knight, a game without its own upscaler.

## 1. Install

1. Put **both** dxvk DLLs next to the game's exe: `d3d11.dll` **and** `dxgi.dll` (64-bit, same version). With only
   `d3d11.dll` the game closes at start.
2. Copy the OptiScaler files there too and rename `OptiScaler.dll` to **`winmm.dll`** (or `version.dll`).
   **Never `dxgi.dll`**: dxvk needs that name.
3. For DLSS-NR: put `nvngx_dlssnr.dll` and `nvngx.dll_dlssnr.dll` next to it (see [INSTALL-DLSSNR.md](../INSTALL-DLSSNR.md)).
   On AMD or Intel cards, use the vendor-neutral port instead.
4. Use the latest release.

## 2. Settings (`OptiScaler.ini`, before the game starts)

```ini
[FrameGen]
Enabled = true
FGInput = upscaler
FGOutput = xefg

[DlssNr]
Enabled = true
NativeDxvkVulkan = true
NativeMotion = true
NativeUpscaler = true
NativeDepthFinder = true
NativeInput = false
FinishedPicture = false
```

- **`NativeDxvkVulkan = true` is the key setting.** Without it OptiScaler treats the game as plain DirectX 11, and
  frame generation cannot work on dxvk.
- `FinishedPicture` must stay **false** (DirectX 12 only).
- NR without frame generation: pick **"NR only"** on the menu's **Optical F5Low** page, or set `[FrameGen] Enabled = false`.

## 3. Check it works

In `OptiScaler.log`:

- `CheckWorkingMode OptiScaler working as winmm.dll`: loaded under the right name.
- `hkvkCreateDevice` / `hkvkCreateSwapchainKHR`: OptiScaler sees dxvk's Vulkan side.
- Lines starting `Native motion` and `Virtual upscaler:`: Optical F5Low is running.

## Good to know

- **Close RTSS** when testing: its overlay hooks dxvk.
- **Two monitors:** if the window keeps resizing at start, that is dxvk itself; it happens without OptiScaler too.
- Low latency runs on the game's own thread by default (`NativeLowLatencyDxvkGameThread`); leave it on.
- Optical F5Low costs some GPU time every frame. Set the mode to **Off** when unused.
- Frame generation on dxvk is **experimental**; please report how it runs.

## Problems?

- **Closes at start, log shows `Dx11wDx12 swapchain creation failed`:** dxvk's `dxgi.dll` is missing. Add it and keep
  OptiScaler as `winmm.dll`.
- **Still crashes:** try once without OptiScaler. If it crashes then too, the cause is dxvk or the game.
- More: [TROUBLESHOOTING.md](../TROUBLESHOOTING.md).
