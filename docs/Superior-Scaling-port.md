# Superior Scaling port

Upstream: janblade `main`, `2a5be38d` (0.1.33). Superior reconstruction reference: mattjaas v59, `914d8d1f50f2d52b11f9bacd7b6bf8fd7966d9e2`.

## Scope

**Superior Scaling is an Enlargement option, not a replacement for janblade's NR pipeline.**
It changes the downscale to the fixed custom Area, inserts cached Low36/Cross5 inter-passes according to the 0.15 worsening-gap rule,
and stabilizes the working residual with private DLSS SR in DLAA mode. The final v59 radius-one estimator and low/high shaping
are folded into janblade's final composition, without an extra full-resolution reconstruction texture/pass.

The ordinary janblade exposure system runs unchanged: Auto, scanned/game/manual sources, metering, adaptation, trim, anchors and Tune.
Superior does not remap exposure choices or change their defaults. The input encoding, HDR curves, LUT, input styles, scene cut,
model/detail reuse, edit cache, diagnostics and output composition remain in the shared host path and keep their UI controls.
Only Bilinear/SGSR1 selection is replaced when Superior is active. Extra final-colour recovery and exposure controls from the initial port were removed.
The private DLSS feature still forces HDR + Auto Exposure and uses unit exposure, independently of janblade's NR input exposure.

The UI adds **DLSS to temporally stabilize NR**: A (higher performance) and K (slower, more stable in motion).
NR model A/B is independent. Model resolution and pass settings stay configurable.
At 100%, enlargement/private DLSS are inactive; native guide units are still corrected from their actual valid/reference dimensions.
Below 100%, depth and MV are matched to the working grid with independent regions/offsets; the private DLSS path reuses current depth.
Edge compression keeps its packed NR guides and unpacks before temporal enlargement. Its inter-pass correction operates on the packed model grid.
The fixed Area/inter-pass/carrier/low shaders retain v59 bytecode and their lazy PSOs. The host composition PSOs use janblade's existing 256-byte ABI.

| NR model | Style | High/medium | Low |
| --- | --- | ---: | ---: |
| A main | Standard | 0.800 | 0.661 |
| A main | Natural | 0.750 | 0.787 |
| A main | Cinematic | 0.950 | 0.784 |
| B small | Standard | 0.950 | 0.861 |
| B small | Natural | 0.925 | 0.838 |
| B small | Cinematic | 0.955 | 0.861 |

Gains use the v59 resolution correction and Structure * Intensity. High/medium multiply within a block; low is the block's arithmetic mean.
An inter-pass runs before the next pass only if the absolute high/low gap would both increase and exceed 0.15.
Final shaping uses only the tail after the last successful inter-pass. Carrier K=1, fixed linear 0.5 encoding, guided radius 1, range sigma .015, spatial sigma 3, strength 1.
No K/headroom analysis is introduced. Native Vulkan is outside the original v59 implementation; DirectX 12 and supported D3D12 bridges are required.

## Install

Use janblade's package and matching NR runtime, plus the DLSS SR runtime required for private temporal stabilization.
Replace the OptiScaler DLL with the build from this branch. Shaders are embedded; no loose HLSL/CSO files are needed.
The Actions artifact also contains the matching `a/` runtime files.

## Validation

The integration was narrowed after commit `21fb2960`: that earlier build replaced unrelated janblade input/exposure/composition and is superseded. Check the build matching the current commit.
CPU contracts cover fixed values, A/K, A/B gains (including mixed passes), exact raster rounding, native/reduced MV units,
strict scheduling, decreasing gaps, low means, failed boundaries, PSO guards, independent guide regions and surface formats.
Recompiling the Superior shaders reproduced the v59 bytecode hashes. The default janblade shader bytecode is unchanged; new Superior-only PSOs compile its shared composition with the v59 residual estimator.

The corrected host integration passed **768 GPU comparisons / 1,991,424 words**, bit-exact against
same-compiler upstream composition and an independently extracted v59 estimator inside that composition.
Coverage: eight HDR curves, SDR, manual/game/automatic exposure, exposure anchors, gamma 2.2/PQ encoding,
clean output, skin protection, split comparison, odd dimensions and FP16/FP32 targets.
The shipped legacy shader blob also remains byte-identical to upstream. Its original compiler settings produce
some rounding differences from a fresh compile, so it is preserved rather than regenerated.

The older tests below validate frozen v59 components, **not the corrected integration as a whole**.
They ran on **NVIDIA GeForce RTX 4090, driver 616.56**:

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

Historical timing before the integration correction; this does not measure the new host composition.

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
./tools/test_superior_port.ps1 -GpuHost
./tools/test_superior_port.ps1 -GpuQuality -ReferenceShaderDirectory <v59-checkout>/OptiScaler/shaders/dlssnr/precompile
./tools/test_superior_port.ps1 -GpuTiming -ReferenceShaderDirectory <v59-checkout>/OptiScaler/shaders/dlssnr/precompile
```

GPU timing requires an otherwise idle GPU. For full validation, compare the original v59 DLL against the port in the same game,
scene/camera, output resolution, DLSS/NR runtimes and explicit settings (including exposure trim and model precision).
Warm each independently and alternate ABBA. Start with the user's 2-pass Cinematic at 65%, then P50/P59 and native 100%,
A/K SR, A/B/mixed NR, HDR/SDR, motion/disocclusion, resolution and preset switches, hold and crop offsets.
Record GPU milliseconds, medians and spread, not only FPS. Compare captured output and moving footage.
No full-game result or native Vulkan implementation is claimed by the headless tests.
The corrected complete path has not been timed against v59; equal total performance is not established.

## Repository and PR route

`mattjaas/OptiScaler-F5-Superior-Scaling` is an independent repository retaining janblade's full history and an upstream remote.
GitHub allows only one personal fork in a fork network, so a matching `codex/superior-scaling-port` branch in
`mattjaas/OptiScaler-DLSSNR-PreSR-Multipass_Autoexposure` supplies the PR route to janblade.
The original repository's existing branches remain unchanged. No upstream PR is opened automatically.
