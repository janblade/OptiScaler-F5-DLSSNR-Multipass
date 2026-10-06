# Install DLSS Neural Rendering

This fork is experimental. Do not use injection mods in anti-cheat-protected multiplayer games.

## Requirements

- A 64-bit game whose temporal upscaler reaches an OptiScaler D3D12 path. Native D3D12 is preferred;
  supported D3D11 and Vulkan games can use OptiScaler's D3D12 bridges.
- NVIDIA driver 616.56 or newer.
- The complete release archive from this repository. It includes `setup_windows.bat`, the
  `OptiScaler` backend folder, `OptiScaler.dll`, `OptiScaler.ini`, and `nvngx.dll_dlssnr.dll`.
- A separately obtained `nvngx_dlssnr.dll` 310.8 runtime appropriate for the GPU.

The two similarly named files are different and both are required:

| File | Purpose |
|---|---|
| `nvngx.dll_dlssnr.dll` | Open-source forwarder supplied by this project |
| `nvngx_dlssnr.dll` | NVIDIA-derived Neural Rendering runtime supplied separately by the user |

## Run without an NVIDIA GPU

The steps above assume the `nvngx.dll_dlssnr.dll` forwarder in this release, which loads the
NVIDIA driver's NGX core. On AMD and Intel GPUs that core cannot start, so Neural Rendering stays
off.

A separate, vendor-neutral build of `nvngx.dll_dlssnr.dll` runs the same model directly in D3D12
compute and DirectML instead of going through NGX, so it needs no NVIDIA driver support. This
fork's own source does not include it — it is built by a separate toolchain outside this
repository — but some release zips include a prebuilt copy under `Optional\` for convenience; check
whether yours does before looking elsewhere. Either way, drop it in as a same-named replacement for
the forwarder:

| File | Purpose |
|---|---|
| `nvngx.dll_dlssnr.dll` | Replace with the vendor-neutral port build (from `Optional\` if your release includes it) instead of this project's NGX forwarder |
| `nvngx_dlssnr.dll` | Unchanged — still required. The port reads its weights from this same file, so the correct runtime for the GPU generation is still needed from the table below. |
| `nr_port.ini` | Copy alongside the port DLL if `Optional\` includes one. Configures the port runtime itself (fp16/DirectML acceleration, how often the coarsest network stage is recomputed) and is read from the same folder the DLL sits in. The DLL still runs without it, just on slower unoptimised defaults. |

Everything else in this guide (install steps, INI keys, game notes) is unchanged. To confirm the
port is loaded, open the `Insert` overlay's Neural Rendering menu: the status line reads
**Model backend: vendor-neutral port** instead of **NVIDIA NGX**.

This path has only been exercised on an NVIDIA GPU with the port DLL forced in, not on real AMD
or Intel hardware. If NR does not start, or the status line still says **NVIDIA NGX**, check that
the file actually loaded is the port build and not the forwarder.

## Choose the correct runtime

| GPU | Runtime | SHA-256 |
|---|---|---|
| RTX 50 | Original NVIDIA-signed 310.8 | `E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E` |
| RTX 20 / 30 / 40 | ShortFuse cross-generation 310.8 | `E67DEE209320CDAFE0E93E45675D7AA34323A53ACC57A72B2E40A181581C989A` |

For RTX 20/30/40, obtain the compatibility runtime from
[ShortFuse's pinned RenoDX thread](https://discord.com/channels/1408098019194310818/1543976771920330884).
The compatibility DLL automatically selects an FP16-oriented path on RTX 20/30, an Ada-compatible
path on RTX 40, and leaves the RTX 50 path unchanged.

The compatibility runtime is modified, so Windows reports the original NVIDIA signature as invalid.
That is expected for this exact hash, but it removes the assurance provided by Authenticode. Keep
security protection enabled, use only the pinned developer attachment, and verify the SHA-256 value:

```powershell
Get-FileHash .\nvngx_dlssnr.dll -Algorithm SHA256
```

## Install

1. Close the game and its launcher.
2. Find the directory containing the real game executable, which is often below the game's root.
3. Back up any existing proxy DLL, `OptiScaler.ini`, and OptiScaler installation.
4. Extract the entire release archive into that executable directory. Do not copy only the two DLLs.
5. Put the correct `nvngx_dlssnr.dll` from the table above in the same directory.
6. Run `setup_windows.bat`. It renames `OptiScaler.dll` to a proxy filename the game will load and
   creates an uninstaller. `dxgi.dll` is the usual first choice. The validated Cyberpunk 2077 setup
   used `dbghelp.dll` to coexist with its existing loaders.
7. Enable Neural Rendering in the `Insert` overlay, or edit `OptiScaler.ini`:

```ini
[DlssNr]
Enabled=true
RunBeforeSR=true
Passes=1
WorkingScale=1.0
```

Begin with one pass. RTX 20/30 use a much heavier FP16 path, so reduced model resolution may be
necessary. With `RunBeforeSR=true`, DLSS Performance at 3840x2160 gives the model a 1920x1080 input
before Super Resolution. `WorkingScale=0.5` lowers only the model's work resolution further.

For a portable setup, leave the process filter disabled:

```ini
[ProcessFilter]
TargetProcessName=auto
```

Do not copy an INI containing another game's executable name. A mismatch intentionally puts
OptiScaler into pass-through mode, which means no menu and no Neural Rendering.

## If antivirus flags the download

Windows Defender has flagged this package. On 2026-10-01 a build was blocked as
`Trojan:Win32/Tecabans.ST!cl`, and about four hours later the same bytes scanned clean with no local
change and no action taken. Local definitions never moved, so the correction happened in Microsoft's
cloud, which is what the `!cl` suffix means: the verdict is delivered per-lookup and can be withdrawn
the same way.

Two things follow, and the first is the useful one.

**Waiting is often the right response.** If the verdict ends in `!cl` or `!ml`, it can disappear on its
own. Check what you actually have before changing any setting:

```powershell
Get-MpThreatDetection | Select-Object -Last 3 | Format-List InitialDetectionTime, ThreatID, Resources
Get-MpThreat | Select-Object ThreatID, ThreatName, IsActive
```

**Verify the bytes, rather than trusting the file.** Every release ships `SHA256SUMS.txt`, and the
release notes carry the archive's own hash. Compare both:

```powershell
Get-FileHash -Algorithm SHA256 .\OptiScaler-DLSSNR-F5-<tag>.zip
Get-ChildItem -Recurse -File | Where-Object Name -ne SHA256SUMS.txt |
    Get-FileHash -Algorithm SHA256 | Format-Table Hash, Path
