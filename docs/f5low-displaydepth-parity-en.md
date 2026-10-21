# DisplayDepth parity and depth interpretation — 20 October 2026



## Findings



The estimator and NR receive aligned **raw depth**. The calibration pass changes UV coordinates/size, not depth values. The display multiplier (4500 in the user's SH3 setup), far plane and linearization switch affect the diagnostic only. F5Low also derives an internal reciprocal-depth proxy for surface weighting; that proxy is not the raw depth supplied to NR. The selector identifies this scene as reversed Z. Raw emulated GS depth and the chosen display settings do not establish calibrated metric camera distance; a physically exact projection cannot be inferred from the screenshot.



Automatic source selection is preserved at source size 0,0. The current SH3 setting, 2048×2048, filters the eligible dimensions while the normal selector still chooses among valid candidates. The automatic aspect heuristic can reject square resources; the explicit SH3 dimensions avoid that rejection.



The previous custom normal reconstruction was not identical to ReShade DisplayDepth: it used an aspect-adjusted reconstruction, chose the smaller depth difference on each axis, and sampled the already point-resized estimator copy. A repeated neighbor could erase the true local slope. A CPU single-plane reproduction showed visible stripes, with 75.28% of interior normals more than 10 degrees from the analytic reference. This reproduces a diagnostic defect, not proof of corrupted NR input. Older real SH3 depth captures were also inspected; they are not captures of the newly attached scene.



## Correction



- Original DisplayDepth center/north/east normal reconstruction, screen coordinates, cross-product order and ordered dithering.

- Linear sampling from the **original selected depth source**, with the same UV scale/offset calibration. The model/estimator continue using their existing point-aligned raw depth.

- Dedicated full-picture-resolution depth preview in native OptiScaler and FX; other diagnostics retain their previous resolution. Separate retained native targets allow switching views without replacing an in-flight target. Native allocation is lazy. ReShade can retain the full-resolution target while its effect is loaded (about 25 MiB at 3413×1920); rendering occurs only while the debug technique runs. This view may cost more while enabled.

- Missing-depth checker and diagnostic legend retained. Degenerate normal NaNs are explicitly rendered black before floating-point intermediates; the original UNORM display also converts them to black.

- Original, unmodified DisplayDepth.fx and its localization include restored in both games. Their downloaded content hashes are recorded in reshade-displaydepth-reference.json. The test-only reference effect/add-on is not installed in either game.

- Default diagnostic linearization enabled. Existing saved values and all user calibration/route settings preserved by installation.



## GPU validation



The independent native test embeds the original DisplayDepth normal and dither functions and compares production Visualise → Pass → AOT against a separately compiled reference. Four cases cover normal/reversed depth, raw/linear preview, different source/output sizes, scale/offset calibration and clear-depth areas. It also checks byte-identical motion/model-depth data, full/half preview switching, missing-depth handling and device health. Details are in native-reference-gpu.log.



Each actual ReShade route renders the original DisplayDepth PS into an independent full-resolution RGBA8 target and compares visible RGB outside our legend rows 0..39:



- dx11: 617283 checked RGB channels, maximum error 0/255.
- vulkan: 617283 checked RGB channels, maximum error 1/255.
- host64: 617283 checked RGB channels, maximum error 0/255.



Tolerance is 1/255. Alpha is excluded because the original float3 DisplayDepth PS does not define the RGBA test target's alpha, while our overlay intentionally remains opaque. These tests establish reference appearance on the controlled inputs, not full cross-API bitwise estimator equivalence. The 55-frame vector/Feeder controls also passed on all three routes: selected depth dimensions/orientation, sky cutoff controls, no-depth fallback, history validation, seven finite diagnostics and exact Feeder XY transfer.



The original DisplayDepth also shows artifacts in the user's current scene. Missing geometry/clear-depth holes or quantization in that original source remain visible; this correction does not invent depth to hide them. The new installation still needs the user's visual confirmation in that scene.



## Complete synthetic vector cost versus pristine F5Low



Original reference aeb439482c13dd7364ee2bef16fa41b902be6f98; RTX 3080; 3413×1920; identical controlled inputs/settings; original/current alternating twice for stationary/moving scenes; 64 warmup and 160 measured frames per run. No game or other GPU test during measurement. Includes luma/pyramid, Match/LK, median, filtering/camera/global, trust and full guide. Excludes NR, debug rendering, CPU readback and shader/Host transport. These are synthetic D3D12 vector timings, not game FPS or debug/transport cost.



| Scene | Original | Current | Difference |
|---|---:|---:|---:|
| stationary | 3.500 ms | 2.269 ms | -1.230 ms (-35.16%) |
| moving | 3.500 ms | 3.043 ms | -0.457 ms (-13.05%) |



Including the current per-frame depth-alignment pass (original receives the same mapped depth before its interval; current maps the 2048-square input with scaleY 1.143 and offsets −4,+117 inside its interval):



| Scene | Original | Current | Difference |
|---|---:|---:|---:|
| stationary | 3.497 ms | 2.292 ms | -1.205 ms (-34.46%) |
| moving | 3.502 ms | 3.082 ms | -0.420 ms (-11.99%) |



Estimator FX SHA256 remains 324ee1e706413e12a2eacb44126ba4c3b8ae52427bc6162bf35d9f61bcfea0a0. This display correction does not introduce a new estimator optimization; the cumulative original/current gains above already existed. The Vulkan menu/FPS issue remains unresolved. Frame generation remains experimental until actual interpolated frames are checked for disocclusion, independent objects, particles/transparency and HUD. No PR or package.

