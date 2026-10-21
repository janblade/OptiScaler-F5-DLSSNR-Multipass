> HISTORICAL: the selective geometric fusion implementation described below was canceled by the user. The active estimator is based on F5Low 0.1.31 with isolated incremental flags. See f5low-incremental-audit-en.md.

# improved F5Low motion vectors

## Scope

Proposed against `janblade/OptiScaler-F5-DLSSNR-Multipass` main at
`aeb439482c13dd7364ee2bef16fa41b902be6f98` (0.1.31). This PR contains OptiScaler
source, its NR forwarder, generated motion bytecode, diagnostics and tests.
The modified ReShade shader/Feeder delivery is maintained separately; their add-on
source and binaries are not part of this PR. Validation in this report is x64.

## Motion estimation and selective geometric fusion

- Preserve the complete six-level F5Low estimator, temporal/global candidates,
  median and smoothing passes. The final field remains backward displacement in
  full-resolution pixels. Geometric fitting follows local optical flow.
- Fit a current-to-previous 2D projective homography from 16 spatially distributed
  correspondences and 32 deterministic four-point hypotheses. Require spatial
  coverage, sufficient consensus, finite projection and photometric support.
- Selective geometric fusion chooses local flow, geometric flow or an agreement
  blend. Preserve a distinct, structured local match when its photometric residual
  is clearly better, including slow foreground motion and a stationary HUD.
- Keep confidence separate from matching support. The old depth/luma-weighted
  structure sum conflated lost sample support at silhouettes with weak texture.
  Confidence now uses the independent gradient tensor; depth/luma weights still
  control the match and LK update. This is not the earlier proposed experimental
  `16 / survivingWeight` normalization, and its separate +0.01 acceptance gate is
  not claimed passed.

## Performance correction

The initial SGF implementation solved all 32 hypotheses serially in a single GPU
thread. One workgroup now samples correspondences and solves independent hypotheses
in parallel; lane zero reduces them in the original order and performs the weighted
refit. Sample count, hypothesis count, thresholds and tie-breaking remain unchanged.
At 2560Ã—1440 the fitting pass dropped from roughly 0.97 ms to 0.05 ms. Per-pass
event markers are now emitted only with `F5LOW_DRED=1`; the normal path avoids those
marker calls. Debug PSO/image creation remains lazy and preview rendering is requested
only while the diagnostic is being displayed.

RTX 3080, GPU timestamps, debug/NR excluded, depth enabled, 24 warmup + 240 measured
frames per run, three runs with counterbalanced build order. Values are medians of
the three run means, in milliseconds. The comparison was rerun with the user's game
closed. Upstream is the original Janblade commit; "previous SGF" is the modified
implementation immediately before this optimization.

| Resolution | Original F5Low | Previous SGF | Improved SGF | Reduction vs previous SGF |
|---|---:|---:|---:|---:|
| 1920Ã—1080 | 1.024 | 2.038 | 1.117 | 45.2% |
| 2560Ã—1440 | 1.717 | 2.795 | 1.871 | 33.1% |
| 3413Ã—1920 | 2.897 | 3.994 | 3.064 | 23.3% |

The remaining geometry/fusion cost is included in the improved column. These are
synthetic motion-pipeline timings, not whole-game FPS or NR/model timings.

## Quality checks

The serial and parallel SGF implementations produced identical **143 lines of
printed quality results**, and both regression runs passed. This compares the SGF
implementation before/after optimization; it does not assert pixel identity to
the original F5Low algorithm or correctness in every game scene. Confidence
independence, producer reset/resize/resource leases, scene-cut behavior and generated
bytecode checks also passed.

A stronger check captured **42 complete GPU snapshots** across six synthetic scene
types at 641Ã—361 (textured pan, sparse, thin lines, layout, grain, and independently
moving foreground/HUD with depth). The vector/confidence field, fusion source and
cost/support buffers, and homography model were byte-identical between serial and
parallel SGF. Snapshot SHA-256: `ca627dc264be2f322dfff81269612eff76a16b2fcccee569afa79a701285cbf1`.
`tests/nr_motion_field_snapshot.cpp` writes these buffers for comparison across builds.

Existing limitations remain: difficult grain/occlusion/textureless regions, a
dominant foreground biasing the sparse homography, weak short-history fast-pan HUD
cases, and histogram cuts between similar distributions. The model is projective
2D geometry, not engine-provided 3D motion or object segmentation.

## Diagnostics

The Optical F5Low tab offers final flow, confidence, camera flow, fusion source,
photometric support, fit quality, pre-fusion flow and separate motion arrows.
Arrows sample each root's local vector and negate backward displacement to show
visible motion; finite vectors are not hidden by a confidence threshold.

