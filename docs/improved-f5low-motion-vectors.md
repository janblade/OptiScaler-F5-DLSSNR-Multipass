# improved F5Low motion vectors

## Scope and status

OptiScaler source changes are based on Janblade F5Low 0.1.31
(`aeb439482c13dd7364ee2bef16fa41b902be6f98`), integrated with 0.1.32
(`fadc3bf67f6c8548e94914baec3f2e551ba8d8a4`). Upstream main subsequently
advanced to 0.1.33. Results below describe this tested branch, not an untested
0.1.33 merge. No upstream release or main branch is being published/replaced.

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

The final low-contrast correction protects coherent independent advection when
it explains the image better than the camera, even if the structure score is
small. It requires a relative and absolute photometric gain, local directional
support, and agreement with that pixel's previous **raw matched** correspondence
at its own reprojected position. Using raw history prevents camera-suppressed
history from locking real motion out permanently. A high-contrast mixed-window
guard protects horizon boundaries. No global confidence boost or semantic
sky/smoke detector is introduced.

In the controlled 8-bit test, a patch moving 0.8 px/frame over a static
background at contrast 0.04 improved final endpoint error from about 0.814 to
0.261 px; at contrast 0.12, about 0.544 to 0.118 px. With a 0.7 px/frame camera
pan and independent 0.8 px/frame motion at contrast 0.12, about 0.474 to 0.083 px.
These are test cases, not a promise for every fog material. Motion of only
0.05 px/frame in low-contrast RGBA8 input may already lack adequate matching
evidence before fusion and is not claimed solved.

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

Final run data and the performance target are appended below. GPU timings must
include the complete estimator, trust/disocclusion and full-resolution guide;
debug rendering, NR, readback and cross-process transport are excluded from
that native total. ReShade and Feeder technique costs are listed separately.
Stationary and moving scenes, original revision, exact dimensions, warmup,
sample counts, API and repeated runs are recorded. Game FPS is not converted
into an allegedly measured estimator-only gain.
