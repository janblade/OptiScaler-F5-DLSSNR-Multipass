# Setup: OptiScaler with DirectX 11 games without DLSS (64-bit)

Many DirectX 11 games make **no DLSS, FSR or XeSS call** of their own. For those, OptiScaler uses **Optical F5Low**, its
own motion estimate from the finished picture. That is what lets DLSS-NR and frame generation work there. Frame generation
replaces the game's DirectX 11 swap chain with a DirectX 12 one (the D3D11-to-D3D12 bridge).

- **64-bit games only.** OptiScaler cannot load into a 32-bit game.
- A DirectX 11 game running on **dxvk** (dxvk's `d3d11.dll` in the game folder) needs [SETUP-DXVK.md](SETUP-DXVK.md) instead.
- A DirectX 11 game **with** its own DLSS, FSR or XeSS: set `[Upscalers] Dx11Upscaler = dlss_12` for DLSS-NR (see
  [TROUBLESHOOTING.md](../TROUBLESHOOTING.md), "Game with its own upscaler, DirectX 11").

## 1. Install

1. Copy the OptiScaler files next to the game's exe and rename `OptiScaler.dll` to **`dxgi.dll`**. If the game already
   has a `dxgi.dll` (another mod), use `winmm.dll` or `version.dll` instead.
2. For DLSS-NR: put `nvngx_dlssnr.dll` and `nvngx.dll_dlssnr.dll` next to it (see [INSTALL-DLSSNR.md](../INSTALL-DLSSNR.md)).
   On AMD or Intel cards, use the vendor-neutral port instead.
3. Run the game in **borderless or windowed** mode.

## 2. Settings

**Easiest:** start the game, press **Insert**, open the **Optical F5Low** page and pick a mode:

- **"NR + frame generation"**: DLSS-NR and frame generation. Then set the frame generation output to **XeFG**, save, and
  **restart** the game.
- **"NR only"**: DLSS-NR without frame generation (lower GPU cost). It runs on the finished picture: if the menu asks
  for it, set **NR Pass at: Finished Picture** under NR Options.

**Or in `OptiScaler.ini`** (NR + frame generation):

```ini
[FrameGen]
Enabled = true
FGInput = upscaler
FGOutput = xefg

[DlssNr]
Enabled = true
NativeMotion = true
NativeUpscaler = true
NativeDepthFinder = true
NativeInput = false
FinishedPicture = false
```

- With frame generation on, **only `NativeUpscaler` works** in a DirectX 11 game: `NativeInput` ("NR only") and
  `FinishedPicture` do nothing once frame generation replaces the swap chain.
- Keep `FinishedPicture = false` in this mode (choosing the mode on the menu does that for you).
- The game's upscaling is not touched (render size = output size). Here OptiScaler's upscaler only steadies the picture
  and feeds frame generation; it follows `[Upscalers] Dx12Upscaler` (FSR when unset).
- Frame generation settings must be set **before** the game starts.

## 3. Check it works

In `OptiScaler.log`:

- `Failed to get ID3D12CommandQueue from pDevice, creating Dx11 swapchain!`: the game is DirectX 11.
- Lines starting `Native motion` and `Virtual upscaler:`: Optical F5Low is running.

## Good to know

- Depth is found automatically in most games. Until one qualifies (menus, loading screens) motion comes from the picture
  alone; "no snapshot" warnings there are normal.
- Optical F5Low costs some GPU time every frame. Set the mode to **Off** when you use neither NR nor frame generation.
- Alt+Enter or a resolution change with XeFG can write error lines like `Preventing flag change for XeFG!`. They are
  harmless if the game keeps running.

## Problems?

- **`Frame count jumped too much` on every other frame and no generated frames:** a known open issue on this path. Send a
  log made with `[Log] LogLevel = 1`, and say whether the FPS overlay shows generated frames.
- **Black blocky areas with frame generation on:** test once each with `NativeMotion = false` and
  `NativeDepthFinder = false`, and report which one helps.
- More: [TROUBLESHOOTING.md](../TROUBLESHOOTING.md).
