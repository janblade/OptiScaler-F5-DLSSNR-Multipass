# Finished Picture HDR10 render-target writeback

Follow-up to `d3b42cae` on `codex/finished-picture-before-native-fg`, based on janblade
`release/0.1.35` at `789f8820`. This removes the final encoded-scratch write/copy for eligible
**non-UAV HDR10** pictures. It does not enable the previously declined compute specialization.

## Scope and fallback

Only the `RecordPqEncode` call on Finished Picture's own DIRECT command list opts in.
Native/virtual-input callers use the default `privateGraphicsList=false`: their command lists
belong to the application, so changing their graphics state would be unsafe without restoring it.
Residual-only Finished Picture, SDR/scRGB, private DLSS, NR, guides/gains and game FG algorithms
are unchanged. The previously enabled direct-UAV encoder remains unchanged.

Eligibility is checked before recording commands:

- source: shader-readable RGBA16F, with UAV support and the existing UAV arrival state;
- destination: **R10G10B10A2 UNORM**, with render-target support and **without UAV support**;
- identical source/target dimensions, nonempty 2D textures, one mip, one array slice and one sample;
- a DIRECT command list and valid descriptor slot.

An eligible picture uses one full-screen triangle: linear scRGB -> the existing shared PQ/matrix
math -> the actual HDR10 render target. The PSO writes **RGB only**, retaining the original alpha
bits. A non-finite encoded result discards that pixel, retaining all its original channels, exactly
as the old compute shader's original-colour fallback does. It never binds the picture as an SRV
while writing it. No blending, depth/stencil, filtering, temporal processing or new approximation.

Other formats/flags/list types and failed setup retain the unchanged common compute shader plus
copy. Root signature, graphics PSO and CPU RTV heap are created lazily once per compositor;
failed setup is not retried every frame. No new full-resolution texture is allocated. The old
scratch remains available for fallback, so this does not claim a VRAM-allocation reduction.

## State and lifetime

`DlssNr_FinishedRaster.h` holds the production eligibility, pipeline creation and recording code,
also used directly by the GPU tests. It sets its own PSO, graphics root, heap, full viewport/scissor,
render target and triangle topology, disables predication and unbinds the render target after drawing.
Transitions restore the picture's arrival state and the linear source's UAV state. It does not
attempt a partial graphics-state backup on an application-owned list; that path never opts in.

The host consumes one existing compositor descriptor slot, just as the previous encoder did,
and advances the shared 96-slot ring only after success. The persistent RTV heap has matching
slot count. Existing queue order, capture/composition fence signals, late-slot reuse, back-buffer
resize waits and device/session shutdown ownership are retained. The raster helper is owned by
the compositor and destroyed with it; it stores no raw swapchain or texture cache.

Build/check the new VS/PS blobs and existing direct-UAV blob with:
`tools/compile_nr_finished_pq.ps1 [-Check]`. The common NR/Finished Colour and Superior blobs
are not regenerated or changed.

## Validation

`tests/run_nr_finished_fg_tests.ps1` compiles the production `RecordPqEncode` body and calls the
actual raster implementation through the fixture's composition bridge. The independent reference
is the **unchanged committed** common compute shader (mode 1) plus `CopyResource`.

Asserted on D3D12 WARP with debug-layer error checks and RTX 4090:

- bit-for-bit equality of every output pixel, including 4K, odd sizes and 1x1;
- all 65,536 FP16 code values on neutral RGB samples, including signed zero, NaN and Inf;
- varied coloured/signed HDR, dark values and finite/stress fixtures;
- preservation of all four original two-bit HDR10 alpha values;
- PSO reuse across 67x41 -> 129x73 -> 1x1 -> 67x41 images, with descriptor-slot rotation;
- deliberately wrong viewport/scissor/topology before the private raster call;
- actual raster success (not a silent comparison of the fallback with itself);
- forced fallback, rejected formats/flags/dimensions/mips/arrays/MSAA and non-DIRECT eligibility;
- an eligible render target with the native/virtual default opt-in disabled: zero raster requests.

The existing native FG routing/toggle/resize/queue, colour, profile and multi-view tests and the
Superior policy/ABI/shader-identity tests also pass. Full Release x64 compilation is required
before DLL delivery. These results do not execute proprietary NR/DLSS or replace in-game testing.

## GPU timings

RTX 4090, driver 616.56. Resident synthetic textures, 8 alternating ABBA/BAAB blocks, separate
warm-up, 640 retained GPU timestamp samples per variant and 4 operations per window. Allocation,
PSO creation, uploads, CPU recording/readback and fence waits are outside timestamps. The
timed operations include transitions. Baseline: current non-UAV compute encode/copy; candidate:
the production private-list raster writer. Both use the same non-UAV render-target picture.

Finite varied-colour HDR fixture:

| Stage | Before median (ms) | After median (ms) | Delta (ms) | Before p10–p90 | After p10–p90 |
|---|---:|---:|---:|---:|---:|
| Encode/write, 1920x1080 | 0.026368 | 0.009472 | -0.016896 | 0.025856–0.026880 | 0.008960–0.009728 |
| Encode/write, 2560x1440 | 0.039936 | 0.013568 | -0.026368 | 0.039424–0.040192 | 0.013056–0.013824 |
| Encode/write, 3840x2160 | 0.166400 | 0.109568 | -0.056832 | 0.160768–0.173824 | 0.104192–0.110848 |
| Capture + PQ conversions, 4K, **without models** | 0.327168 | 0.229120 | -0.098048 | 0.318976–0.389632 | 0.224256–0.235776 |

Both variants of the combined fixture already use active-MV capture, with the same guide
allocations, a full depth-sized copy, 1920x1080 active MV in a 2560x1440 allocation, and the same
PQ decoder. It omits all NR/DLSS/model-dependent work. Differences between isolated and combined
stages are cache/content dependent; do not add their savings together. Paired-block median
savings at 4K were 0.056064–0.058880 ms for encode and 0.097024–0.098688 ms combined.

The NaN/Inf stress fixture also matched bit-for-bit and saved 0.054272 ms for the 4K encoder
(0.163328 -> 0.109056 ms) and 0.096256 ms combined (0.325120 -> 0.228864 ms). An earlier,
more correlated HDR fixture saved about 0.083 ms for encode. Content affects these timings;
no whole-game speedup or fixed gain for every image/GPU is claimed. No additional runtime
diagnostic pass, readback, timer or UI option is introduced.

Reproduce after the Finished Picture suite:

```powershell
artifacts/validation/finished-fg/nr_finished_performance.exe --hardware raster.csv --raster
```

This runs finite and stress comparisons at 1080p, 1440p and 4K and writes every retained sample.
