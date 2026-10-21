# F5Low route review — 2026-10-19

## Applied changes

- The corrected estimator is the only production path. Removed the public baseline, legacy output, internal experiments and sample-sharing permutations. Candidate-cost reuse, per-pixel LK acceptance and vector-evidence filtering run automatically. Camera/history/depth and the requested sky control remain available.
- Source-image sample reuse is built in. It does not share candidates, residuals, confidence decisions or displaced LK coordinates. Native AOT contains the same 46 shader entries; pure sample-sharing selector removal preserved all 46 DXBC blobs before the separate photometric correction.
- Raw hardware-depth reads in the motion shader and Feeder now use point/clamp sampling and apply the mirror coordinate transform. This avoids interpolated depths across different surfaces. It does not change a user's selected buffer or orientation/scale settings.
- Photometric acceptance at image borders now evaluates the same clamped current-window coordinates as conventional/inverse LK. Integer candidate matching remains unchanged.
- Generated ReShade entry points reuse workgroup scratch allocation between separate dispatches. Simultaneously live arrays retain disjoint ranges. No values survive or are shared between dispatches. This addresses excessive Vulkan workgroup allocation caused by declarations from unused entry points being retained in SPIR-V.
- Seven public debug views, with legends: motion arrows, raw depth, final flow, orange/green confidence with rejection, RGB confidence channels, history rejection, still age. Optional trace remains a developer capture, compiled out by default. Removed modes are not compatibility options.

## What direction correction actually does

Matching still makes per-pixel decisions. Candidate/LK changes and camera/regional regularization are different changes: the camera/regional proposal is applied in filtering and validated against each pixel's own depth/luminance-weighted photometric window. It is not merely confidence hysteresis inside Match. The earlier recommendation to compare stages was an investigation plan; it was not proof that Smooth weight changes caused the original instability. Current stage attribution is saved in final-stage-attribution.log. For the upper band of the controlled no-depth horizon pan, Match has EPE 0.481 px, 70.3% aligned within 30 degrees and jitter 0.413 px; Smooth reaches 0.040 px, 100% aligned and 0.054 px jitter. In that case the inconsistency already exists before filtering, and filtering reduces it. This does not establish the cause in every real scene. A coherent-looking field alone does not prove correct motion, especially on independently moving smoke/HUD.

## Verified and unresolved

| Check | Result and scope |
|---|---|
| Full native transplant, DX11 | PASS: 208 stage comparisons, zero differences, including delivered guide; actual NR composition also confirmed. |
| Vulkan scratch reuse | PASS: same 24 frozen inputs, 551 valid stage buffers bit-exact before/after. Uninitialized depth, first previous luma and disabled preview excluded explicitly. |
| Depth/sky/controls/seven views | PASS in controlled DX11, Vulkan and x86/Host64 runs, including point resampling, mirror, normal/reversed depth and unavailable-depth transitions. Provider-5 Feeder XY stays bit-exact; optional Feeder validation affects only the mask. |
| Native DX11/Vulkan and Feeder DX11/Vulkan x64 NR | PASS: eight captured image pairs per model state/route; model ON changes output, OFF leaves input exact. This checks composition, not optical-flow correctness. |
| x86 producer / Host64 | See final-host64-nr.log and depth-controls-host64-normal-mirror/result.json for current binary results. |
| Trace independence | PASS: final vectors with trace ON/OFF have identical bytes. Trace cannot be required to improve production quality. |
| Vulkan vs native strict numerical parity | NOT passed: 443 comparisons, 133 mismatching stages in the pre-reuse capture. Small arithmetic differences can alter later discrete decisions. The worst byte-valued mask difference is not a vector magnitude. Scratch reuse preserved Vulkan results, but does not solve this cross-backend difference. |
| Full quality suite | NOT fully passed: exit 1. Two HDR criteria already failed in the earlier version; the depth-edge relative-gain criterion also fails because the no-depth score improved, making its demanded extra gain unattainable. Actual values and all other results remain in final-quality.log. Tests were not weakened. |
| Actual MGR sky/directions | Pending real-scene capture. The sky switch detects clear-depth output, not semantic sky. A sky quad at a non-clear depth will not necessarily be classified. A controlled pass does not resolve this user's report. |
| PCSX2 Vulkan menu/FPS bug | Unresolved. Fresh current-source fixture: NR composed with both menu states; steady CPU/Present interval 45.24 ms closed versus 45.78 ms open (initial closed warmup 49.19 ms). It did not reproduce the large gain. F5Low and Feeder/NR GPU timings are stored in vulkan-timing-candidate-nr1-menu1-route-review-final-menu/result.json. No claim that scratch reuse fixes the real PCSX2 menu behavior. |

## Required original-baseline synthetic measurement

RTX 3080, 3413×1920, 64 warmup and 160 measured frames per run, alternating repeats. Original: untouched tracked F5Low/TrustMask/AOT at commit aeb439482c13dd7364ee2bef16fa41b902be6f98 (0.1.31). Full native GPU vector cost includes luma, pyramid, matching/LK, median, filtering/camera/global, trust and guide reconstruction; excludes NR, debug, readback and API transport. Values are medians, not a game-FPS guarantee.

| Scene / Cena | Original (ms) | Current / Atual (ms) | Delta |
|---|---:|---:|---:|
| stationary | 3.501 | 2.257 | -1.244 (-35.5%) |
| moving | 3.517 | 2.970 | -0.547 (-15.6%) |

## Shader-specific Vulkan saving

Before/after scratch allocation reuse: **12.772 → 4.818 ms**, **-7.954 ms (-62.3%)**, two alternating runs each, final steady segment. NR, transport, debug and menu disabled. Feeder time is recorded separately. These are not the pristine-original comparison above. DX11 shader timings are stored separately under the route-review-scratch tag.

## Real-game diagnosis

The temporary read-only addon in MGR captures three frames when F10 is pressed with the F5Low producer enabled. Files appear in F5Low-route-capture under the game folder. It records selected raw depth, guide depth, matching/median/filtering, camera, luma/history, confidence, final guide and Feeder inputs. Only the three requested capture frames incur GPU readback stalls. It does not edit uniforms, depth or vectors. Remove the temporary addon after the capture is analyzed.

No package, PR, publication, backups or Requiem changes were made for this review. Game installation and hashes are recorded separately in installations.json; real-scene confirmation remains pending.
