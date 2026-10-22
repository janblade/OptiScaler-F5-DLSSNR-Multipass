# improved F5Low motion vectors

## Scope and status

The estimator reference is pristine Janblade F5Low **0.1.31**
(`aeb439482c13dd7364ee2bef16fa41b902be6f98`). The proposed source is integrated
with current upstream **0.1.33** (`2a5be38dc778913b4229efbeb750e2a2c6474242`).
The 0.1.33 merge and final candidate were compiled and tested. This is an
OptiScaler-only pull request; it does not publish a release or replace main.
The separate runtime package contains the modified Feeder/Host/ReShade files;
those projects and binaries are not added to this repository.

The objectives are coherent, inexpensive current-to-previous motion for NR and
frame generation, without removing genuine independent water, fog or smoke
motion. Synthetic motion tests establish estimator behavior, not the quality of
every interpolated game frame. The remaining shader performance target is
reported explicitly rather than inferred from game FPS.

## Estimator changes in this PR

### Preserve the F5Low architecture and matching contract

The six-level pyramid, per-pixel candidate search, depth/luminance weighting,
median, Smooth, global candidate, still-age and scene-cut pipeline remain.
Integer matching windows and conventional Lucas–Kanade displaced sampling
coordinates remain individual to each pixel. The field uses RGBA16F:

- XY: full-resolution pixel offset from current to previous content, after the
  finest-level output is scaled by Median.
- Z: original local two-axis texture structure, low = weak, high = strong.
- W: separately derived support for the selected vector or validated proposal.
  It is not a calibrated probability.
- History rejection is a separate quantity: 1 = reject, 0 = retain history.

Darkness does not, by itself, make a motion vector invalid. A flat surface may
move. Conversely, high local texture does not prove that its selected match is
correct. The patch keeps these concepts separate.

### Matching and photometric refinement

- Reuse an already evaluated candidate cost only within that pixel. Preserve
  search order, tie behavior, original window and all weights.
- Preserve a validated fractional inherited basin rather than losing it when
  the integer search lands on the same rounded candidate.
- Clamp a numerically negative smaller structure eigenvalue to zero. A tiny
  negative roundoff result must not become negative confidence/filter support.
- Guard ill-conditioned LK solves. Weak-texture recovery requires both useful
  explained temporal residual and an improvement of that pixel's original
  weighted photometric objective.
- Check a subpixel proposal in its own sampled objective, including the proper
  unrefined fractional-sampler baseline. Avoid accepting a step merely because
  integer-load and filtered-sampler costs differ at edges.
- Keep boundary/depth separation and exact-static feature protection. No
  temporal freeze is used as a general solution for ambiguous motion.

### Selective geometric fusion and regional direction

The current model is a regularized screen-space **affine motion prior**, fitted
from contemporaneous finest-level observations in 128 spatial strata with
robust reweighting. It is not an engine camera matrix, depth-derived 3D camera
pose, projective homography, or the canceled early RANSAC implementation.

Ambiguous pixels may propose a coherent regional motion or the camera prior.
Each pixel validates the proposal in its own depth/luminance-weighted
photometric window. Reliable independent foreground motion is protected;
foreground depth boundaries are not merged indiscriminately. There is no rule
that forces every vector in the entire image to one direction. Rotation,
parallax and independently moving objects require different directions.

Independent advection recovery applies to both low-contrast material and
textured water/highlights when they supply adequate evidence. Smooth detects
relative speed disagreement as well as angular conflict. Each regional proposal
is still evaluated in that pixel's own original weighted photometric window.
Recovery requires spatial coherence, absolute/relative photometric improvement
over the camera and temporal confirmation of the pixel's **pre-Smooth measured
center vector** at its own reprojection into raw Match history. This avoids
using already camera-attenuated history as the sole evidence for movement.
Partial evidence gates combine by their minimum instead of multiplying toward
zero. Regional speed is restored only when its original per-pixel objective
improves. Existing exact-static and foreground/depth boundaries remain.

The final measured recovery and its temporal regressions are reported below.
There is no global confidence boost or semantic sky/smoke classifier, and no
claim that forcing a region to one direction is always physically correct.

## Data-preserving optimization

These changes are separate from intentional quality regularization:

- Share only identical, immutable source-image/depth loads at their original
  coordinates. All lanes, including padded edge lanes, reach each barrier.
