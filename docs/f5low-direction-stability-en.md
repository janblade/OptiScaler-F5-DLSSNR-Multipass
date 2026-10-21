# F5Low direction stability — audit, 2026-10-18

## Changes

- Fixed MGR startup selection: both `PresetPath` and `StartupPresetPath` select `F5Low-Corrected.ini`. The old startup key was overriding the previously announced corrected preset. The comparison preset is preserved.
- Production FX specializes the same ordinary LK / cost-reuse path as native. Previously rejected evidence filtering, inverse refinement and generic photometric acceptance remain available for internal comparisons, and cannot be silently activated by old preset values in production.
- In ambiguous texture, Smooth expands its motion support modestly and applies a luma noise floor near black. It no longer confines every weak estimate to a narrow gate around its own potentially arbitrary centre. Strong support keeps the original motion/luma ranges. No blanket temporal EMA, dark/sky zeroing, extra camera fitting or SGF was introduced.
- Expanded support respects depth surfaces. Reuses the matching surface penalty, with the existing 0.02 floor. It is applied only where ambiguity expands the filter; identical depth skips the ratio. The extra exponential was folded into the existing weight.
- Ordinary LK stops repeating the same unresolved tensor when the determinant cannot supply an update. It also reuses the already loaded current window. Integer zero costs and immediately repeated coarse candidates are reused without changing candidate ordering or tie preference.
- Smooth uses 16×16 cooperative groups over a 24×24 halo tile, versus 8×8 over 16×16. Tile entries per output fall from 4 to 2.25. Output arithmetic order is preserved; this is not a claim of a 44% GPU-time saving.
- Arrow drawing skips roots outside its support and skips hue calculations for uncovered pixels. FX uses a dedicated linear guide sampler to match the native visualizer while retaining point sampling for exported motion.
- Seven public views only: arrows, depth, final optical flow, green/orange confidence, RGB confidence channels, history rejection, still age. Every view has an automatic on-screen legend; native inline/expanded views also include explanatory text.
- Still age reads the existing counter without enabling three trace buffers. Fixed its FX current-frame readiness flag: the previous-age flag initially hid the current counter for one frame.
- Removed the native component-mask selector from this UI. It previously wrote the same TrustMask debug setting used by the consumer. Native production now explicitly uses the final combined mask. These diagnostic selections write only the preview.

## Confidence meaning

Green/orange is a conservative evidence display: the minimum of texture structure, photometric fit and accepted F5Low history. Green means strong support; orange means weak support or rejection. It does not newly reject or zero motion. RGB shows those signals separately (R texture, G photometric, B accepted history). The separate grey history view displays the actual F5Low rejection mask: white rejects, black accepts. Optional Feeder checks may increase consumer rejection beyond that native mask; the installed preset keeps those extra checks off. These views do not measure private neural-model history or a calibrated probability of vector correctness.

## Correctness evidence

- Native/reShade D3D11 readback: all seven public views, 208 comparisons each, no mismatching stages after the age fix. Match, median, smooth, final guide and Feeder XY agree exactly; mask differences of at most one R8 UNORM step are allowed. Display comparison allows R8/half quantization.
- Motion, median and filtered output: 58,565,808 bytes identical with actual trace ON/OFF. The constant-depth branch optimization independently preserved the same 58,565,808 bytes. Trace allocation was checked, not inferred from a preset.
- Native debug/age, knee-zero finite-value, trust, stage and start/stop/reversal/HUD checks pass. The D3D12 debug layer is unavailable on this machine; these are hardware execution/readback checks.
- DXBC and SPIR-V compilation pass. Actual native DX11, native Vulkan and Feeder Vulkan routes all evaluate and compose NR with ApplyModel on and off. The x86 D3D11 client → Host64 route is covered by the parity captures.
- Feeder provider 5 continues to export full-resolution pixel XY unchanged with unit scale/sign. Its optional validation can change a mask, not rewrite valid XY; stale producer stamps reject history. No Feeder binary or mask threshold was changed for this iteration.

Known original test result: the full quality executable still exits 1 on one HDR relative assertion. It compares perceptual grain error to the candidate's own newly improved legacy mode. All other original assertions pass. Same-input comparisons against the previously installed revision improve: scRGB perceptual grain EPE 0.389043 → 0.374154 px, legacy 0.370504 → 0.343469 px. The raw failure is retained in `quality-validated.log`, with all four HDR comparisons in `hdr-comparison.json`.

## Measured stability

These are synthetic scenes with known motion, not measurements from the user's video. In the weak upper band of a 0.7/0.3-pixel pan, jitter is 0.142079 → 0.130352 px and directions within 30° improve from 96.88% to 98.03%. Stationary grain false motion falls from 1.054968 to 0.904223 px, with jitter 1.541599 → 1.340926 px. The depth-separated object border EPE is 0.250962 → 0.229123 px. Clean stationary regions remain zero. Start/stop/reversal remain responsive. Textured interiors are effectively unchanged, with tiny metric differences retained in the raw files. Sparse and aperture-ambiguous scenes still lack sufficient evidence; this does not establish that every chaotic vector in MGR is fixed.

## Performance

| GPU workload | Prior revision | New revision |
|---|---:|---:|
| FX, 1920×1080, debug off, two alternating rounds | 1.4510 ms | 1.4044 ms |
| Native estimator, textured fixture, median of three runs | 0.9288 ms | 0.9564 ms |
| Native estimator, weak-texture fixture, median of three runs | 0.9144 ms | 0.8858 ms |
| FX, 3412×1920, debug off, one pair | 3.7028 ms | 3.5461 ms |
| Arrow preview alone, 1080p, one pair | 0.1270 ms | 0.1034 ms |

RTX 3080, timestamped GPU ranges, no game competing with the fixtures. Clocks vary (recorded in JSON); small differences require caution. Native textured cost increases by about 0.028 ms while the weak-texture case decreases by about 0.029 ms. Do not describe this as a universal native speedup or identical timing. The locally installed modified Lumenite Kernel 2.0 at half-resolution search measured 0.5787 ms at 1080p; this F5Low pipeline remains heavier. These component tests exclude NR and transport. They do not demonstrate recovery of the reported eight-FPS gap or equal image quality to Lumenite.

## Delivery

Current modified OptiScaler x64, shared generated shader and unchanged compatible Feeder/Host dependencies are installed in the two existing test setups. No Requiem changes, package, PR or game backup is made by this revision. MGR real-scene direction stability and mask behavior await user confirmation before a package or PR.

Additional optimization trial: streaming coincident LK derivative reads was tested in isolation and **not adopted**. It changed 1,054 of 58,565,808 snapshot bytes; this is not proof of a quality regression, but it failed the unchanged-output requirement for this optimization. See `lk-reuse-equivalence.json`.
