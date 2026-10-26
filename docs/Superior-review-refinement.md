# Superior Scaling review refinements — validation branch

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

Final shader-specialization checks and GPU timings are recorded below when complete. Synthetic guide correspondence alone does not prove the behavior of the closed NR model. The separate actual-NR sequence compares persistent histories under fixed and changing guide extents, with an every-frame-reset positive control. A game test remains necessary for real disocclusion, jitter and engine-specific dynamic-resolution metadata.