- Reuse filtered LK gradient endpoints only within the same pixel when their
  constructed coordinates are identical. There is no common neighbor LK
  sampling window when offsets differ.
- Sum invariant window weights once per pixel in the original addition order.
- Stop refinement only at an exact floating-point fixed point.
- Use constant integer pitches for the same source-cache addresses.
- Reuse workgroup scratch allocation between separate entry points, never
  overlapping live arrays in one invocation.
- Stage the existing immutable Smooth input record and guide luma at the same
  clamped coordinates. This includes copying the existing W component of an
  input vector record, not sharing a newly computed confidence/decision. It is
  an explicitly validated exception: the original stencil, center read,
  per-pixel evidence/luma/depth/motion weights and accumulation order remain.
- Cache descriptors only for internally owned textures and known heap slots.
  External color/depth inputs are rewritten. Resource identity, format and
  SRV/UAV kind are keyed; caches clear before resources/heap slots are recycled.
  Each frame advances to a fresh descriptor bank after its actual allocations,
  including optional diagnostics. GPU math is unaffected.

Rejected candidates included a shared raw previous-image cache, a larger
matching group, an LK source-lattice cache, a fused pyramid reduction and an
earlier luma/depth pass fusion. Some were exact but slower, especially in Vulkan.
They are not shipped merely because they reduced texture fetch counts.

The proposed `16 / surviving depth weight` confidence normalization is not
claimed implemented or accepted here. The fixed-threshold silhouette hypothesis
must not be confused with the independently established camera suppression and
sampling/refinement issues addressed above.

## Depth, debug and resource correctness

- Preserve automatic native depth selection. Zero size overrides mean auto;
  manual source dimensions, UV scale and pixel offsets remain optional.
- Keep tiny nonzero hardware depths rather than collapsing distant PCSX2
  geometry into the clear plane. Keep aligned raw depth for NR and matching.
- Make depth-guided reconstruction finite if clear-depth holes leave no valid
  ordinary interpolation support. Retain the normal interpolation path when
  its weight sum is valid.
- Use original selected depth with linear sampling for DisplayDepth-style
  normals. Display normals on the left, depth on the right in scene coordinates.
  Preview linearization, far plane and multiplier affect only visualization,
  not the raw depth supplied to DLSS/NR or matching.
- Optional sky/far-depth suppression remains available and off by default. It
  affects exported XY, published support and history rejection together. It is
  raw-depth classification, not semantic sky detection; effects sharing that
  depth can also be excluded. Missing depth falls back without suppressing the
  whole scene.
- Retain preview resources and establish the required fences/resource states
  across the producer, renderer and frame-generation device. Compare canonical
  COM device identity rather than treating interface-pointer aliases as
  different devices. Preview/fullscreen must not change estimator output.
- Invalidate histories on reset, resize, encoding change and missing/nonconsecutive
  producer frames. Colour-encoding changes can legitimately change neural model
  output; they must not retain a mismatched temporal history.

Seven public views remain, with legends: motion arrows, normals/depth, final
optical flow, orange/green confidence and rejection, RGB confidence channels
(texture/photometric/history), history rejection, and still age. Intermediate
candidate/median/refinement views are not public choices. Trace is optional,
compile-time diagnostics; it is not a quality mode and is off in the package.
There is no public baseline, legacy-output or umbrella internal-experiments
switch. The corrected estimator is the production path.

## Feeder/shader integration: companion delivery, outside this PR

The companion ReShade estimator is generated mechanically by
`tools/generate_native_fx.py` from the active native kernels. Native matching,
camera/refinement, reconstruction and debug code is transplanted, not replaced
with camera vectors alone. ReShade resource bindings, intrinsics and history
storage are adapted. Vulkan nearest-even rounding is made explicit to match
HLSL candidate rounding; runtime dimensions preserve odd-size arithmetic.

Provider 5 exports the full-resolution pixel guide directly. Its stored XY
reaches Feeder without a UV quantization round trip or a second static/camera
rewrite. Extra Feeder validation only adds history rejection; disabling it must
not disable the provider's own rejection mask. Frame metadata/continuity guards
prevent accepting stale or missing output. Device/frame ownership is validated
before handing buffers to native NR. The x86 DX11 client transports to Host64;
native x64 DX11/Vulkan can use the same consumer without a host process.

