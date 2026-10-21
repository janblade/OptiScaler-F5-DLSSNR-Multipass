# F5Low x64 integration and optimization audit

## Scope

OptiScaler F5Low and a mechanically generated ReShade transplant of its estimator, camera fitting, refinement, reconstruction and trust kernels. PCSX2 uses x64 Vulkan without a Host; Metal Gear Rising uses its x86 Feeder client and the x64 Host. Requiem is outside this update. Visual approval in the games is still required before any package or pull request.

## Correctness changes

- Preserve very small nonzero reversed hardware depth. The former 1e-6 floor classified PCSX2 geometry as a single distant surface. Only the actual clear plane is excluded from camera fitting. Raw depth exported to NR is not linearized or rescaled.
- Make depth-guided motion finite when a clear-depth hole has four nearby proxy texels: preserve ordinary weights; use the nearest relative-depth support only if the original sum is zero or invalid. The reproduced case changed from 230,400 nonfinite outputs to zero in 925,604 outputs.
- Recover well-conditioned weak-texture LK solves only with explained temporal residual and a verified improvement of that pixel's original weighted matching objective. This is an intentional quality change, separate from data-preserving optimizations.
- Validate regional and camera proposals independently per pixel. Require compatible depth and appearance, protect exact static features, and preserve photometrically confirmed independent low-contrast motion, including smoke. Dark areas can legitimately move; their brightness alone does not justify zero motion.
- Publish selected-motion support separately from history rejection. Support rises from 0 (weak) to 1 (strong). A rejection mask rises toward 1 for rejected history. Neither quantity is a calibrated probability.
- Preserve nearest-even candidate rounding in the Vulkan shader. Keep SDR luma reduction arithmetic aligned with the native implementation.
- Provider 5 exports stored pixel XY directly. Feeder validation adds a mask and does not replace F5Low XY with its own static or geometric decision. The F5Low history mask remains mandatory when extra Feeder validation is off. A continuity stamp prevents stale validation history on re-enabling the checks.

## Data-preserving optimizations

- Reuse identical displaced LK gradient endpoints only within the same pixel; check constructed coordinates for equality. No common displaced window is used across neighboring pixels.
- Reuse duplicate candidate costs within one pixel, preserving evaluation and tie order.
- Stop only at an exact floating-point fixed point, preserving final confidence and trace counters.
- Sum invariant weights once per pixel in the original order. Reuse this value across that pixel's costs and refinement steps.
- Use constant integer tile pitches for the same cooperative image addresses. Remove an LDS support array whose values were only read by their own output pixels.
- Skip redundant depth arithmetic when all four guide depths are identical, preserving the original bilinear floor and accumulation order.
- Fuse compatible ReShade passes within DX11 UAV limits. Omit unused legacy UV publication for provider 5 and skip full-resolution Feeder validation-history copies when those extra checks are off.

Raw source samples may be shared; this does not authorize sharing neighboring matching decisions, candidate offsets, residuals, confidence, weights or motion-dependent LK coordinates. The regional/camera model is a separately tested quality regularization, not a sample-sharing optimization.

## Measurements and limits

RTX 3080, approximately 3412/3413 x 1920, synthetic fixtures; GPU median times, not game FPS promises. Native flow measurements exclude NR model evaluation. Shader measurements include F5Low plus the Feeder shader, with transport and NR disabled to isolate their GPU cost.

| Comparison | Before | After | Saved |
|---|---:|---:|---:|
| Native, stationary | 2.781 ms | 2.076 ms | 0.705 ms |
| Native, moving | 3.016 ms | 2.791 ms | 0.225 ms |
| DX11 shader + Feeder, moving | 3.921 ms | 3.560 ms | 0.360 ms |
| Vulkan shader + Feeder, moving, latest repeat 1 | 13.893 ms | 13.054 ms | 0.839 ms |
| Vulkan shader + Feeder, moving, latest repeat 2 | 14.035 ms | 13.097 ms | 0.939 ms |

Earlier Vulkan runs showed approximately 0.675 ms saved on stationary input but about 0.475 ms slower flow on moving input. The latest alternating repeats improved, but the variability means a universal moving-scene Vulkan speedup is not established. Background graphics load and scheduling affect these measurements; telemetry and individual reports are retained.

