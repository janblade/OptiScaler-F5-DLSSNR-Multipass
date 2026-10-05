# Credits

This fork was itself forked from [wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass), which builds on [Dagherbou/OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR) and [OptiScaler](https://github.com/optiscaler/OptiScaler). OptiScaler began with [PotatoOfDoom's CyberFSR2](https://github.com/PotatoOfDoom/CyberFSR2).

Colour processing is derived from [RenoDX by clshortfuse](https://github.com/clshortfuse/renodx). See the [RenoDX attribution and licence](../Licenses/RenoDX_ATTRIBUTION.txt) for details.

## DLSS-NR exposure controls

Automatic exposure from the HDR frame, the wider exposure Trim range and the Trim Anchor points are @mattjaas's work, from [wilsjo2's PR #77](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/pull/77), adapted onto this fork's code.

## Depth buffer detection

The rules the native depth finder uses to pick the scene's depth buffer (how draws are weighed, the aspect-ratio test, which frames are ignored, the snapshot taken before a clear) are adapted from the [Generic Depth add-on in ReShade](https://github.com/crosire/reshade/blob/main/examples/09-depth/generic_depth_addon.cpp) by Patrick Mours (BSD-3-Clause). See the [licence](../Licenses/ReShade_GenericDepth_LICENSE.txt).

## Motion and trust mask

The optical flow for the native input producer is our own (a luma pyramid, block matching and a few Lucas-Kanade steps), written from the standard method; the public descriptions of the ReShade optical-flow shaders were read for ideas and none of their code is used. The flow upgrade (trying the last frame's flow and the neighbouring coarse cells as candidates, a deeper pyramid, and smoothing that follows the picture's edges) took its ideas from the public description of [LumeniteFX](https://github.com/umar-afzaal/LumeniteFX)'s LumaFlow (its licence reserves all rights, so none of its code or constants are used; its own notice names AMD FidelityFX Optical Flow, Zenteon's motion vectors and Video++ as its sources). The trust mask follows the checks of the validation in [DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder)'s DLSS5_Feed.fx (MIT, Copyright (c) 2026 Jean-Laurent ROUZIES): reproject with the motion, and distrust a pixel where the depth, the motion or the luma there disagrees with here. See the [licence](../Licenses/DLSS5Feeder_LICENSE.txt).

## Shutdown safety

Process-exit hardening (a heap-leaked state singleton, an atomic shutdown flag, an `ExitProcess`-aware
`DllMain`, and safe feature teardown during shutdown) is adapted from [wilsjo2's fork](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass), commit [`dac290ed`](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/commit/dac290ed17ed88962a7f9ee63a41b7500a0fc923).

## RTX 40 MFG unlock

The built-in RTX 40 multi frame generation unlock is adapted from [y4my4my4m's fork](https://github.com/y4my4my4m/OptiScaler_DLSSNR_Multipass_MFG) (GPL-3.0). The provider discovery, the Streamline plugin frame-ceiling patch, the software frame pacing option and the PTX temporal fix are adapted from [KleberMotta's fork](https://github.com/KleberMotta/OptiScaler-DLSS5-MFG-RTX40) (MIT), a port of the MFG Unlock ReShade addon by [Dreamt](https://github.com/ImDreamt/MFGAdaUnlock-RenoDx) and [mavismmg](https://github.com/mavismmg/MFGAdaUnlock-RenoDx). The technique originates from [dashdogy's RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock), which is also the optional external unlocker (see [RTX40-MFG.md](RTX40-MFG.md)). See the [licences](../Licenses/MFGUnlock_LICENSE.txt).

The fix for an unsynchronized read/write race in `MfgUnlock::LastStatus()` follows the same lock-and-return-by-value approach as [wilsjo2's fork](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass), commit [`bf91eebd`](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/commit/bf91eebd00da40d079b8d987dbf44aca0be598e0) ("Extend optional Ada MFG support with shared patch and status locking"); the write side of the race (`MfgUnlock::TryApply`) was found and fixed independently during review.

## KCD2 HDR compatibility

The binary quirk patch that keeps Kingdom Come: Deliverance II's native HDR10 output working with frame generation active is adapted from [wilsjo2's fork](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass), commit [`34dfe6d9`](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/commit/34dfe6d9757837d79a8931b04af0e3955cbe29a7) ("Keep KCD2 HDR output compatible with native DLSSG").

## Streamline dual-runtime isolation

The fix keeping the game's own Streamline plugins (Common, Reflex, PCL) separate from OptiScaler's
private DLSS Frame Generation runtime when NVIDIA's override selection would otherwise point both
at the same already-loaded module is adapted from [wilsjo2's fork](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass), commit [`ae9a50fa`](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/commit/ae9a50fa12b1e81600d0394108d523dad8b7fd23) ("Isolate game Streamline plugins from active DLSSG output").

## Highlight guard: brightening only

Bounding the Composed path's Highlight guard to brightening only, leaving darkening uncapped, is
@mattjaas's work, from [wilsjo2's PR #94](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/pull/94),
adapted onto this fork's code. This fork's Replace-mode guard (`ApplyReplaceGuard`) is unrelated to
that PR and keeps its own, still-symmetric bound for a different reason.

## OptiScaler contributors

These credits are retained from the original OptiScaler README:

- @PotatoOfDoom for CyberFSR2.
- @Artur for DLSS Enabler and help with the NVNGX API.
- @LukeFZ and @Nukem for their mods and shared knowledge.
- @FakeMichau for support, testing and features.
- @QM for testing and access to games.
- @TheRazerMD for testing and support.
- @Cryio, @krispy, @krisshietala, @Lordubuntu, @scz and @Veeqo for the earlier compatibility matrix.
- The DLSS2FSR community for its support.

This project uses [FreeType](https://gitlab.freedesktop.org/freetype/freetype), licensed under the [FTL](https://gitlab.freedesktop.org/freetype/freetype/-/blob/master/docs/FTL.TXT). Other notices are in [Licenses](../Licenses).

## Upstream sponsorship

The original OptiScaler project credits [SignPath.io](https://signpath.io/) for Windows code signing and the [SignPath Foundation](https://signpath.org/) for its certificate.

To support the original developers: [cdozdil on GitHub Sponsors](https://github.com/sponsors/cdozdil?frequency=one-time) and [nitec on Buy Me a Coffee](https://buymeacoffee.com/nitec).
