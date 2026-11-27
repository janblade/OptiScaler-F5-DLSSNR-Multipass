# Optional FSR 4 shader override installer

`get_compatible_fsr4.ps1` downloads the GPU-appropriate precompiled DLL from the pinned upstream release and puts it in place of the package's `OptiScaler\amd_fidelityfx_upscaler_dx12.dll`. It is a downloader only: **no patched DLL is shipped in this package**.

Upstream project (this fork of the FSR 4 override): <https://github.com/zgauthier2000/fsr4-shader-performance-overrides>  
Pinned release: <https://github.com/zgauthier2000/fsr4-shader-performance-overrides/releases/tag/dll-2026-11-22>  
Manifest: [`redist/fsr4/manifest.json`](../redist/fsr4/manifest.json) — archive URL, archive SHA-256 and all 16 DLL SHA-256 values come from the upstream release's `SHA256SUMS`.

## Warnings — read before running

- **Single-player games only.** These are modified game binaries. Never use them in online games or games with anti-cheat; anti-cheat can treat the replacement as tampering.
- **Not for Radeon RX 9000 / RDNA4.** The upstream builds do not support those cards; the script refuses them.
- **The DLLs are not Authenticode-signed.** The patch removes AMD's signature. Integrity is verified only by SHA-256 against the pinned manifest. Do not disable antivirus to use them; check the hash instead.
- `lossy` and `lite-lossy` **change the picture**. Read the upstream `WARNING.txt` and compare before/after; the archive's `WHICH-SHOULD-I-CHOOSE.txt` explains the trade-off.
- A game or OptiScaler update may restore the original DLL. Re-run the script after updates.
- The script **does not edit `OptiScaler.ini`**. If `Fsr4Model` or `Fsr4GpuSupport` was forced to a value, set both back to `auto` yourself.
- The script replaces a DLL only if its SHA-256 is known: the package's 4.1.2.4555 DLL, AMD's 4.1.1.2740 DLL, or one of the upstream builds. An unfamiliar DLL is never overwritten.

## Usage

Open PowerShell in the folder that contains `get_compatible_fsr4.ps1` (the package root, next to `OptiScaler.dll`):

```powershell
powershell -ExecutionPolicy Bypass -File .\get_compatible_fsr4.ps1 -Variant exact
```

`-Destination` is optional: when omitted it defaults to the folder containing the script (`$PSScriptRoot`), i.e. the extracted package root next to `OptiScaler.dll`. If you run the script from somewhere else, pass the game folder explicitly. That folder must contain `OptiScaler.dll` and an `OptiScaler` folder. The script writes `OptiScaler\amd_fidelityfx_upscaler_dx12.dll`, keeps `OptiScaler\amd_fidelityfx_upscaler_dx12.dll.orig`, and records what it installed in `OptiScaler\.fsr4-override.json`.

After printing the warnings, the script asks `Accept [y/N]:`. Type `Y` and press Enter to continue. Just pressing Enter, `N`, or anything else cancels with no changes. For unattended runs, pass `-AcceptThirdPartyDll` to skip the prompt.

### Variants

| `-Variant` | ZIP folder | Picture |
|---|---|---|
| `exact` (default) | `exact/` | Matches AMD's picture, faster shaders |
| `lossy` | `lossy/` | Faster still, changes the picture |
| `lite` | `4.1.2l-exact/` | Lite model, AMD's Lite picture |
| `lite-lossy` | `4.1.2l-lossy/` | Lite model, faster still, changes the picture |

All four are pinned for all four archives. The upstream release notes explain which ones were measured on which cards; performance varies by game and driver.

### GPU selection

Detection uses the PCI device ID first (`VEN_1002&DEV_xxxx`) and the adapter name second. A dGPU wins over an iGPU when both are present. Use `-GpuFamily` to override detection on dual-GPU systems or when Windows reports a generic name.

| Detected | `-GpuFamily` | Archive |
|---|---|---|
| Radeon RX 7000, Steam Machine | `rx7000` | `fsr4.1.2-cyboman-rx7000-…` |
| Radeon RX 6000 | `rx6000` | `fsr4.1.2-cyboman-rx6000-…` |
| Radeon 780M / 880M / 890M, RDNA 3 / 3.5 iGPU | `igpu-rdna3` | `fsr4.1.2-igpu-cyboman-rdna3-…` |
| Steam Deck, Radeon 680M / 660M, RDNA2 iGPU | `igpu-rdna2` | `fsr4.1.2-igpu-cyboman-rdna2-…` |
| Radeon RX 9000 / RDNA4 | `rdna4` | refused — unsupported upstream |
| GeForce, Intel Arc, other GPUs | `igpu-rdna3` | upstream fallback, with a warning |

### Other parameters

- `-Destination <folder>`: game folder to patch. Defaults to the script's own folder (`$PSScriptRoot`), so it can be omitted when running from the extracted package root.
- `-ArchivePath <file.zip>`: use a previously downloaded pinned ZIP instead of downloading; its SHA-256 is still checked. Downloads are cached under `.dependencies\fsr4\<version>\` next to the script.
- `-Restore`: put the `.orig` backup back and remove the marker. No confirmation switch is needed for a restore.
- `-AcceptThirdPartyDll`: optional shortcut for unattended installs. It acknowledges the warnings above without the `Accept [y/N]:` prompt; the warnings are still printed.

```powershell
# Offline install from a pinned archive, from the extracted package root
.\get_compatible_fsr4.ps1 -GpuFamily rx7000 -Variant lossy `
  -ArchivePath "D:\downloads\fsr4.1.2-cyboman-rx7000-2026-11-22.zip" -AcceptThirdPartyDll

# Undo
.\get_compatible_fsr4.ps1 -Restore
```

## Distribution and updates

`package_release.ps1` copies `get_compatible_fsr4.ps1`, `redist/fsr4/manifest.json` and this document into every release ZIP, and checks that the manifest still pins four archives with four variants each. The cache under `.dependencies` is not packaged.

To move to a newer upstream release, update `version`, `upstream.releaseUrl`, each archive `url`/`sha256` and each variant DLL hash in the manifest from the upstream `SHA256SUMS`. Keep the `originals` entries unless the package's own upscaler DLL changes.

## Licence

The upstream downloader and its patched shaders are GPL-2.0-or-later. Verbatim copies ship with this package:

- [`FSR4-OVERRIDES-LICENSE.txt`](FSR4-OVERRIDES-LICENSE.txt) — the upstream `LICENSE` (GPLv2 text).
- [`FSR4-OVERRIDES-NOTICE.md`](FSR4-OVERRIDES-NOTICE.md) — the upstream `windows/prebuilt/NOTICE.md`, with AMD's notice and the shader modification details.

Upstream links: <https://github.com/zgauthier2000/fsr4-shader-performance-overrides/blob/main/LICENSE> and <https://github.com/zgauthier2000/fsr4-shader-performance-overrides/blob/main/windows/prebuilt/NOTICE.md>. The patched DLL is AMD's FidelityFX DLL under AMD's notice, with modified shaders. This package ships only the script, the manifest, these instructions and the two verbatim licence/notice files; it does not redistribute the patched DLL.
