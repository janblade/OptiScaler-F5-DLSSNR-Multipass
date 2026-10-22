# OptiScaler Neural Rendering

A game mod that uses NVIDIA AI to change lighting, detail and colour. You can adjust the look and how much performance the effect costs.

This is an experimental community version of OptiScaler. Results and game support vary.

**[Download the latest version](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/releases/latest)** · [Setup guide](INSTALL-DLSSNR.md) · [What's new](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/releases) · [Discord](https://discord.gg/xzkNNwxbZu)

## Superior Scaling edition

This repository adds **NR Output > Enlargement > Superior Scaling** to janblade's 0.1.33 code.
It keeps the v59 Superior shaders and offers private DLSS SR A/K stabilization, separate from the NR model.
Resolution and NR pass settings remain configurable. See the [port and validation notes](docs/Superior-Scaling-port.md).
Builds are available as artifacts of [Build Superior Scaling port](https://github.com/mattjaas/OptiScaler-F5-Superior-Scaling/actions/workflows/build_nr_detail_quality_experiments.yml).
The release link above points to janblade's original edition, which does not contain this port.

## What you can change

- Adjust the strength, lighting, detail and colour of the effect.
- Use separate settings for skin and scenery.
- Run the effect before or after the game's upscaling.
- Apply NR to the finished picture to help with green noise. Works with frame generation on or off in native DirectX 12 games, including HDR.
- Apply it more than once, with different settings each time. Extra passes cost more performance.
- Lower the model resolution to reduce the performance cost.

## What you need

An NVIDIA RTX 20, 30, 40 or 50-series GPU and a supported 64-bit game. Older cards can be much slower.

Download the NVIDIA model file, `nvngx_dlssnr.dll`, separately. The file you need depends on your GPU. The [setup guide](INSTALL-DLSSNR.md#choose-the-correct-runtime) explains which one to use and how to check it.

## Install on Windows

1. Close the game and back up any existing mod files.
2. Download the release ZIP and extract **all files** beside the game's executable.
3. Add the model file described above to the same folder.
4. Run `setup_windows.bat` and choose **NVIDIA** when asked.
5. Start the game, select DLSS, then press **Insert** to open OptiScaler. Enable Neural Rendering and start with one pass.

See the [setup guide](INSTALL-DLSSNR.md) for game-specific steps and troubleshooting.

## Keep in mind

- Neural Rendering costs performance and can cause flicker or other visual problems. Using it before Ray Reconstruction is still experimental.
- The optional **hybrid mode** is for RTX 50 GPUs. Its files are included. Loading may pause the game and look like a freeze; please wait.
- Avoid anti-cheat-protected multiplayer games.

For frame generation, see the [setup notes](docs/DLSS-FRAME-GENERATION.md). For bugs, [open an issue](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/issues) with your game, GPU, settings and `OptiScaler.log`. For questions and help, join the [Discord server](https://discord.gg/xzkNNwxbZu).

## Credits

Built on [OptiScaler](https://github.com/optiscaler/OptiScaler) and [Dagherbou's Neural Rendering fork](https://github.com/Dagherbou/OptiScaler_DLSSNR), with colour processing from [RenoDX](https://github.com/clshortfuse/renodx).

[Full credits](docs/CREDITS.md) · [Licence](LICENSE) · [OptiScaler documentation](https://github.com/optiscaler/OptiScaler/wiki)

## Support

This fork is made in my spare time. If it's useful to you and you'd like to say thanks,
you can [buy me a coffee](https://buymeacoffee.com/janblade). Bug reports and logs help just as much.
