# Four Superior Scaling experiments

Baseline: shipped `0fa378f4abf16a18c8f502abc90609335815a8bd`. Only the D3D12 Superior path changes.
Janblade's other enlargement algorithms, exposure settings and Compress screen edges path are preserved.

## Decisions

1. **Keep normal-render specialization.** Separate temporal SDR and HDR mappings 0/2/4/6 for packed and ordinary
   reconstruction make passthrough, mapping, debug-off, compare-off and ApplyModel-on compile-time constants.
   They retain the original colour calculations, exposure, skin protection, strengths and shaping coefficients.
   Diagnostics, comparison/zoom, ApplyModel-off, raw answers, other mappings and failed PSO creation use the generic PSOs.
   All PSOs are created lazily, once per shader/device, rather than per frame.
2. **Keep exact GPU warp lookup for normal packed output.** One immutable FP32 texture stores original metadata in row
   zero, native column queries in row one and native row queries in row two. Its width is `max(11, nativeW, nativeH)`.
   A single small dispatch builds it during generation creation; changing resolution/layout replaces the generation
   through the existing GPU lifetime mechanism. There are no per-frame uploads, allocations or extra dispatches.
   The builder includes the same axis mapping, UV divisions and `precise` native product as the arithmetic consumer.
   Consumers use `Load`, never interpolation. At 3840x2160, the logical texture payload is 184,320 bytes, excluding
   allocator alignment. Failure to allocate/build the lookup retains arithmetic reconstruction. Comparison and zoom
   always use arithmetic mapping and can read original metadata from either texture's row zero.
3. **Reject the tested shared 3x3 tile.** A group loads proxy/residual RGB into two 12x12 FP32 LDS arrays, synchronizes
   once, then runs the original nine taps in the original order with unchanged weights and accumulation. Groups that
   do not fit the nonlinear footprint use direct reads. It passes bit-exact image checks but is slower in every
   screened and fully timed SDR/HDR0 profile, with and without packing. It is only an opt-in test experiment, never a runtime option.
4. **Reject fused colour/depth/motion packing.** The test uses a separate three-UAV root signature, shares `PwUnpack`
   and keeps the original colour filter and vector endpoint transform. It does not provide a stable timing gain and
   compiler reassociation changes depth sample selection and some motion values. Attempts with `precise` native UV,
   precise native position, or colour work moved after guides also fail exact colour comparison. No fused shader or
   three-UAV interface is added to the application; the original packer remains intact.

These are results for these concrete implementations, not proof that every future LDS or fusion design must lose.

## Validation and timing scope

GPU: NVIDIA GeForce RTX 4090, driver 616.56. Synthetic resident 3840x2160 textured/edge images. Main timing profiles:
P50/P65 ordinary, Balanced P50/P65/P100 packed; SDR and HDR mappings 0/2/4/6; NR Small Standard gain coefficients,
Structure=Intensity=1, debug and comparison off. Each A/B uses identical input textures, Low64 and shaping coefficients.
Resolve inputs/output are FP16, original guide and warp data FP32. Fusion's synthetic native colour/depth/motion
sources are FP32, packed colour FP16, packed depth R32F and packed motion RG32F; it tests Light/Balanced/Strong.

Full timings use eight alternating ABBA/BAAB blocks, eight warmup quartets per block, forty measured quartets,
four repetitions per timestamp: 640 samples per variant/profile. Each complete block is submitted together, avoiding
CPU/readback idle gaps between tiny GPU windows. Raw samples are retained without outlier filtering. The preliminary
screen has three blocks, sixteen measured quartets after eight warmup quartets: 96 samples per variant/profile.
An earlier screen with idle gaps had unstable GPU clocks and is excluded from the reported decisions.

Resolve timestamps include the final dispatch and output UAV barrier only; they exclude NR/private DLSS inference,
packing, carrier preparation/decoding, Low64, one-time lookup construction and CPU setup. Fusion timestamps include
the original two packing dispatches versus the single fused dispatch, with identical output transitions.
No complete-game or proprietary temporal-inference speedup is claimed. Per-sample variability and paired block medians
are retained alongside the timings; compare paired measurements rather than subtracting unrelated runs' medians.

The image matrix compares frozen-baseline shaders against experimental and production PSOs over odd native dimensions,
50/65/100%, Light/Balanced/Strong/custom shifted layouts, FP16 and FP32 output, SDR/HDR0/2/4/6, signed residuals,
gamma/PQ input, game/automatic exposure and skin protection. It passes **5,952 comparisons / 968,170,176 32-bit words**
with exact equality. Final timed 4K configurations additionally compare the complete output word for word before timing.
CPU tests verify normal/fallback selection, existing gains, interpass scheduling and guide metadata. Frozen v59 shaders
and generic janblade/Vulkan assets retain their identity checks. These tests establish equality for tested inputs,
not an exhaustive proof for every game frame or driver.

## Reproduction

```powershell
./tools/compile_nr_interpass.ps1
./tools/test_superior_port.ps1
./tools/test_superior_port.ps1 -GpuHost -GpuSpatial
./tools/test_superior_port.ps1 -GpuCandidates
./tools/test_superior_port.ps1 -GpuCandidateTiming
python tools/summarize_superior_candidates.py artifacts/validation/four-final-timing.csv
```

Candidate preparation extracts the baseline from Git; use a checkout with full history. Rejected shaders are generated
only under ignored `artifacts/validation/four-candidates`, never embedded in the DLL. The fusion executable returns 2
for its recorded quality rejection; the opt-in script recognizes that outcome. A different outcome requires review.

## Measured results

The final combination wins all eight paired block medians in each of the 25 measured configurations.
Median paired savings are **0.0279-0.111 ms** for the final resolve alone. Examples (milliseconds):

| Output path, HDR mapping 0 | Scale | Baseline median | Final median | Paired median delta | Baseline / final stddev |
| --- | --- | --- | --- | --- | --- |
| Ordinary | 65% | 0.291840 | 0.238848 | -0.049984 | 0.073423 / 0.060409 |
| Balanced packed | 65% | 0.311296 | 0.240640 | -0.064512 | 0.061365 / 0.045335 |
| Balanced packed | 100% | 0.352000 | 0.275328 | -0.060736 | 0.123591 / 0.104950 |

Specialization alone saves 0.025-0.097 ms in its full matrix. Lookup after specialization saves a further
0.0068-0.0518 ms in the packed matrix; these separate runs are not added to predict total savings.
The repeated ordinary SDR P65 check also wins (paired median -0.029888 ms).
Fusion's paired deltas range from -0.003584 to +0.003200 ms, with mixed block outcomes and failed exact comparisons;
there is no stable saving supporting adoption.
The full LDS matrix loses by 0.0225-0.1072 ms (paired medians), including bit-exact 4K checks.

The raw samples and statistics (final, specialization, lookup after specialization, rejected LDS, rejected fusion,
SDR recheck) are summarised, with how to regenerate them, in [validation/README.md](validation/README.md).
