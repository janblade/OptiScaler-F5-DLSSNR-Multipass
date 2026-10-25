# Superior Scaling depth-aware refinement

Based on janblade's latest `release/0.1.34` at `b276cbb4` (2026-10-25), on the independent branch
`codex/superior-depth-aware-refinement`. This is a prospective follow-up to merged PR #237.
No pull request has been created; game validation is required before submitting a new PR.

## Behaviour

Superior now prepares its resampled motion with the limited depth-aware estimator and reconstructs
device depth on continuous surfaces. At depth discontinuities, bounded slopes and depth-compatible
motion selection avoid averaging unrelated foreground/background surfaces. A sparse second dispatch
corrects only source footprints that minify either depth or motion; searches are bounded to 8x8.
Independent valid rectangles, source origins, reversed Z, MVLowRes declarations and source motion
units are retained. Full native 100% without compression does not introduce guide reconstruction.
Ordinary below/above-native Superior guide resampling and compressed Superior both use this path.

At exactly 100% Model resolution with active Compress, NR depth and MV use the declared render
resolution multiplied by the **actual packed-colour/native-colour ratio on each axis**, rounded to
the nearest pixel. They are auxiliary textures, not the model's 16-pixel-aligned colour grid.
For 3840x2160 colour, 1920x1080 render and Balanced compression:

| Consumer | Colour | Depth / motion |
| --- | --- | --- |
| NR | 3456x1952 | 1728x976 |
| Private residual DLSS | 3456x1952 carrier | 3456x1952 reconstructed from NR guides |

The smaller guide lattice preserves the colour warp's normalized geometry, including centre phase,
offsets and shifts. Motion transforms both endpoints through that geometry and is expressed in guide
pixels. NR receives the guide dimensions and scale 1. The private DLAA step has no independent depth
extent, so the same guide pair is reconstructed uniformly onto its carrier raster. Motion units are
converted once more to carrier pixels, with no second nonlinear compression. Render-size changes
reset temporal history and reallocate only when dimensions change.

Guided residual reconstruction remains enabled over the **entire image**, including native 100%
with Compress. There is no native-centre bypass or experimental stage-skipping menu. Radius one,
3x3 taps and spatial sigma 3.0 are unchanged. Range sigma follows Model resolution, excluding the
additional compression ratio:

```
t = clamp((modelScale - 0.5) / 0.5, 0, 1)
rangeSigma = 0.015 - 0.012 * t^3
```

| Model resolution | Range sigma |
| --- | ---: |
| 50% and lower | 0.015 |
| 65% | 0.014676 |
| 70% | 0.014232 |
| 100% and higher | 0.003 |

The inverse variance is computed once on CPU and passed as float bits in the otherwise unused
final-composition meter-width field. The 256-byte ABI and inter-pass filter are unchanged; no
per-pixel cubic calculation or extra dispatch is introduced by the sigma policy. This does not
alter exposure metering, janblade's automatic exposure or HDR composition.

Private DLSS UI choices are **CNN - higher performance** and **Transformer - slower, more stable NR
in motion**. The saved values remain 1 and 11 for existing settings/profiles. CNN requests A while
range sigma is at least 0.01 and E strictly below 0.01 (above approximately 87.35% on this curve),
with or without compression. Transformer requests K throughout. A model change recreates the
private feature and resets history. Unavailable hints may be ignored by the installed runtime;
the existing explicit-creation-failure fallback to runtime Default remains. Private IsHDR and
AutoExposure remain enabled.

Small NR model, Natural style high/medium calibration changes from 0.925 to **0.900**. Its low gain
remains 0.838. Main/Small/Default detection, other calibrations, high/medium's Model-resolution-only
scale and low's additional compression scale remain unchanged. The existing 0.15 inter-pass rule
naturally responds to the new high gain; mixed-model chains can have a different boundary schedule.

## Menu and original fallback

Upscale Mode is no longer disabled as a whole at 100% when a non-Superior mode was selected. Classic
and supported Superior can always be selected. Matched residual and NVIDIA residual are disabled
as new selections at 100% and above, where the original shader uses Classic composition
(`gModelWorkScale` is 1 and its residual branch requires less than 0.999). A saved residual choice
is preserved for returning below 100%, and the preview identifies the active Classic fallback.
No render-path change is applied to ordinary enlargement/compression. Native Vulkan retains its
existing unsupported-Superior restriction; an NGX failure on D3D12 does not permanently lock out retry.

## Validation and limits

Run `tools/test_superior_port.ps1` for CPU policy/ABI checks, and
`tools/test_superior_port.ps1 -GpuGuides -GpuSpatial -GpuHost` for GPU validation. The latter prepares
frozen janblade baseline shaders and independent literal-sigma reference shaders with
`tools/prepare_superior_refinement_tests.py`. `tools/compile_nr_interpass.ps1 -Check` verifies every
shipped shader against a rebuild; ordinary janblade/Vulkan blobs and frozen v59 kernels are preserved.

The geometry is an approximation: bounded searches can miss very thin surfaces, foreground selection
can expand silhouettes, and smaller render-resolution guides discard detail compared with a denser
field. These changes intentionally alter reconstruction, so numerical validation does not establish
universal visual improvement. NR/private DLSS model acceptance and motion quality need game testing.

`-GpuRefinementTiming` compares the baseline and candidate GPU preprocessing/composition at 4K with
Balanced compression. The measured route includes colour packing, guide preparation and correction,
private guide reconstruction where needed, carrier preparation/decode, low field and final cached
guided composition with barriers. It excludes proprietary NR/DLSS inference and therefore does not
establish a full-game frame-time gain or the relative cost of CNN versus Transformer.