The modified Feeder/ReShade add-on and Host64 projects/binaries are not included
in this OptiScaler-only PR. Their runtime package is separate. Clean Vulkan
installation uses the Vulkan layer and must not also inject a root DXGI ReShade
proxy. The clean package also defaults FG to `nofg` rather than a disabled
XeFG configuration that can still initialize a bridge.

## Validation boundaries

The route matrix covers native OptiScaler DX11/Vulkan, shader + Feeder x64
DX11/Vulkan, and x86 DX11 → Feeder32 → Host64. Controlled tests exercise normal
and reversed depth, mirrored calibration, low-resolution source depth, exact
clear/far-depth exclusion, small positive depth, no-depth fallback, continuity,
Feeder XY preservation, seven finite debug outputs and DisplayDepth reference
rendering. Independent NR on/off captures distinguish actual model composition
from merely reporting loaded inputs. Native debug, resize/reset, producer,
camera/foreground and low-contrast tests accompany the implementation.

XeFG and FSR FG have controlled DX11/Vulkan checks; prior game confirmation
covered normal and fullscreen arrows/normals/depth with XeFG. DLSS-G cannot be
validated on this RTX 3080: fallback to no FG is **not** a DLSS-G pass. DX12 game
F5Low is outside the requested scope. The internal D3D12 processing backend is
still required by the supported routes.

The reported real PCSX2 Vulkan symptom—opening the OptiScaler menu substantially
increases FPS—has not been conclusively reproduced and resolved in the synthetic
fixtures. Correct NR composition or a clean Vulkan installation alone does not
prove that game-specific performance issue fixed. Final water/fog quality in the
games requires the user's scene check; synthetic ground truth is reported
separately.

## Measurements

RTX 3080, 3413×1920, normal Lucas–Kanade. Native total includes the complete
estimator, Trust/disocclusion and full-resolution guide; excludes NR, FG, debug,
CPU readback, IPC and separate input acquisition/depth selection. Inputs are
provided. 64 warmup + 160 measured frames, four counterbalanced runs per variant;
median of run medians. “Before” is the previously installed modified build,
not the pristine reference.

| Native scene | Original 1.31 ms | Before ms | Final ms | Final − original ms |
|---|---:|---:|---:|---:|
| Moving | 3.531264 | 3.081728 | 3.056640 | −0.474624 |
| Stationary | 3.503616 | 2.219008 | 2.201600 | −1.302016 |

The native difference versus the immediately preceding build is only −0.025088
ms moving / −0.017408 ms stationary, within the scale of run-to-run variation.
Do not present the full improvement versus original as a gain from this last
small cache change alone.

Actual ReShade motion + Feeder GPU technique totals, 320 frames/run, 60 warmup,
four counterbalanced runs per variant per route. The intervals are paired and
summed per frame before the median is computed. Transport/NR/FG/debug are off:

| Shader producer | Before ms | Final ms | Final − before ms |
|---|---:|---:|---:|
| x64 DX11 | 3.709152 | 3.604128 | −0.105024 |
| x64 Vulkan | 4.943872 | 5.044224 | +0.100352 |
| x86 DX11 for Host64 | 3.811840 | 3.828064 | +0.016224 |

These are different workloads/backends from the native table; no pristine
1.31 ReShade transplant baseline exists. Host64's producer timing does not
include cross-process transport or consumer model/display latency. DX11 had
wide run spread (before 3.588592–3.829648, final 3.600384–3.662320 ms), with only
two of four paired comparisons improving. No reliable general DX11 speedup is
claimed. Vulkan and Host64 have a small net cost increase in this sample.
The requested additional 0.7-ms saving / ≤1.7-ms gap to Kernel HQ was **not
demonstrated**. Desktop GPU activity/clocks and run medians are recorded.

## Final recovery, regressions and acceptance limits

Known advecting texture with changing illumination, 321×181, 24 frames, last 16
measured, dy = 0.35×dx. Not physical game-water ground truth. Example normal LK:

