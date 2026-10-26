# Superior Scaling review refinements â€” validation branch

This branch is for manual validation before updating PR #249. Its PR head remains 7362c5f6. Do not push this branch to the PR ref until the user confirms the dynamic-resolution game test.

## Runtime changes

- Superior's compression tracker hashes source formats, not render subrect/allocation dimensions. The fixed packed colour layout still resets history when it changes; ordinary Compress retains its original signature. Current source rectangles and motion units are supplied on every NR evaluation.
- Render-resolution guides are only selected for render sizes no greater than the native colour size on either axis. Their normalized colour warp and endpoint conversion are unchanged.
- Packed depth/MV reserve the full packed colour capacity for the render-guide path. Only the active guide subrect is dispatched and passed to NR. Private DLSS now receives that explicit active extent, never inferring it from allocation size. This avoids per-frame guide reallocations under DRS; it retains more memory than the previous tightly sized guides. A guide-only capacity change preserves the packed colour/unpack resources. Deferred resource retirement is retained.
- CNN A/E selection is latched independently of private feature generations. Auto's dynamic effective scale cannot change it. An explicit change of CNN/Transformer, Auto/manual, or the configured model scale can choose it again; E still means sigma strictly below .01 at selection time, A otherwise. Transformer remains K. The range-sigma curve itself still follows the applied Model resolution.
- Final resolve validates the supplied inverse range variance. Nonpositive/NaN/infinite input falls back to 1/(.015*.015). Valid values use the original arithmetic.
- Joint depth/MV plus minification correction are the only depth-aware production paths. Motion-only surface reconstruction remains for missing depth. Four unreachable bilinear/minification entry points and their blobs were removed (four removed; five reachable quantization specializations added, 52 shaders total); no experimental UI switch was added.
- Depth tolerance includes a source-format quantization bound (two UNORM steps, expanded by the L1 distance of local depth-plane projection). D16/16-bit UNORM and D24 are recognized, including typeless aliases. R32F's floor is zero, preserving its previous tolerance. Precision is carried as float bits in the previously unused warp.reserved word, preserved for private reconstruction; no constant-buffer size/root binding changed. The format-aware arithmetic is compiled only into quantization variants; R32F keeps the original tolerance instructions. PSOs are created lazily and cached. Device-depth tolerance is not claimed to be invariant in eye space under reversed Z.
- Invalid/nonfinite source depth stores the far plane for the declared Z convention. Correction work-list shaders validate dimensions/bands before unsigned subtraction/division and validate mapped pixel bounds.
- Original non-Superior shaders, gains, inter-pass scheduling, exposure and colour packing are unchanged. High/mid excludes Compress's extra scale; low includes it.

## Review cleanup

The unrelated changed-lines formatting gate was removed and janblade's original workflow restored. Existing whole-tree formatting failures are not claimed fixed by this branch; a formatting-policy proposal belongs in a separate PR. The removed helper's force-push/encoding edge cases therefore do not ship here.

Historical surface/minified-motion tests were removed: they exercised combinations that production did not dispatch. The joint tests compare the actual shipped joint/no-depth paths, with analytic depth/MV bounds and explicit historical 7362c5f6 shader references (missing references fail preparation). No-depth checks use analytic motion, not a comparison with itself. The generator asserts its literal-sigma replacement pattern matches once.

## Reproduction

```powershell
./tools/compile_nr_interpass.ps1 -Check
./tools/test_superior_port.ps1 -GpuGuides -GpuPrivateGuides
./tools/test_superior_port.ps1 -GpuReviewTiming
./tools/test_superior_port.ps1 -NrModelPath '<game>/nvngx_dlssnr.dll'
```

The actual-NR test requires an installed NVIDIA NGX driver and a local model DLL. Neither proprietary model nor driver files are redistributed. It builds the existing forwarder separately and does not replace files in the game.

## Validation results

Completed checks before the timing pass:

