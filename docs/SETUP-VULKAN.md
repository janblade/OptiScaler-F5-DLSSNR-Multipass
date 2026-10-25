# Setup: OptiScaler with Vulkan games and emulators (example: shadPS4)

Many Vulkan programs, emulators such as shadPS4 among them, make **no DLSS, FSR or XeSS call** of their own. For those,
OptiScaler uses **Optical F5Low**, its own motion estimate from the finished picture. That is what lets DLSS-NR and frame
generation work there. Frame generation then presents through OptiScaler's Vulkan bridge.

A Vulkan game that **does** call its own upscaler needs the Vulkan route "Bridge, game's upscaler" instead; see
[TROUBLESHOOTING.md](../TROUBLESHOOTING.md) ("Game with its own upscaler, Vulkan").

## 1. Install

1. Use the latest release.
2. Copy the OptiScaler files into the folder that holds **shadPS4.exe**, the emulator itself (not a separate launcher).
3. Rename `OptiScaler.dll` to **`winmm.dll`** (`version.dll` also works). Vulkan programs don't load `dxgi.dll`.
4. For DLSS-NR: put `nvngx_dlssnr.dll` and `nvngx.dll_dlssnr.dll` next to it (see [INSTALL-DLSSNR.md](../INSTALL-DLSSNR.md)).
   On AMD or Intel cards, use the vendor-neutral port instead.

## 2. Settings

**Easiest:** start a game, press **Insert**, open the **Optical F5Low** page and pick **"NR + frame generation"**. Then
set the frame generation output to **XeFG**, save, and **restart** the emulator.

**Or in `OptiScaler.ini`:**

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

- `FinishedPicture` **must stay false** in Vulkan. With true, DLSS-NR never starts ("Waiting for a finished picture.").
- Don't copy `NativeFrameGenerationOnly = true` from another game's ini: it is for Vulkan games that call their own upscaler.
- Frame generation settings must be set **before** the program starts.

## 3. Check it works

In `OptiScaler.log` (next to the exe):

- `CheckWorkingMode OptiScaler working as winmm.dll`: OptiScaler loaded.
- `hkvkCreateDevice` / `hkvkCreateSwapchainKHR`: it runs as Vulkan.
- Lines starting `Native motion` and `Virtual upscaler:`: Optical F5Low is running.

## Good to know

- **"No snapshot" warnings are normal.** Many emulators never give a usable depth buffer; motion then comes from the
  picture alone.
- **Turn off the program's own V-Sync** if frame generation stutters.
- Optical F5Low costs some GPU time every frame. Set the mode to **Off** when you use neither NR nor frame generation.
- 30 fps PS4 games: frame generation at 2x gives 60 fps on screen. The game itself still runs at 30.

## Problems?

- **Closes at start:** look for `vkCreateDevice returned -3` in the log, and see "A Vulkan game or emulator closes at
  start" in [TROUBLESHOOTING.md](../TROUBLESHOOTING.md). Send the log and your GPU.
- **Black blocky areas with frame generation on:** test once each with `NativeMotion = false` and
  `NativeDepthFinder = false`, and report which one helps.
- More: [TROUBLESHOOTING.md](../TROUBLESHOOTING.md).
