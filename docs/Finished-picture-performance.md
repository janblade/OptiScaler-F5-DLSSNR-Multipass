# Finished Picture performance follow-up (2026-10-27)

Base: `17ae60b3`, on `codex/finished-picture-before-native-fg`, itself based on janblade
`release/0.1.35` at `789f8820`. No UI, configuration, profile or model setting changes.

## Runtime changes

1. Remove the remaining ordinary-presentation `queue->Wait` after the readiness picker
   accepts the capture. Same-queue submission already orders the capture before composition;
   other-queue captures must already be complete. Keep the producer/composition fence signals,
   allocator reuse protection, device-removal rejection and resize lifetime rules.
2. Capture only the valid depth/MV rectangle, using the exact `ResolveGuideRegions` geometry
   that the composition host uses. Preserve full allocation sizes, independent depth/MV origins,
   MV units and MVLowRes/output-resolution MV selection. No resampling or numerical change.
   Missing dimensions retain the existing full-available-extent interpretation. Full rectangles
   still use `CopyResource`. Depth/stencil resources and unknown/multi-plane formats also retain
   whole-resource copying: D3D12 restricts partial depth/stencil copies. No new UAV or shader pass.
3. For PQ pictures that already support UAV access **and** typed UAV loads/stores, fuse the
   final PQ conversion with writing the picture. The optional separate PSO reads the original
   same-pixel RGB/alpha through its UAV, retaining exactly the old alpha and invalid-value fallback.
   It removes the encoded-scratch write/read and final `CopyResource`; the two PQ/linear conversion
   dispatches and FP16 linear NR intermediate remain. Do not fold the FP16 rounding boundary into
   NR or change its colour space. Non-UAV pictures, missing format support or a failed direct PSO
   retain the original encoder plus copy. Both shared Finished Picture/native-input HDR10 callers
   use this helper; the default NR, Superior and Vulkan shaders are unchanged.
4. Read each picture description once for routing/colour selection/composition, outside the NR
   lock, and reuse captured source descriptions for clone validation and copy geometry. Retain
   swapchain-owned colour-space private data, which already caches `SetColorSpace1` changes.
   Do not introduce a global raw-swapchain/resource-pointer cache that could outlive resize/destruction.
   The new PSO and cached format-support result belong to `DlssNr_Dx12` and die with that instance;
   format-support checks are not repeated on ordinary frames or DRS size changes.

The new shader is built/checkable with `tools/compile_nr_finished_pq.ps1 [-Check]`.
Do not regenerate the existing `dlssnr_finished_color` or main NR blobs for this change.
The optional direct encoder uses local Finished Colour mode **6**, distinct from main NR mode 6.

## Correctness

`tests/run_nr_finished_fg_tests.ps1` now also builds `nr_finished_performance.cpp`. It extracts
the production `RecordPqEncode` body from `DlssNr_Late.inl`, with a dispatch bridge that binds
the actual old/new PSOs. The independent baseline uses the **unchanged committed** Finished Colour
blob (mode 1) and old copy sequence. Tests assert bit-for-bit output equality, not only a printed
error metric. Covered on D3D12 WARP with debug-layer error checks and RTX 4090:

- R10G10B10A2 UNORM, RGBA8 UNORM and RGBA16F, odd 67x41 sizes, frame edges and alpha;
- signed HDR input, FP16 storage, NaN/Inf, original FP16 special values and fallback;
- production direct PSO rejection/failure fallback and an actual non-UAV picture;
- a 3840x2160 HDR10 comparison on hardware;
- active copy bytes and untouched padding, independent offsets, repeated DRS shrink/grow,
  scalar R16 UNORM/R32F and RG16F/RG32F guides, typeless clones, actual R16/R32 depth/stencil
  allocations falling back to full copies, overflow guards and multi-plane rejection;
- the existing before-FG, no-FG, resize, FG/NR toggle, queue, colour, profile and multi-view suites.

The Superior CPU/policy/ABI tests and shader identity tests also pass. No approximation, reduced
precision or new filter is introduced. These fixtures do not execute proprietary NR/DLSS inference
or replace visual validation in a game. The earlier Crimson Desert game report still refers to
the preceding before-FG experiment, not these new optimizations.

## GPU measurements

NVIDIA GeForce RTX 4090, driver 616.56, D3D12 direct queue, resident synthetic textures.
Display picture: 3840x2160 HDR10; guide allocation: 2560x1440, RG16F (4 bytes/texel).
Separate warm-up, alternating ABBA/BAAB, 8 blocks, 640 retained timestamp samples per variant,
4 operations per timing window. Uploads, allocation, CPU setup/readback and fences are outside
timestamps. Each operation includes its resource transitions. CPU overhead reduction is not timed.

| Stage | Before median (ms) | After median (ms) | Delta (ms) | Before p10–p90 | After p10–p90 |
|---|---:|---:|---:|---:|---:|
| Final HDR10 encode + picture write | 0.119552 | 0.061184 | -0.058368 | 0.114944–0.126464 | 0.060928–0.066560 |
| MV capture, active 1920x1080 | 0.011520 | 0.007424 | -0.004096 | 0.011520–0.011776 | 0.007424–0.007936 |
| MV capture, active 1280x720 at offset 8,4 | 0.011520 | 0.004608 | -0.006912 | 0.011520–0.011776 | 0.004352–0.004864 |
| MV capture, almost full 2558x1439 | 0.011520 | 0.011520 | 0.000000 | 0.011520–0.011776 | 0.011520–0.011776 |
| MV capture, full 2560x1440 | 0.011520 | 0.011520 | 0.000000 | 0.011520–0.011776 | 0.011520–0.011776 |
| Combined capture + HDR10 conversions, **without models** | 0.230656 | 0.171136 | -0.059520 | 0.224256–0.288512 | 0.167936–0.186112 |

Combined fixture: full depth-sized copy on both variants, full/1920x1080 MV copy respectively,
PQ decode into FP16 and encode back into the UAV-capable HDR10 picture. Both guide payloads
use the same four-byte storage; this times memory operations, not depth interpretation. It omits
all NR/DLSS/model-dependent work and must not be presented as complete game or NR latency.
Paired block median savings were 0.057856–0.063616 ms combined and 0.056320–0.058624 ms for
the final encode alone. Near-full/full copy differences stayed within ±0.000256 ms (timing noise).
Earlier independent passes also found about 0.057–0.058 ms saved by the final encoder.

The HDR10 saving applies only when the actual game/Streamline picture is UAV-capable and the
format is supported. It does not apply to SDR/scRGB or to the preserved non-UAV fallback. Active
guide savings depend on allocation versus active extent and source flags. Do not add the table's
combined and individual savings together. Whole-game timing/pacing and native FG remain to be
compared in the game; no whole-game speedup percentage is claimed.

Reproduce after `tests/run_nr_finished_fg_tests.ps1`:

```powershell
artifacts/validation/finished-fg/nr_finished_performance.exe --hardware measurements.csv
```

The CSV contains every retained GPU timestamp sample, variant, block and ABBA position.