- CPU policies / ABI / frozen v59 identity pass. Auto CNN identity remains fixed over 1000 alternating render-scale changes from either initial A or E; tracker does not reset for guide-size-only changes.
- Joint production shaders: 2340 cases / 22,037,760 pixels, actual R16_UNORM depth, D24-quantized R32 depth, R32F, FP16/FP32 MV, independent padded source rectangles and reversed Z. FP32-motion analytic plane error is bounded per pixel by .001 working pixels; aggregate FP16/FP32 plane MV RMS is .000638489 (old quantization-unaware baseline .037317641). Depth-plane RMS .000032517269 -> .000003030203. Valid unquantized scenes are bit-identical to the prior production joint shader. Layer mismatches do not increase (2928/13,222,656 in both versions).
- Render-grid guide checks include full-colour-sized allocations with smaller active subrects; 559,104 NR pixels and 6,488,064 private MV components.
- Dynamic GPU correspondence: 32 frames, 745,824 query points, alternating source extents and pixel/normalized MV encodings, fixed guide capacity, center/edges/crossings; maximum error .000061035 colour pixels. This validates geometry, not the internal NR implementation.
- **Actual NR 310.9.16.0, RTX 4090 / 616.56:** both Main A and Small B, all three styles, 48 frames each, fixed 256x144 colour, guide subrects cycling 128x72 / 192x108 / 256x144 within fixed-size allocations, moving high-frequency input and nonzero MV. After eight warmup frames, all 18,063,360 RGB components match the persistent fixed-guide reference bit-for-bit. Every-frame-reset controls differ (RMS .002806876 to .007185160), establishing that temporal history was exercised. The model also changes the input in each case. The test asserts these controls, rather than treating an identity/non-temporal result as evidence.

Final checks: all 52 blobs rebuild byte-for-byte; CPU/ABI tests pass. The final joint/guide tests above pass with the separate quantization PSOs. Fused-vs-separate private guide/carrier parity passes 384 cases / 129,377,760 words across both precisions. Cubic sigma/literal references pass 360 cases / 18,018,720 components (maximum .000488281 on FP16 output); all five malformed inverse-variance cases recover bit-identically to valid sigma .015.

The native-capacity timing variant uses full packed-colour allocation for the candidate and the original tightly sized guide allocation for the baseline. It still dispatches only the active guide subrect. The 65% route has the same allocation in both versions.

Actual model DLL SHA256: `cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed`.

Persistent guide capacity adds approximately 57.9 MiB at 4K/Balanced with 1080p guides compared with the old tight allocation; it eliminates repeated packed-guide reallocations while the active extent changes. Existing typeless source-copy resize work is separate and remains unchanged. Synthetic guide correspondence alone does not prove the behavior of the closed NR model. The separate actual-NR sequence compares persistent histories under fixed and changing guide extents, with an every-frame-reset positive control. A game test remains necessary for real disocclusion, jitter and engine-specific dynamic-resolution metadata.


## GPU timing

RTX 4090, driver 616.56, output 3840x2160, Balanced compression, 1080p/1440p source guides. Synthetic affine motion/sloped depth, R32F textures; `depth16` means D16-quantized values stored in R32F, not a claim about D16 texture bandwidth. Preflight: 0% GPU, about 30 W, 210 MHz.

The measured resident route includes colour packing, actual joint guides/correction, fused private guide/carrier preparation, identity carrier decode, low reconstruction, final SDR/HDR0 resolve and barriers. It excludes NR/DLSS inference and CPU allocations. Each configuration has eight alternating ABBA/BAAB blocks, eight warmup quartets per block then 40 measured quartets, four GPU repetitions per timestamp window: 640 samples per variant. Pair deltas compare the means of A and B within each quartet. No timing samples were discarded. At 100%, measurements explicitly include the new larger allocation stride.