| dx / contrast | Magnitude before → final | EPE before → final | Within 30° before → final |
|---|---:|---:|---:|
| 0.4 / 0.04 | 0.0443 → 0.1008 | 0.3849 → 0.3488 | 99.35% → 99.24% |
| 0.8 / 0.04 | 0.4101 → 0.5658 | 0.4798 → 0.3595 | 99.92% → 99.96% |
| 0.4 / 0.65 | 0.4254 → 0.3884 | 0.1492 → 0.1517 | 91.07% → 93.84% |
| 0.8 / 0.65 | 0.8408 → 0.8242 | 0.1663 → 0.1658 | 99.61% → 99.80% |

Actual XY temporal variation increased in all four representative cases:
0.054852→0.126400, 0.302713→0.335500, 0.075950→0.091519,
0.093012→0.105777 pixels/frame respectively. The observer measures the mean
frame-to-frame vector change at the same interior pixel across 15 intervals;
expected velocity is constant. It reproduced the preceding EPE within 10⁻⁶ px.
Previously suppressed flow has artificially low variation, but this is still
a real tradeoff. Inverse LK shows the same trend. **Recovered motion is not
proof of improved temporal uniformity or finished frame-generation quality.**

The worst static sky/grain EPE increased 0.016020→0.027816 px with normal LK
(+0.011796) and 0.022053→0.037735 with inverse LK (+0.015682), failing the
strict +0.01-px screening gate. Clean static sky remains static. The smallest
0.05-px motion remains mostly suppressed. Across 80 changing-illumination
cases in both LK modes the worst EPE increase was +0.004469 px. Some textured
cases have better angular alignment but slightly reduced speed. The user
approved delivery with these limits disclosed; in-game review is still needed.

## Latest optimization and API correctness checks

Trust shares only immutable LumaNow/DepthBefore samples at fixed original
coordinates: 10×10 halo for an 8×8 group, 200 floats / 800 bytes. Reprojected
samples, per-pixel weights, residuals, confidence, candidates and decisions stay
private. All lanes reach the barrier before padded pixels are skipped. No
motion-dependent LK window is shared. Isolation comparisons use the same
quality revision and rounding: all four native size/refinement combinations
and six actual shader API/refinement controls preserved captured flow/masks.

DX11 allocates a dedicated 200-float Trust scratch. Vulkan reuses the existing
1,152-float pool because the universal dedicated allocation regressed actual
warm Vulkan performance. SPIR-V inspection confirms the extra 800-byte object
is absent. That rejected regression is not counted as a performance gain.

The generator's exact `__RENDERER__ == 0x20000` checks missed versioned Vulkan
identifiers. Both now select the renderer family with
`(__RENDERER__ & 0xF0000) == 0x20000`, activating the intended scratch layout and
explicit nearest-even helper. The [Khronos GLSL.std.450 specification](https://registry.khronos.org/SPIR-V/specs/unified1/GLSL.std.450.html)
allows implementation-selected tie handling for Round; the native-compatible
helper is therefore kept for portability. Normal-LK captures on this RTX 3080
matched the old rounding control; no visual gain is attributed to this alone.

Production evidence filtering is permanently on in native and generated shader
(flags 7), with no user toggle. This and the real native inverse-refinement
function/toggle were already in the preceding build; this round verifies them,
not claims them as newly added features. Debug code/arrow gain/colors did not
change in the last round; functional improvements are measured in delivered XY.

Six final real shader controls cover DX11/Vulkan/x86 DX11 producer × normal /
inverse refinement. Optional sky rejection, depth convention/mirroring/point
alignment, tiny nonzero depth, missing-depth fallback, validation transitions,
seven finite debug outputs and DisplayDepth parity passed. Final fixed-sample
cache comparisons: no differences in 936 DX11, 1,379 Vulkan and 936 x86 DX11
non-COLOR buffers per refinement variant. COLOR is excluded only for a fixture
UI clock. Feeder XY exactly equals the producer guide in these captures.

Ten actual NR composition controls (native DX11/Vulkan and shader+Feeder
DX11/Vulkan/Host64 × model on/off), eight before/after pairs each: all passed.
Applied model changes output; ApplyModel off leaves it identical. Host captures
are in the consumer: they prove model evaluation/composition, not client display
of a returned frame. Full AOT/source consistency plus native camera, debug and
Trust GPU tests passed. Earlier XeFG/FSR integration checks are separate prior
evidence, not a rerun of every FG backend on this final candidate. DLSS-G and
DX12-game F5Low coverage are not claimed. The Vulkan menu-open FPS issue remains
unresolved. No assertion of "all routes/all hardware certified" is made.
