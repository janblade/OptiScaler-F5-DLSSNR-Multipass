# F5Low integration and depth preview audit — 20 October 2026

## Latest repair: white depth preview and split normals/depth

The production Visualise() correctly supplied the display multiplier and far plane, but the shared Pass() then overwrote both with matching-stage constants. This made the reversed, very small PS2 depth values turn white during preview linearization. Pass() now preserves these constants for the visualization shader; matching constants are unchanged for estimator passes.

The existing Depth debug is now **Normals / depth**: reconstructed view normals on the left, depth on the right. Both halves retain the original scene coordinates. A thin divider separates them; missing depth produces a magenta checker. Normals are reconstructed from depth using an assumed projection, not read from engine G-buffer normals. Edge differences choose the smaller depth change to reduce crossing surfaces. The seven public debug choices remain available.

The correction exists in native OptiScaler and the mechanically generated ReShade shader. Preview multiplier/linearization/far plane affect only this diagnostic. NR and the estimator still consume aligned raw depth. Estimator FX core SHA256 remained 324ee1e706413e12a2eacb44126ba4c3b8ae52427bc6162bf35d9f61bcfea0a0.

### Evidence

- Real production D3D12 Visualise → Pass → AOT regression: old constant-overwrite behavior fails 4029/4029 depth checks; corrected behavior fails 0/4029 (RGBA8 tolerance 1.51/255). 3876 colored normal pixels were produced. Raw/linear controls and missing-depth checker passed.
- Motion field and model depth remained byte-identical before and after debug rendering.
- Fresh compilation of all 46 AOT kernels and Release OptiScaler build succeeded.
- Updated 55-frame route controls passed on DX11, Vulkan and x86 client/Host64: depth orientation/size, sky cutoff controls, no-depth fallback, validation transitions, Feeder XY identity and seven finite debug views.
- The new normals/depth view has synthetic GPU validation; its appearance in the user's actual Silent Hill 3 scene awaits confirmation. Earlier actual-game depth/NR captures described below predate this final display correction.

## Integration fixes completed during this work

1. **Feeder NR placement:** a validated external frame-producer tuple (contract version, producer ID, host mode and cadence) now identifies the Feeder evaluation seam. NR processes that completed image there, instead of waiting for a Present on the private D3D12 device. Missing/invalid tuples retain the existing native-game placement. Eleven tuple cases and actual model-on/model-off checks passed in DX11, Vulkan and Host64; model-off output was unchanged.
2. **Square depth selection:** ReShade's aspect heuristic rejected PCSX2 square depth surfaces after restart. It is disabled in that installation while the user's manual alignment is preserved. Native OptiScaler gained explicit source dimensions and point-sampled scale/offset calibration, shared by its DX11/DX12/Vulkan depth adapters. Explicit dimensions bypass only the aspect test, not resource validity. Identity calibration adds no GPU pass.
3. **Raw depth identity:** Feeder provider 5 now uses the same integer pixel-center arithmetic as the motion provider. Actual PCSX2 captures confirm byte-identical raw depth and Feeder depth, exact guide XY transfer, and depth availability. Actual native PCSX2 captures confirm selected 2048×2048 depth and ongoing NR/depth use.
4. **Feeder instrumentation overhead:** routine stale-frame readbacks/hashes were removed from normal play; existing trace mode still enables them. No consistent additional GPU speedup was demonstrated for this change. Do not attribute the vector timing gains below to this instrumentation change.

## Full synthetic vector cost versus pristine original

Reference commit: aeb439482c13dd7364ee2bef16fa41b902be6f98. RTX 3080, 3413×1920, same controlled input/settings, 64 warmup + 160 measured frames, original/current alternating twice for stationary and moving cases. Numbers aggregate run medians. No games or concurrent GPU tests ran during timing.

The interval includes luma/pyramid, Match/Lucas–Kanade, median, filtering/camera/global, trust and full guide reconstruction. It excludes NR inference, debug rendering, CPU readback and shader/Host transport. These are synthetic D3D12 vector costs, not game FPS or Vulkan/Host end-to-end timings.

| Scene | Original | Current | Difference |
|---|---:|---:|---:|
| stationary | 3.497 ms | 2.256 ms | -1.241 ms (-35.49%) |
| moving | 3.500 ms | 3.041 ms | -0.459 ms (-13.11%) |

### Including the current per-frame depth-alignment pass

Both algorithms see the same calibrated depth. Original receives its mapped copy before the timed interval; current maps a 2048×2048 source to the picture each frame, with scaleY 1.143 and pixel offsets −4,+117, inside its timed interval.

| Scene | Original | Current | Difference |
|---|---:|---:|---:|
| stationary | 3.497 ms | 2.303 ms | -1.194 ms (-34.14%) |
| moving | 3.500 ms | 3.075 ms | -0.425 ms (-12.14%) |

The cumulative estimator improvements predate this display repair. This repair did not change the estimator math. The separate Vulkan Feeder transport A/B did not demonstrate an extra GPU gain. The reported PCSX2 FPS increase when opening the OptiScaler menu remains unresolved; the current evidence does not establish it as fixed.

## Installation and limits

Updated OptiScaler x64 DLL: PCSX2/winmm.dll and MGR/host64/winmm.dll. Updated provider, shared core, debug and Feeder shaders in both games and both modified source trees. SHA256 equality and PE architectures verified. User INIs/presets were unchanged by final installation. Temporary manual-capture add-ons/triggers were removed. No Requiem changes, PR or package.

Current PCSX2 depth calibration is specific to the user's Silent Hill 3 setup (2048 square, scaleY 1.143, offsets −4,+117, reversed depth, display multiplier 4500, far plane 1000). Other games may require different settings. Sky cutoff classifies raw far depth, not semantic sky; sky/smoke sharing depth cannot be separated reliably by depth alone. Strict cross-API bitwise estimator parity has not been established; route functionality and Feeder transfer are independently tested. Previous quality limitations remain recorded in the preceding repair audit.

Evidence files: depth-normal-debug/production-visualizer-regression.json, route-controls.json, estimator-core-unchanged.json, installation-verification.json, synthetic-total-summary.json, calibrated-total-summary.json, native-depth-debug-gpu.log; parent directory: selected-routes.json, host64-finished-picture/result.json, pcsx2-final-shader-result.json, pcsx2-native-result.json, calibration-aot-check.json, depth-regressions.log.