| Source / depth / mapping | Model % | Before median ms | After median ms | Paired delta ms | Paired delta SD ms |
|---|---:|---:|---:|---:|---:|
| SDR-render1080-depth0 | 65 | 0.653056 | 0.653056 | +0.000192 | 0.001842 |
| HDR0-render1080-depth0 | 65 | 0.563712 | 0.565248 | +0.000320 | 0.106178 |
| SDR-render1440-depth0 | 65 | 0.707584 | 0.708864 | +0.001280 | 0.001520 |
| HDR0-render1440-depth0 | 65 | 0.628736 | 0.632576 | +0.003584 | 0.002447 |
| SDR-render1080-depth16 | 65 | 0.663680 | 0.643328 | -0.019328 | 0.001739 |
| HDR0-render1080-depth16 | 65 | 0.584576 | 0.562944 | -0.022272 | 0.002236 |
| SDR-render1440-depth16 | 65 | 0.800000 | 0.708864 | -0.090624 | 0.002053 |
| HDR0-render1440-depth16 | 65 | 0.723456 | 0.633344 | -0.091520 | 0.003071 |
| SDR-render1080-depth0 | 100 | 0.900480 | 0.902272 | +0.001792 | 0.001646 |
| HDR0-render1080-depth0 | 100 | 0.790656 | 0.792192 | +0.002304 | 0.002122 |
| SDR-render1440-depth0 | 100 | 1.000192 | 1.002624 | +0.002048 | 0.001487 |
| HDR0-render1440-depth0 | 100 | 0.896640 | 0.899328 | +0.003392 | 0.001963 |
| SDR-render1080-depth16 | 100 | 0.906496 | 0.901888 | -0.004608 | 0.001875 |
| HDR0-render1080-depth16 | 100 | 0.798208 | 0.795520 | -0.003456 | 0.001754 |
| SDR-render1440-depth16 | 100 | 1.047552 | 0.990208 | -0.057984 | 0.001510 |
| HDR0-render1440-depth16 | 100 | 0.946688 | 0.889600 | -0.057088 | 0.019969 |

Raw samples and variability including P05/P95 are in [CSV](validation/superior-review-timing.csv) and [JSON](validation/superior-review-timing.json). These are stage measurements, not a full-game speedup. R32F remains close to its previous GPU cost; quantized-depth cases avoid much of the unnecessary surface search triggered by mistaking quantization for layer boundaries. History preservation / allocation savings under real dynamic resolution require the user's game validation; resets on genuine NR colour-grid changes (for example Auto model-resolution bucket changes) are deliberately retained.

PR #249 has not been updated. Manual validation should first use fixed Model resolution 100%, Superior + Compress, and vary the game's rendering resolution. Separately test Auto to confirm CNN identity does not change; Auto may still resize the NR colour grid and require legitimate model/history recreation.


## DLL / publication status

Build (No Signing) succeeded for runtime commit `45d55eb75b3f0e40385810e5f6cff614c4578527`: [Actions run](https://github.com/mattjaas/OptiScaler-F5-Superior-Scaling/actions/runs/40367103753). DLL: x64, 30,428,160 bytes, ProductVersion `0.1.34-final (45d55eb)`, SHA256 `4d6d285db6829f6371f8173f27bdd29e135b0d86eb47e18380689a74cb685dd6`.

The subsequent source readability edits rebuild all 52 shaders to the identical committed bytes; no C++ runtime or generated-bytecode change followed this DLL build. CPU and shader rebuild CI passed. Janblade's restored whole-tree formatting check still fails; its policy has not been replaced or weakened in this branch.

The production R32F path adds approximately 0.0002–0.0036 ms in the paired stage measurements. Quantized fixtures improve by approximately 0.0035–0.0915 ms. One R32F/HDR P65 measurement has a large timing outlier (paired SD .106 ms); use the raw samples/P05/P95 rather than treating the sub-microsecond median difference as a proven gain.