Two additional isolated views are provided:

- **Selective geometric fusion rejection only:** white where geometry is not used
  (invalid/local-only), black where geometry is accepted, including blends. An absent
  model produces white throughout. This is a decision mask, not an occlusion mask.
- **Geometric model only (warped grid):** cyan projected grid on black, dark gray
  reference grid, black if the model is invalid. No optical-flow or scene underlay.

Inline, enlarged and fullscreen presentation use the same diagnostic textures.
DX11 and Vulkan now request actual previews and transport the D3D12 producer's
textures to the active renderer. Shared buffers/images, queue/fence ordering and
resource leases prevent foreign texture IDs and reuse during reads. D3D12 frame
generation presentation uses the appropriate D3D12 path. Vulkan status reads are
also synchronized with the producer. GPU readback tests passed RGBA/R8 transfers,
odd sizes, resize, arrow direction, isolated masks/grid and unchanged vector fields.
The D3D12 debug layer was unavailable; this is hardware/readback validation, not
a claim of full debug-layer coverage. The earlier cross-API preview build was
also confirmed working by the user in PCSX2.

## Feeder and NR integration

NR feature creation must execute before its first evaluation. DXGI Present counts
do not prove progress on Feeder's private D3D12 device in a Vulkan game. The new
guard follows a ticket for the exact command-list recording on every D3D12 route:

- Repeated Evaluate calls or another list's submission cannot unlock creation.
- Reset discards the recording; a discarded build is retired and recreated.
- ExecuteCommandLists publishes completion for the correct ticket. A creation-only
  GPU fence prevents cross-queue assumptions; pending creation skips NR without a
  CPU wait. This extra fence is used during model creation, not every frame.
- Primary and additional pass features have independent tickets. Cache/seam progress
  uses real watched submissions when the caller supplies no explicit queue epoch.
- GPU timing samples can be read without guessing a DXGI swapchain queue.
- "Running" is published after successful model evaluation and composition.

Cleanup now retires the matching NR generation before NGX shutdown or release of
the last OptiScaler D3D12 feature. The forwarder also shuts down the model's per-device
runtime after its features are released and permits initialization on a later device.
Use the updated **nvngx.dll_dlssnr.dll together with OptiScaler**.

A Vulkan shutdown test exposed a second lifetime problem: NGX SR could retain
ReShade proxy COM objects after Vulkan unloaded the module implementing their
vtables. NGX initialization now identifies that specific ReShade implementation
and pins it until process termination. This deliberately prevents unloading that
module mid-process while vendor caches may reference it; it adds no per-frame wait.
The latest Vulkan A/B runs completed processing and exited normally.

Matched-frame captures, eight before/after pairs per ApplyModel state:

| x64 route | ApplyModel on: mean absolute RGB delta (0â€“255 scale) | ApplyModel off |
|---|---:|---:|
| Feeder / D3D11 | 6.5137 | 0.0000 |
| Feeder / Vulkan | 4.7190 | 0.0000 |
| Native F5Low + NR / D3D11 | 3.0926 | 0.0000 |

The nonzero on/zero off result proves that NR edits reach composition for these
fixtures; merely creating the model or displaying its menu was not used as proof.
The Vulkan fixture has no DXGI swapchain. Both ApplyModel states exit successfully.
Native F5Low + NR was tested through D3D11 Finished Picture; Feeder uses its own
provider-5 shader route. Only one producer/consumer route is enabled per fixture.

## Build and validation notes

Build `OptiScaler.sln` Release x64 and the NR forwarder project separately with
VS 2022 C++ Build Tools/Windows SDK. Regenerated `F5LowShaderBytecode.h` is included.
`tests/run_improved_f5low_tests.cmd` supplies portable commands for the ticket,
bytecode, flow, producer, visualizer and cross-API fixtures from an x64 Native Tools
prompt. `nr_optical_flow_benchmark.cpp` can be compiled against each implementation
and accepts width/height. `nr_feeder_vulkan_smoke.cpp` is an isolated functional
application for the complete local Feeder/ReShade/NGX runtime, not a performance test.

The corresponding ReShade vector shader compiled all 32 entries to DXBC and SPIR-V
at 641Ã—361 and 1920Ã—1080 (128 successful compilations), including the parallel fit
and the same isolated model/rejection drawing helpers. That shader is outside this PR.
Game-native DLSS still receives the game's original motion resources/scales;
the shared D3D12 guard was updated without replacing them with F5Low. No fresh
gameplay validation of every native game/API/FG combination is claimed. x86 is
outside this validation. Requiem was not reinstalled and its startup crash is not
claimed resolved by these tests.