```

A hash that matches the release notes means you have the published file. A hash that does not means the
download is not ours, and no antivirus setting is the right fix for that.

Why a scanner reacts at all, since the honest answer is more useful than calling it a false alarm: this
mod loads by being renamed over a DLL the game already imports, and it detours Direct3D and Vulkan entry
points once loaded. That is the same mechanism a malicious DLL uses, and a scanner cannot tell the two
apart from the outside. Nothing here hides that, and the project does not ask you to turn protection off.

If you need to play before a cloud verdict clears, scope an exclusion to the one game directory — never
to a drive, and never by turning real-time protection off:

```powershell
Add-MpPreference -ExclusionPath 'C:\Games\<your game>'
```

Remove it when the verdict clears, so the folder is covered again:

```powershell
Remove-MpPreference -ExclusionPath 'C:\Games\<your game>'
```

Both commands need an administrator PowerShell. An exclusion covers that directory and nothing else, and
it changes nothing for anyone else who downloads the release.

## Game notes

- **Baldur's Gate 3:** install beside `bg3.exe` / `bg3_dx11.exe` in `Baldurs Gate 3\bin`.
  Use `Dx12Upscaler=dlss` for `bg3.exe`, or `Dx11Upscaler=dlss_12` for `bg3_dx11.exe`.
- **Hogwarts Legacy:** install in `Phoenix\Binaries\Win64`; `dxgi.dll` was validated.
- **Cyberpunk 2077:** install in `bin\x64`; `dbghelp.dll` was validated on the development machine.
  Existing CET/RED4ext/ReShade loaders can require a different proxy or correct chaining.

Do not install the RenoDX DLSS add-on merely to obtain its compatibility runtime. This OptiScaler
fork drives `nvngx_dlssnr.dll` itself, and two Neural Rendering injectors can conflict.

## Troubleshoot flickering or ghosting with Reuse detail between frames

**Reuse detail between frames** runs the model every other frame and moves its detail onto the frame
in between, where it is trusted. **Flickering** is too little trust: a surface the depth or colour
guide describes poorly gets none of the model's detail on a reused frame, so a full and a reused frame
differ and the picture alternates between them. **Ghosting** (trailing detail behind a moving object)
is the opposite failure, too much trust: detail gets dragged across a moving object's own edge into
the background behind it. The two pull in opposite directions, so the fix for one can cause the other.

1. **Confirm Reuse is the cause.** Untick **Neural Rendering → Reuse detail between frames**. If the
   artifact stops, the steps below apply. If it does not, this feature is not involved.
2. **See where it happens.** Tick **Debug → Show dropped detail**. On reused frames this paints
   magenta where detail was dropped and cyan where Fill replaced it, so you can watch the affected
   area and confirm it lines up with a dropped (flicker) or a moving-edge (ghosting) region.
3. **Flickering, most cases: raise Depth tolerance**, under **Debug → How far a moved sample is
   trusted** (ini key `DetailReuseDepthTolerance`, under `[DlssNr]`). This is the fix for a surface the
   depth guide does not describe well — water, glass, fine geometry. Raise it until the flickering
   area settles; Fill uses three times whatever it is set to as its own neighbour window, so raising it
   also widens how far Fill may borrow detail across a depth step. **Red Dead Redemption 2** gets a
   higher value (0.267) automatically; every other game starts at 0.051. **Raising it too far is what
   causes ghosting** — it is the same test, loosened past the point where it still stops detail
   crossing a moving object's edge. If ghosting appears after raising this, lower it again rather than
   chasing the flicker further; see the next step if frame generation is also on.
4. **Flickering on fine, busy, colour-shifting detail instead** (foliage, patterned surfaces): try
   **Colour box** and **Colour falloff** in the same section, which govern how far the saved colour
   may drift from the current pixel and still be trusted, rather than depth.
5. **Ghosting, with frame generation on**: frame generation interpolates between pairs of full and
   reused frames, so any trailing in a reused frame — most likely from Depth tolerance set higher than
   this content needs — shows up doubled, in the generated frames too. Turn off **Keep on with frame
   generation** to have Reuse stand aside whenever frame generation is running: no reuse there at all
   while it runs, but no ghosting from it either.
6. **Flickering at the edges of the screen while moving or turning fast** is a separate,
   already-handled case: check **Pause while moving fast** is not set to 0 — it runs every frame
   through the model during fast motion instead of reusing.
7. **Still not right**: Reuse suits a single model pass best, since the same dropped or trailing areas
   compound across 2 or 3 passes. Reduce to one pass, or turn Reuse off for that scene as a last resort.

## Individual pass controls

Under **DLSS Neural Rendering → Model passes**, expand Pass 1, Pass 2, or Pass 3. Each contains
Style, Intensity, Local structure, Local tone, Skin structure, and Auto skin mask. Sliders commit
when released to avoid rebuilding the model on every movement. Set the model pass count to 2 or 3
to activate later passes; editing inactive passes prepares their settings without running them.

Later passes inherit pass 1 unless overridden, except Local tone, which defaults to 0 to preserve
the earlier build's appearance. Reset on a later-pass slider clears its override. Intensity,
local structure, and local tone range from 0 to 2; skin structure ranges from -1 to 2, with -1
following local structure. The corresponding INI keys are `Pass2Intensity`, `Pass2LocalStructure`,
`Pass2LocalTone`, `Pass2SkinStructure`, and `Pass2AutoMask`, with matching `Pass3...` keys.
Use `auto` for the default behavior. Styles retain `Pass2Style` / `Pass3Style`.

These controls apply to D3D12 multipass and its bridges, both before/after SR and after native RR.
Native Vulkan and the driver-proxy backend remain single-pass. Preset hints are still transmitted
at model creation, but the runtime version 310.8 contains a single built-in preset and falls back to
it for any hint, so changing a hint is not expected to change the picture. They are preserved under
**Advanced preset hints (effect unverified)** and in the INI for compatibility.

## What the model sees

The model averages the picture it is given 2x2 before its main network runs, then brings the result
back to full size at the end. So the main network always works at **half** of the size NR hands it,
and that halved size is what its cost and its finest added detail follow. **Model resolution** is
applied on top of that: at 50% the main network sees a quarter of the frame's width and height. The
menu shows both numbers under the slider and `OptiScaler.log` prints them when NR starts.

A reduced model size is rounded to a multiple of 16 so it holds still under dynamic resolution
instead of rebuilding the model for a pixel of difference. A pass at the native size is never rounded.

It is also worth knowing what the model does not take: it has no jitter input and no camera
matrices (colour, its own previous output, motion vectors and optional depth/UI/mask only), and its
output is limited to the 0..1 range, which is why NR works on a tone-mapped copy of the frame
rather than replacing HDR values.

## Padded DLSS input sizes

With a build containing the padded pre-SR fix and `RunBeforeSR=true`, an origin-zero 2558x1439 image
inside a 2560x1440 colour texture runs NR at 2558x1439 when `WorkingScale=1`. The padding is not
processed or overwritten. This does not change the game's DLSS preset ratios. Non-zero colour
offsets and invalid rectangles still fall back after SR. The older release downloads do not gain
this fix through an INI change; the loaded OptiScaler proxy DLL must be updated.
See [validation and reporting instructions](docs/PADDED-PRESR.md).

## Neural Rendering with native Ray Reconstruction

Enable RR in the game's settings and **Enable Neural Rendering** in OptiScaler.
The same **NR Pass at:** setting controls both ordinary SR and combined RR+SR:
**Before Super Resolution** runs NR on the active colour input before reconstruction/upscaling;
**After Super Resolution** runs it afterward.
Both use the common model-resolution slider, pass count and per-pass profiles.

```ini
[DlssNr]
Enabled=true
RunBeforeSR=true
Passes=1
WorkingScale=1.0
```

At 2560x1440 input and 4K output, `WorkingScale=1` runs NR at 2560x1440 before RR+SR,
or 3840x2160 afterward. It does not change the game's RR/SR quality setting.
Invalid or offset colour rectangles use the same post-upscale fallback as ordinary SR.
Switching between SR and RR or changing placement resets NR history. DX12, its bridges and
native Vulkan share this behavior. The separate deferred-residual DLSS experiment remains SR-only.

The retired `ApplyAfterRR`, `RRPasses` and `RRWorkingScale` INI keys are ignored. Existing users
now inherit their ordinary NR resolution and pass count, which can increase work compared with
an old half-resolution RR setting. Before RR, NR edits noisy ray-traced colour; the combined
path still requires in-game image-quality validation.

If Cyberpunk's RR option is greyed out with this fork, avoid the `d3d12.dll` proxy: an
[upstream report](https://github.com/Dagherbou/OptiScaler_DLSSNR/issues/8) confirmed that using
`dxgi.dll` resolved a Streamline conflict. Back up existing loaders before changing the proxy.
Keep the game's genuine `nvngx_dlssd.dll` (RR) separate from `nvngx_dlssnr.dll` (NR).
For an RR-only comparison, disable the master NR switch; “Apply the model” merely hides the edit
and still incurs NR's GPU cost. Successful RR initialization alone does not prove image quality.

## Optional DLSS Frame Generation

For the six NVIDIA Streamline/FG dependencies, the pinned download command, and separate instructions
for native/external FG versus OptiScaler's own FG, see [DLSS-FRAME-GENERATION.md](docs/DLSS-FRAME-GENERATION.md).
Do not copy another game's Streamline folder or assume NR working proves FG compatibility.
The optional component does not include the NR model or enable FG automatically.

## Diagnose a missing menu

Set:

```ini
[Log]
LogToFile=true
LogLevel=2
```

Then launch into a rendered scene and press `Insert` (`Alt+Insert` can help on some keyboard layouts).

- No `OptiScaler.log` beside the executable: the proxy was not loaded. Check the directory, proxy
  filename, antivirus quarantine, and conflicts with another DLL using the same proxy name.
- The log says `OptiScaler ... loaded` and `working as ...`: injection succeeded. A remaining problem
  belongs to the overlay input or Neural Rendering initialization, not the loader.
- `the model would not initialise`: on RTX 20/30/40, first check that the runtime hash is the
  compatibility `E67DEE...` build rather than the original `E16BC...` build.
- The menu toggles but does not accept input: try `[Hotfix] ManualInputPolling=true` and test without
  conflicting overlays.

## Apply NR to the finished picture

Set **NR Pass at:** to **Finished Picture** in the Neural Rendering menu to apply the effect after the game finishes its lighting and effects. This can help with green noise. It works with frame generation on or off and is off by default.

This option currently supports native DirectX 12 games using supported SDR, HDR10 or scRGB screen formats. HDR is detected automatically; no extra HDR setting is needed. It can also change the HUD and menus. Test the look and performance in your game.

With this option on, **Run the model before Super Resolution** chooses whether NR generates its changes before upscaling or directly on the finished picture. The older residual placement controls are hidden. Model strength, colour, precision and resolution still work. If the game does not provide usable picture or movement data, NR skips that picture (depth is optional); the status line explains known unsupported cases. It does not fall back to the earlier hook, which could bring the green noise back.

The INI setting is `[DlssNr] FinishedPicture=true`. Turn it off to return to your previous placement settings. Native Vulkan and the DirectX 11/Vulkan bridges are not supported by this option.


To run the model before upscaling but apply its changes to the finished picture, set **NR Pass at:** to **Finished Picture** and enable **Run the model before Super Resolution**. This uses the existing private DLSS path to upscale the saved changes. The game's picture stays unchanged until the final application, and NR is not run a second time.

This combination is experimental. It converts the saved changes into bounded relative colour adjustments because the game's final tone mapping is not available to the mod. It can look different from running the model directly on the finished picture. It supports the SDR, HDR10 and scRGB output paths in native DirectX 12, with normal frame generation on or off. Ray Reconstruction and the separate NR every second frame mode are not supported in this combination. A separate DLSS pass still has a GPU cost; a smaller NR input does not guarantee a faster overall frame.

The NR timing is **elapsed GPU time**, including delays while other GPU work runs. It is not the number of milliseconds added to each game frame. Applying NR later can increase this reading without lowering FPS. Compare FPS in the same scene to judge the performance change. With the pre-SR combination, this timing covers NR itself; the separate DLSS pass and final application also take GPU time.
