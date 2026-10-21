# Superior Scaling port

Upstream: janblade `main`, `2a5be38d` (0.1.33, including HDR output and RGBA/BGRA history fixes).
Original integration base: `fix/menu-overlay-back-buffers`, `ae7d2c4092e5aee79bf9b1e6ea7a5bd38933a2d4`.
Superior reference: mattjaas v59, `914d8d1f50f2d52b11f9bacd7b6bf8fd7966d9e2`.

## Use

Choose **NR Output > Enlargement > Superior Scaling**. Bilinear/SGSR1 is replaced by the fixed custom Area path.
**DLSS to temporally stabilize NR** selects private SR preset **A** (higher performance) or **K** (slower, more stable NR in motion).
Keep using janblade's model resolution, model preset, style, intensity, Structure and per-pass controls.
The NR model A/B and the private SR A/K are independent selections.

Below 100%, use post-SR NR with current depth and motion guides. At 100%, ordinary NR runs without enlargement or private DLSS.
DirectX 12 and its supported bridges are used. Native Vulkan does not implement Superior, as in the reference; the UI reports this restriction.
The private DLSS feature requires the normal DLSS SR runtime in addition to the NR runtime used by janblade.
Use janblade's installation package and required runtimes, replacing its OptiScaler DLL with this build.
The full Actions artifact also contains the matching `a/` runtime files; the shaders themselves are embedded in the DLL.

## Integration and fixed behavior

The port retains janblade's model creation, feature submission gate, API entry points and pass profiles.
Only the Superior input/inter-pass/output path is separate. Its 256-byte shader ABI lives in `shaders/dlssnr/superior`;
janblade's existing shader ABI and bytecode are unchanged. Lazy PSOs share the existing descriptor/constant machinery.

| Part | Behavior |
| --- | --- |
| Input raster | Exact v59 rounding, without janblade's optional 16-pixel alignment |
| Input downscale | Custom Area |
| NR guides | Independent depth/MV active regions, offsets and clipping; point-matched R32/RG32 below 100% |
| Motion units | Original game scale multiplied by active guide/reference dimensions; native 100% uses the original guides |
| Private DLSS | DLAA at the NR working raster, HDR + automatic exposure flags, unit exposure, zero jitter, frame time |
| Carrier | Fixed linear `0.5 * residual + 0.5`, range -1 to 2, K=1; no K/headroom analysis |
| Inter-pass | Fast cached Low36 + Cross5; block high products, block low arithmetic means |
| Schedule | Before the next pass only when its predicted absolute high/low gap both grows and exceeds 0.15 |
| Final gains | High product and low mean of passes since the last successful inter-pass |
| Guided resolve | Radius 1, range sigma 0.015, spatial sigma 3.0, strength 1 |
| Gain adjustment | v59 resolution exponent, then Structure * Intensity on high/medium and low |
| Final colour | Optional chromaticity/saturation recovery and smoothing, with no work when disabled |
| Resource reuse | Persistent textures, same-frame NR depth reused by DLAA, fused carrier + motion, immutable clamp descriptors |
| Lifetime | Submission/reset tracking and fence-backed retirement for Superior generations |
| Failures | No composition after failed NR evaluation; fallback PSOs and history resets retained |

All 39 frozen v59 shader source/header/bytecode files are listed with SHA-256 in `superior-v59-shaders.json`.
The optimized input encode/keep, Area, HDR meter/reduction, carrier/motion, cached inter-pass, final low 8x16,
normal resolve, SDR, HDR soft-knee and replace variants are retained. This port introduces no new precision reduction.

The merged RGBA/BGRA fix uses canonical colour formats for model/proxy resources, keeping model and private DLSS history.
Resources copied directly to/from the game (original/copyback, crop, hold, final-colour fallback) keep its exact format.
A held frame is recaptured when that exact format changes. RGBA16F/HDR resources are unchanged by this rule.

### Frequency gains at P50 and full Structure/Intensity

| NR model | Style | High/medium | Low |
| --- | --- | ---: | ---: |
| A main | Standard | 0.800 | 0.661 |
| A main | Natural | 0.750 | 0.787 |
| A main | Cinematic | 0.950 | 0.784 |
| B small | Standard | 0.950 | 0.861 |
| B small | Natural | 0.925 | 0.838 |
| B small | Cinematic | 0.955 | 0.861 |

Each pass selects its own table before scaling and block accumulation. As in janblade, passes after the first three inherit the base model preset.

### Exposure and controls

Superior preserves the v59 manual, game and automatic HDR exposure path, including trim, highlight protection and up to eight trim anchors.
The default trim values are 1 for game exposure and 5 for automatic exposure. Existing explicit trim values are respected.
Janblade's Auto or scanned-source selection resolves to the v59 automatic meter while Superior is enabled.
The separate janblade exposure scanner, readbacks, Tune, adaptive/percentile meter and input processing are bypassed in this mode.
Their controls are not shown as working Superior controls; legacy modes retain them.
HDR composition, exposure controls, ordinary output strength, hold/compare and supported debug views remain accessible.

