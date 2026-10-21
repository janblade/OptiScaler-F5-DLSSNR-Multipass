# F5Low camera motion repair — audit

## Changes

- Retains the six-level Match → Lucas–Kanade → Median → Smooth → Trust/Uncover/Guide architecture. Adds one 128-thread GPU camera-fit dispatch between Median and Smooth; no CPU readback or temporal averaging of final vectors.
- Fits the current frame's finest motion observations. Fixed stratified coverage, bounded squared structure weights, preference for farther valid depth, component-median initialization and three robust IRLS iterations prevent arbitrary local winners from owning the model.
- Uses a regularized affine model with broad support. Narrow support uses a coupled rotation/zoom/translation fit, with broad-axis coverage, inlier, residual and finite-value checks. The lower regularization bias preserves rotation when the sky occupies most of the image.
- Applies **selective geometric fusion** to weak local evidence. Each pixel validates its own original 16-sample photometric/depth/luma window, independently displaced into the previous image. Foreground silhouettes and measured near-zero motion with exact unchanged samples are protected.
- Confidence remains **0 = low, 1 = high**. The original `.z`/`F5Low::Confidence` export remains local two-axis texture structure. The orange/green view additionally considers validated camera agreement and limits support by photometric fit and accepted history. RGB keeps texture/fit/history separate. The distinct history rejection mask uses **1 = rejected**.
- Retains seven public views and legends: arrows, depth, final flow, orange/green support, RGB confidence, history rejection, still age. Debug selection does not alter motion. The actual trace-on/off outputs are identical across 58,565,808 bytes.

## Data-preserving optimization

Only identical source-image values are cached: current-image tile, raw depth samples and zero-offset previous-image samples. Normal LK uses a 14×14 current tile; inverse LK retains 16×16. Dynamic previous-image coordinates, candidate offsets/evaluation, residuals, confidence, sample windows and per-pixel weights remain independent. Guide/Median caches were rejected after their GPU timings showed no useful gain. Normal and inverse comparisons each preserved **274,159,339 bytes exactly**, including camera output, matching levels, traces and consumer guides. The camera model and weak-evidence filtering are separately validated quality changes; the byte-preserving claim applies to sample reuse.

## Same-input GPU evidence

Errors are full-resolution pixels in controlled fixtures, measured at the final consumer guide.

| Scene | Installed base EPE | New EPE | Reduction |
|---|---:|---:|---:|
| pan-horizon-no-depth | 0.329526 | 0.031613 | 90.4% |
| creep-horizon | 0.171029 | 0.032964 | 80.7% |
| static-horizon-grain | 0.904223 | 0.080578 | 91.1% |
| fast-horizon | 0.529381 | 0.088109 | 83.4% |
| textured | 0.064298 | 0.064249 | 0.1% |

With 80% sky, camera rotation error falls from **0.7970 to 0.2144 px**; direction within 30° rises from **0.12% to 99.9996%**. With 40% sky, rotation/zoom/combined errors fall from 0.6874/0.5781/0.5376 to 0.0749/0.0583/0.0533 px. Large independently moving foreground border error falls from 0.3391 to 0.2734 px with depth. Static HUD error in the rotation/zoom fixtures is below 0.0001 px. Start/stop/reversal is checked; no final-vector EMA is added.

Two HDR assertions still report FAIL: they compare perceptual luma with the newly improved legacy luma, rather than with the previously installed result. Preserve the raw log. All other legacy assertions pass. Same-mode grain error improves from 0.374 to 0.295 px in scRGB and 0.370 to 0.284 px in PQ. Clean dark HDR pan mean error increases by approximately 0.012 px; every scored sample remains within 0.5 px.

## Integration and performance

- All seven native/ReShade views pass readback parity: 231 comparisons per view. Match, Median, camera model, Smooth, final guide and Feeder XY are exact; R8 history permits one quantization step and preview permits R8/half rounding. Normal and reversed depth are covered.
- The complete 46-entry native bytecode check passes. All 41 FX entries compile to both DXBC and SPIR-V at 641×361 and 1920×1080.
- Actual NR evaluation/composition passes with ApplyModel on/off for native DX11, native Vulkan and Feeder Vulkan x64. The x86 D3D11 → Host64 route is covered by parity captures. Feeder provider 5 exports the supplied final XY without modifying it; optional validation affects history rejection. No Feeder ABI or add-on binary change was needed.
- GPU timestamps, 3412×1920, debug off, two interleaved rounds, including trust/consumer guides and excluding NR inference:

| Fixture | Base run medians (ms) | Final run medians (ms) |
|---|---:|---:|
| textured | 3.0597–3.0618 | 2.9819–3.0044 |
| weak texture | 2.8129–2.9440 | 2.8488–2.8488 |

The textured fixture improves about 2%; weak-texture cost is within run variation. Camera fitting costs about 0.070 ms. These timings do not establish an in-game FPS gain against Lumenite.

## Scope

Modified production source is in the local OptiScaler project and ReShadeMotionVectors; the FX core is mechanically generated from the native kernels. Equality requires identical inputs and settings. The native game-provided DLSS-vector route was not changed or exercised in a new game session in this round. Actual MGR/PCSX2 scene confirmation remains the user's next check. No finite image-only estimator guarantees uniquely correct motion in invisible or fully ambiguous content. Package and PR remain pending that confirmation.

## Installed configuration and artifact

MGR had internal experiments, evidence filtering and refinement guard enabled, plus ZeroMargin=0.01 and StillFrames=6. The update restores the validated normal-LK profile, disables those experiments, uses ZeroMargin=0 and StillFrames=8, and preserves arrow appearance. Native motion/input are enabled in PCSX2 and disabled in the MGR host so each installation uses one vector producer. Actual x86→Host64 NR evaluation/composition also passed with ApplyModel on and off.

Compiled x64 DLL SHA-256: `5c6d02d0a4885f9f60ca0400a520fd355af4a0b40c37e71d11ba8ef54103162a`.