Optimization-only GPU snapshots compared 274,159,339 bytes with zero differences for conventional and inverse refinement where tested. This exact-output statement does not include the intentional quality corrections above. DX11 native/shader readback parity passed 208 comparisons before the final equivalent indexing cleanup; the final rerun is recorded separately.

Vulkan is functional and Feeder preserves its input XY exactly. Strict bit-exact parity between Vulkan SPIR-V and native DXBC has NOT passed: small floating-point differences can affect later discrete choices, with occasional larger downstream discrepancies. Shared source and identical sampling intent do not prove identical cross-API output.

Seven debug views, sky exclusion, missing-depth fallback, tiny valid depth, validation OFF/ON/OFF and trace independence have dedicated GPU checks. The general quality suite has two HDR perceptual-vs-legacy assertions failing in both the previous and updated versions; small regressions in some weak-line/slow-pan synthetic metrics are also recorded. These are limitations, not passed checks.

The real PCSX2 FPS increase when opening the OptiScaler menu remains unresolved. NR was still composing in both menu states. The isolated fixture did not reproduce the large increase. Menu rendering can enter downstream ReShade input; no unvalidated renderer-order change was applied.

## Removed experiments gate and debug

There is no F5LOW_INTERNAL_EXPERIMENTS requirement. The individual cost-reuse, refinement guard, evidence-filter and inverse-refinement controls work directly. Corrections are automatic. Trace is diagnostic recording and must not change vectors.

The seven public views are motion arrows, depth, final optical flow, confidence/rejection, confidence channels, history rejection, and still age. Every view has a legend. Classic confidence uses green for strong support and orange for weak or rejected input; RGB confidence uses red texture structure, green photometric fit, blue accepted history. History rejection uses white for rejected history and black for accepted history.

The shader's sky toggle excludes the exact clear-depth plane from exported motion/confidence and rejects its temporal history. It requires correct depth. Smoke or backgrounds that share clear depth are also excluded; this is not semantic sky segmentation. Default: off.

## Evidence

Local evidence is in this output directory: selected-full-build.log, tile-index-snapshot.log, tile-index-inverse-after.log, pixel-weight-total-snapshot.log, paused-browser-confirm.json and its GPU telemetry, clean-performance-vulkan.json, final-core-checks.json, final-guide-extreme.log, final-quality.log, sky-validation-final/result.json, and the individual DX11/Vulkan contract reports. Delivery and final route results are recorded separately after building/installing.

Primary rounding references: [HLSL round](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-round), [GLSL.std.450 Round/RoundEven](https://registry.khronos.org/SPIR-V/specs/1.0/GLSL.std.450.html).

## Final verification of this candidate

- All 46 native bytecode entries were regenerated from the selected sources. The measured default/trace kernels match the final entries exactly (`selected-source-bytecode-audit.json`).
- Final DX11 transplant: 208 comparisons, zero mismatching stages, NR actually composed (`contract-candidate-641x361-nr1-selected-final`).
- Final strict Vulkan transplant comparison: 443 comparisons, 132 mismatching stages; NR composed and Feeder XY was unchanged. This is a failed exact-parity check, not a passed check (`vulkan-contract-candidate-641x361-nr1-selected-final`). The reported worst error includes byte-valued rejection masks; it is not a motion magnitude in pixels.
- Four x64 routes passed: native DX11, Feeder DX11, native Vulkan, Feeder Vulkan. Each captured eight input/output frame pairs with ApplyModel on and off: on changed the image; off preserved it exactly (`selected-routes.json`).
- The x86 client / Host64 route also evaluated and composed NR with the final DLL (`selected-route-x86-Host64/result.json`). The private test fixture was corrected to wait for the first shader compilation before timing out; that fixture correction is not installed in games.
- Seven core checks passed, including the extreme guide-depth case and binary identity with trace on/off. Sky exclusion, valid tiny depth, no-depth fallback, optional validation transitions, XY preservation and all seven finite debug views passed again with the final DLL.
- Final native performance uses two alternating repeats per scene (`selected-native-performance.json`). The stationary gain was approximately 0.705 ms; the moving gain was approximately 0.225 ms. Latest shader measurements retain the variability described above.
