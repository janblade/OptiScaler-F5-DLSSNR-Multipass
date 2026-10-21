# F5Low incremental audit (English)

## Decision and scope

The canceled camera homography/selective geometric fusion and all later motion-estimator patches were removed. The estimator starts at F5Low 0.1.31, commit `aeb439482c13dd7364ee2bef16fa41b902be6f98`. Local Feeder integration, external NR submission/lifetime fixes, preview transports and relevant diagnostics were retained. Other local repository changes were preserved.

The pinned reference header SHA-256 is `6f501d350ee6c2504115e726224b45342ba189f8c17f41aafa53a91d9bb0cb90`. Reference source is kept separately from the candidate. AeonSR 1.3.0 was inspected at commit `d381f138b572bfd397c62fc57e552c354871a6ed`; its photometric acceptance motivated an isolated experiment. No Aeon camera model or geometric fusion was transplanted.

**Default:** duplicate-cost reuse ON; refinement acceptance, evidence filtering and inverse refinement OFF. No PR/package is created pending user confirmation. This report is not a claim that MGR's sky or every NR artifact is visually solved.

## Contract audit

- Native matching uses six pyramid levels with the finest level at ceil(width/2) × ceil(height/2). No spatial resolution was reduced. Raw matching uses each level's pixels; median scales the finest vectors by 2. The filtered field stores full-resolution pixel displacement in half-resolution RGBA16F.
- Direction is current pixel to its previous-frame location: `previous_uv = current_uv + motion / full_image_size`. Display arrows negate that backward displacement to show visible motion.
- The final consumer guide is full-resolution RGBA16F, produced by the native **DepthProxy → GuideDepth → HiddenBefore/Uncover → GuideMotion** kernels. These kernels are also mechanically translated into the shader, including the disocclusion/surface reconstruction; output is not just bilinear enlargement of optical flow.
- `.z` retains original weighted LK structure confidence; it is not correspondence correctness. Experimental `.w` combines structure, normalized photometric error, candidate-cost separation and agreement with the valid same-position history predictor. This is not full forward/backward consistency. Neither channel is mapped automatically to reconstruction-history rejection.
- Finished-picture optical flow adds no projection jitter. The game's native DLSS vector/jitter route is preserved and was not replaced; gameplay on a native-DLSS title is outside these two installations.
- Matching interprets raw normal/reversed depth through inverse nearness. ReShade applies its selected depth orientation/coordinate settings before feeding those same calculations. ReShade supplies one selected depth buffer; native multi-buffer collection is retained outside the shader.
- A fixed native `precise` arithmetic contract in Smooth removes cross-API fused/reassociated rounding differences. It is a reproducibility adjustment shared by both implementations. Thus “increments off” means base algorithm with that contract, not a claim of literal identity to every pristine 0.1.31 float bit. In the full reference run, only three printed lines changed: sparse within-1-pixel rounded 99.2%→99.3% (mean unchanged), and a reported unflagged cut magnitude changed 138.890→138.891 px; all gates passed.

## Increment 1: reuse identical candidate costs — accepted/default ON

Problem: the same integer candidate can be evaluated again for global/history predictors, repeated coarse cells and the search centre. Reuse the already-known matching cost when displacement equals the current centre; skip coarse duplicates that cannot replace a strict winner. Preserve tie order, penalty and LK iteration count. No new shared-memory cache or adaptive search was assumed beneficial.

Evidence: full quality suite passed. Six scenes × seven compared frames × matching/median/smooth yielded **58,565,808 identical bytes** between flags 0 and 1 (grain, thin structures, independent motion and depth included). This proves equivalence for those fixtures, not every possible future input.

## Increment 2: photometric acceptance of LK — experimental/default OFF

The final fractional LK correction is evaluated with the same mean-removed, weighted 16-sample objective as integer matching. Reject only corrections that worsen that objective beyond 1e-6. If the zero-margin rule changed the integer winner, compare against the actual zero cost, not the old winner's stale cost. This does not forbid subpixel motion in flat regions.

