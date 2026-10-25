# Setup: OptiScaler with ReShade (DirectX 12)

OptiScaler loads ReShade itself, after its own hooks, so the two never fight over `dxgi.dll`.

## 1. Install

1. Install ReShade as usual: pick the game's exe and DirectX 10/11/12. The installer puts a `dxgi.dll` in the game folder.
   If OptiScaler is already your `dxgi.dll`, back it up first: the ReShade installer overwrites it.
2. In the game folder, rename ReShade's `dxgi.dll` to **`ReShade64.dll`**.
3. Make sure OptiScaler is still in the folder under its own name (`dxgi.dll`, `winmm.dll`, ...). Put your backup back
   if the installer replaced it.
4. In `OptiScaler.ini`, set:

   ```ini
   [Plugins]
   LoadReshade = true
   ```

5. Start the game. **Home** opens ReShade's menu, **Insert** opens OptiScaler's.

## 2. Check it works

- `OptiScaler.log` shows `Loading ReShade64.dll, result: ...` with a non-zero number.
- `ReShade.log` is a real log, not the "ReShade was not loaded" text.
- The OptiScaler menu's **Plugins** page lists ReShade as loaded (see [PLUGINS.md](PLUGINS.md)).

## Good to know

- ReShade turns off depth-based effects when it detects online play.
- With frame generation, keep the preset light: every effect costs GPU time on each real frame.
- With a Luma mod, OptiScaler creates the DirectX 12 device before loading ReShade (`CreateD3D12DeviceForLuma`).

## Undo

Set `LoadReshade = auto`, or rename `ReShade64.dll` back to `dxgi.dll` and remove OptiScaler.