Soft-knee, reversible and balanced curves (0..4), and linear/sRGB inputs match v59.
Janblade-only HLG/PQ/linear proxy curves and gamma-2.2/PQ input conversion are not implemented by the v59 shaders.
Unsupported input settings leave the game frame intact and report the reason, instead of interpreting it with the wrong math.
Native Vulkan and reduced pre-SR Superior likewise report their unsupported combination.

## Validation

Release x64 DLL compilation succeeded for `6f778e89` before the upstream merge; subsequent integration changes are checked by the branch build workflow.
Check the Actions run matching the exact downloaded commit for the final build result.
CPU contracts cover fixed values, A/K, A/B gains (including mixed passes), exact raster rounding, native/reduced MV units,
strict scheduling, decreasing gaps, low means, failed boundaries, PSO guards, independent guide regions and surface formats.
Recompiling the Superior shaders reproduced the v59 bytecode hashes. The legacy shader files are unchanged.

GPU validation ran on **NVIDIA GeForce RTX 4090, driver 616.56**:

| Test | Coverage | Result |
| --- | --- | --- |
| Carrier/decode/shaping | 52 cases | Bit-exact |
| Carrier + guide preparation | 240 cases, 10,957,920 words | Bit-exact |
| Encode/keep/Area | 3,840 cases, 66,528,000 words | Bit-exact |
| Exposure chain | 2,880 cases, 217,294,560 words | Zero measured numerical difference |
| HDR soft-knee resolve | 7,776 cases, 43,830,720 words | Bit-exact |
| Final low | 198 cases, 54,648 words | Bit-exact |
| Cached Low36/Cross5 | 480 comparisons | Zero measured numerical difference |

These fixtures cover FP16/FP32 textures, signed HDR, non-finite values, borders, odd dimensions and cropped guides where applicable.
Some compare the frozen generic/reference path against its optimized specialization; others compare original and port blobs directly.
They do not execute proprietary NR/DLSS inference or the complete game's command stream.

### GPU timing scope

Resident **Low64 + guided resolve only**, synthetic 3840x2160 image, P50/P59/P65, SDR and HDR 0/2/4.
Eight blocks alternate ABBA/BAAB with separate warm-up, 40 quartets per block and four GPU repetitions per timestamp:
640 samples per variant/profile/scale. Both use the same v59 resolve bytecode. No NR or DLSS inference is included.

| Profile | P50 original / port ms | P59 original / port ms | P65 original / port ms |
| --- | --- | --- | --- |
| SDR | 0.340736 / 0.340736 | 0.359040 / 0.360192 | 0.361472 / 0.361472 |
| HDR 0 | 0.300672 / 0.300288 | 0.295680 / 0.295168 | 0.300800 / 0.300160 |
| HDR 2 | 0.262912 / 0.262656 | 0.279040 / 0.278784 | 0.293632 / 0.293632 |
| HDR 4 | 0.261376 / 0.261376 | 0.278784 / 0.278784 | 0.292608 / 0.292608 |

Paired median differences are -0.000128 to +0.000384 ms, much smaller than observed variability
(per-variant standard deviation about 0.038-0.056 ms). There is no resolved timing difference in this fixture.
[Raw samples](validation/superior-port-v59-timing.csv) and [statistics/metadata](validation/superior-port-v59-timing.json) are committed.
This does **not** prove equal total in-game performance or complete temporal image equivalence.

### Reproduce and complete game validation

From Windows with MSVC x64 tools and Python:

```powershell
./tools/test_superior_port.ps1
./tools/test_superior_port.ps1 -GpuQuality -ReferenceShaderDirectory <v59-checkout>/OptiScaler/shaders/dlssnr/precompile
./tools/test_superior_port.ps1 -GpuTiming -ReferenceShaderDirectory <v59-checkout>/OptiScaler/shaders/dlssnr/precompile
```

GPU timing requires an otherwise idle GPU. For full validation, compare the original v59 DLL against the port in the same game,
scene/camera, output resolution, DLSS/NR runtimes and explicit settings (including exposure trim and model precision).
Warm each independently and alternate ABBA. Start with the user's 2-pass Cinematic at 65%, then P50/P59 and native 100%,
A/K SR, A/B/mixed NR, HDR/SDR, motion/disocclusion, resolution and preset switches, hold and crop offsets.
Record GPU milliseconds, medians and spread, not only FPS. Compare captured output and moving footage.
No full-game result or native Vulkan implementation is claimed by the headless tests.

## Repository and PR route

`mattjaas/OptiScaler-F5-Superior-Scaling` is an independent repository retaining janblade's full history and an upstream remote.
GitHub allows only one personal fork in a fork network, so a matching `codex/superior-scaling-port` branch in
`mattjaas/OptiScaler-DLSSNR-PreSR-Multipass_Autoexposure` supplies the PR route to janblade.
The original repository's existing branches remain unchanged. No upstream PR is opened automatically.