Production results: edge/no-depth within-1px 0.7445→0.7553 and small/no-depth 0.6424→0.6500; grain 0.8164→0.8135, small/depth 0.8746→0.8695 and slow-thin band 0.5511→0.5448. Pan error 0.0441→0.0443 px. Extra GPU work and mixed quality do not justify enabling it by default.

Zero reach remains measured in finest matching pixels (half resolution), clamped to 64; default zero margin/reach stay 0. Existing still-age affects global-candidate acceptance after a stable history and does not universally protect all candidates or LK corrections. No universal sky/low-confidence zeroing was added. A prototype final-zero configuration also changed margin/reach, so it was not an isolated flag comparison and was not promoted.

## Increment 3: vector evidence — experimental/default OFF

Compute evidence in a separate `.w` channel; optionally weight Smooth by that evidence with a 0.05 support floor. Reuse structure/cost/search information; calculate fractional error/contrast where needed. Native PSOs are specialized offline so disabled features do not impose the enabled shader's register/branch allocation. 37 embedded shaders were regenerated and checked. No runtime HLSL compilation was added to the presentation path.

Production results improve some edges/thin bands, but the sparse test regresses **0.9981→0.7829 within 1px**, and smoothing is more expensive. This fails the quality/performance gate. Keep it available for isolated experiments, OFF in both installed defaults. The combined 1+2+4 case retains that sparse regression. Do not treat this confidence experiment as a finished quality improvement.

The shader has an explicit **Compute vector evidence for debug** toggle, OFF by default; selecting a view alone does not silently activate expensive matching diagnostics. Magenta means evidence was not calculated. Native diagnostics calculate it only while the evidence preview is requested. Evidence is preserved when smoothing radius is zero and diagnostics remain usable with estimator increments disabled.

## Existing inverse refinement — measured, not accepted

The existing inverse path reuses current-frame gradients and stops on convergence. Three paired 1080p runs measured medians 0.986816 ms (reuse, ordinary refinement) versus 0.784597 ms (reuse, inverse). A correctly activated prototype quality comparison lost thin structures: within-1px 1.0000→0.0084, with other regressions. Tiny subsequent reproducibility rounding does not justify accepting that large failure. Existing default remains OFF.

## Structural directional correction — deferred by its gate

Confidence/acceptance do not yet pass the full combined quality/performance gate. Per the requested ordering, no directional correction is enabled or presented as completed. Aeon's geometric-model-dependent support was not replaced with an invented normal/tangent guess. No new full camera model was added.

## Measured native flow cost

RTX 3080, 1920×1080 input, 24 warmup + 240 measured frames/run, three counterbalanced runs per configuration. Table is the median of run means, measured with D3D12 per-pass timestamps. It includes luma/pyramid, scene cut, matching, median, Smooth and global candidate, **not** full NR, game frame time, consumer-guide cost or ReShade transport/history-copy overhead. Raw per-pass values and GPU clock/temperature/workload snapshots are in `production-performance-raw.json`.

| Configuration | Feature bits | Median ms | Change vs modified base |
|---|---|---:|---:|
| upstream | None | 1.083123 | -0.40% |
| base | 0 | 1.087467 | +0.00% |
| reuse | 1 | 1.039134 | -4.44% |
| refinement | 2 | 1.143881 | +5.19% |
| evidence | 4 | 1.160576 | +6.72% |
| combined | 7 | 1.133875 | +4.27% |

Default reuse is about 4.1% faster than pristine upstream in this fixture, about 4.4% versus the modified base. One reuse run was slower; three repeats are not a universal speed guarantee. The earlier runs with Zenless Zone Zero active (~60% GPU) are explicitly invalidated under `invalid-contended-timings`; they support no performance claims. Valid runs completed before the later MGR launch, and subsequent inverse timing ran after games closed.

## Feeder ownership and NR integration

