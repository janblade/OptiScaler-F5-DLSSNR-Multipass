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

## Recorded local verification (2026-10-25)

CPU checks passed, including the cubic endpoints, the 70% example, monotonicity, strict CNN sigma
threshold (exactly 0.01 stays A), Transformer stability and native-mode menu selection policy.
All 50 shader blobs and generated headers match a clean rebuild. GPU validation passed:

- Existing host composition: 2304 cases / 5,974,272 words, fixed-sigma reference parity.
- Independent cubic literal references: 360 cases / 18,018,720 components, 50/65/70/88/100%,
  SDR/HDR, game/automatic exposure, packed/uniform/cached queries and FP16/FP32 outputs.
  Maximum component error 0.000488281 (FP16 rounding); comparisons enforce FP16-relative and
  FP32 2e-5-relative bounds. This checks implementation, not superiority of the new visual tuning.
- Joint guide reconstruction: 1092 cases / 10,284,288 pixels, including 84 render-resolution cases.
  Plane depth RMS 0.000072889837 to 0.000000023551; depth/MV layer mismatches 976 to 976
  across 4,407,552 fixture queries. Bounded same-surface motion and non-minified preservation checks pass.
- Smaller guide/private conversion: 559,104 NR pixels and 6,488,064 private motion components.
- Original Compress pack/unpack and 48 carrier/MV parity cases also pass.

### Timestamp comparison

RTX 4090, driver 616.56, output 3840x2160, Balanced compression. Sloped R32F depth and affine
RG32F MV are generated at 1080p or 1440p. Base is janblade `b276cbb4`: point guides, colour-sized
compressed guides and fixed range sigma 0.015. Candidate is code commit `3f3e1046`: joint guides,
render-resolution packed guides at 100%, private guide reconstruction and cubic sigma.
The full synthetic changed route described above is timed together, with **NR/DLSS inference excluded**.

Before testing the GPU was at 2%, 27.57 W, 210 MHz; after settling it returned to 2%, 27.50 W,
210 MHz. Eight alternating ABBA/BAAB blocks each use eight warmup quartets and 40 measured
quartets, with four GPU repetitions per timestamp: 640 samples per variant/configuration.
Delta is the median of paired-quartet differences; variability is the standard deviation of those
paired differences. Timing is not a measurement of game image quality or model inference cost.

| Render | Output | Model resolution | Base ms | Candidate ms | Paired delta ms | Delta stddev ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 1080p | SDR | 65% | 0.595328 | 0.646144 | +0.049920 | 0.047227 |
| 1080p | HDR soft knee | 65% | 0.514560 | 0.568320 | +0.053184 | 0.045769 |
| 1080p | SDR | 100% | 0.868352 | 0.990336 | +0.120448 | 0.065094 |
| 1080p | HDR soft knee | 100% | 0.753664 | 0.872960 | +0.117504 | 0.062193 |
| 1440p | SDR | 65% | 0.607872 | 0.704256 | +0.094400 | 0.053017 |
| 1440p | HDR soft knee | 65% | 0.527872 | 0.624640 | +0.094016 | 0.062121 |
| 1440p | SDR | 100% | 0.892160 | 1.085952 | +0.191808 | 0.064035 |
| 1440p | HDR soft knee | 100% | 0.778368 | 0.970624 | +0.189568 | 0.059409 |

The new guide reconstruction has measurable overhead in these fixtures: approximately 0.05 ms
at 65% / 1080p render and 0.12 ms at native 100% + Balanced, increasing to approximately
0.09 / 0.19 ms at 1440p render. These are intentional reconstruction changes, not a demonstrated
performance optimization. CNN A/E inference cost is not covered and can change the full-frame result.
Raw samples, summaries and quality results are retained in `validation/superior-refinement-*`.

The subsequent quality-preserving [private guide/carrier fusion](Superior-private-guide-fusion.md)
reduces native-compression preparation cost; its A/B baseline is this branch, not janblade.