Reproduced two concrete problems in the old provider-5 shader: flat low-structure pixels produced history mask ~0.98 even with validation OFF; the static hypothesis could replace (4,-2) px by zero (4.47214 px error). Provider 5 now consumes full-resolution `F5Low::PixelMotionVectors` directly, preserving RGBA16F XY in Feeder's RG16F pixel output without a UV quantization round trip. Redundant Feeder camera fitting/static rewriting is bypassed for provider 5. Other provider behavior is retained.

Optional depth/luma/consistency validation only writes a history-rejection mask. `MV_VALIDATE=0` and `MASK_STRENGTH=0` work. The actual compiled Guides shader passed **768 checkbox/scenario combinations on CPU WARP**: vector error 0, old flat mask 0.98→0, enabled rejection still capable of mask 1. The existing mask IPC/external-NR integration remains in the add-on/host; changes in this increment are in the provider shader, not replacement add-on binaries.

## ReShade transplant and validation

The portable generator reads canonical modified native kernels. Adaptations are FX resource bindings/intrinsics, namespace duplication per pyramid level, 0–255 ages in R32U, frame-stamped history, and one selected depth. Copies needed by FX resource scheduling do not simplify the estimator. Native source hashes accompany generated source.

Odd resolution exposed a real arithmetic discrepancy: constant-folded FX dimensions versus runtime native constant-buffer dimensions changed matching after motion began. A runtime dimension source with a zero→automatic-buffer-size fallback preserves the native dimension arithmetic, in the recommended non-Performance mode; it is not saved into presets. This fixed the tested 641×361 mismatch. ReShade Performance Mode must remain OFF: parameter specialization produced matching differences in a separate negative test, so both provider-5 effects explicitly reject that mode.

**928 stage comparisons in 8 cases passed with zero stored-field differences**, including moving grain/sky, independent objects + HUD, static texture, all experimental bits, selected real depth with a moving boundary, odd resolution and odd resolution with/without real depth, and normal/reversed depth. Each case uses 24 actual consecutive ReShade inputs, replayed through production D3D12 classes. Compare luma, Match, Median, Smooth and complete GuideMotion; Feeder XY is also checked bit for bit. Depth-case flags confirm the real depth input was active. These are D3D11-FX versus D3D12-native captures; Vulkan has compiler/runtime/transport checks rather than this same bytewise replay across all devices.

The final FX entries compile as DXBC and Vulkan SPIR-V at 641×361 and 1920×1080 (148 checks); Feeder entries also compile on both. Native arrows point with known visible motion; all 11 views preserve estimator/consumer fields. Real D3D12→D3D11 and D3D12→Vulkan preview readbacks pass resize/odd-size cases. The optional Windows D3D12 debug layer is unavailable on this system; do not interpret these checks as debug-layer certification.

## Delivery and outstanding confirmation

Modified source resides in the working OptiScaler, ReShadeMotionVectors and DLSS5-Feeder-v1.19.0 trees. Manual installation/debug instructions are in README-EN.md and README-PT.md. PCSX2 uses native F5Low; MGR replaces Turkey-X/NoFxK with shader→Feeder32→Host64→modified OptiScaler. No game backup, package or PR is created. The two installations still need the user's gameplay inspection, especially sky, slow camera motion, object boundaries and NR output. Ambiguous optical-flow regions and the original aliasing/short-duration HUD weaknesses remain limitations.

Primary references: [F5Low 0.1.31 base](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/tree/aeb439482c13dd7364ee2bef16fa41b902be6f98), [AeonSR pinned source](https://github.com/BarbatosAWLS/AeonSR/blob/d381f138b572bfd397c62fc57e552c354871a6ed/src/motion/optical_flow_shaders.inl). The implementation and local GPU captures, rather than documentation estimates, support the measurements above.

## Actual NR and producer integration checks

The final x64 Feeder route was exercised through its private D3D12 device on both D3D11 and Vulkan. A/B captures reached actual model evaluation and composition with successful exits. Eight same-frame before/after pairs per run changed when ApplyModel was on and were byte-identical when it was off (zero changed bytes on both APIs). This checks delivery/application, not subjective reconstruction quality. The current native-producer GPU tests also passed input-lease lifetime, drain and DXGI resize checks. `validation-summary.json` links these results.
