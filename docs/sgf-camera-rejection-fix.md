# F5Low: camera-model rejection investigation and correction

## Observed symptom and diagnosis limits

In the supplied Metal Gear Rising recording, the isolated model grid disappears on
some frames. That view returns black when the global fit-valid flag is false.
The rejection mask then becomes white because geometry contributes nowhere; this
does not mean every final optical-flow vector is invalid. Local optical flow remains
available. A previous depth-buffer warning was historical and is not evidence for
the user's current depth selection. No depth calibration is changed by this fix.

Two independently reproduced fitter failure mechanisms are corrected below.
Synthetic reproduction establishes those faults, not that they explain every invalid
model in every supplied gameplay frame. Textureless regions, parallax, disocclusion
and independently moving objects can still legitimately prevent a supported global
2D homography. The implementation does not reconstruct a 3D camera pose from depth.

## 1. Duplicate deterministic hypotheses

The previous linear hash produced only **15 distinct four-point combinations across
32 hypotheses**. A perfectly consistent background could therefore have at least
six eligible sites in all four quadrants, yet no usable minimal hypothesis was tried.

The corrected fixed table retains all 15 old distinct combinations in their original
first-occurrence order. The other 17 slots test distinct combinations selected to
broaden coverage of eight-site support patterns. Sample count, solve count, 0.75-pixel
inlier threshold, six-inlier minimum, four-quadrant coverage and 30% weighted support
gate remain unchanged. This is a sample-coverage fix, not relaxed acceptance.

Exhaustive CPU enumeration, counting only support masks covering all four quadrants:

| Eligible sites | Arrangements | Old masks without a usable hypothesis | Corrected |
|---|---:|---:|---:|
| 6 | 4,480 | 3,645 | 2,594 |
| 8 | 10,896 | 5,875 | 1,478 |
| 10 | 7,744 | 1,327 | 1 |

Every support mask reached by the old sampler remains reached. These are combinatorial
coverage counts, not measured gameplay success rates. Additional hypotheses can alter
model selection; preserving old coverage is not a universal quality guarantee.

## 2. Losing the valid minimal model when refitting fails

The old fitter discarded the entire model when its weighted least-squares refit
failed or lost the required consensus. A deterministic fixture has six supported
inliers before refitting and five afterward, causing an invalid global model.

The corrected fitter keeps the original minimal model only if the refit fails and
the original still passes all existing gates: at least six inliers, all four
quadrants, 0.75-pixel residual limit, 30% weighted support, and the five projective
domain probes. It does not reuse a previous-frame model or force unsupported geometry.
The fallback recheck runs only after a failed refit and qualifying initial support.

## Direct production-shader GPU reproduction

`tests/nr_geometry_fit_gpu.cpp` compiles the production FitGeometry entry and injects
correspondence/confidence fields. Estimation is deliberately bypassed to isolate the
fitter. The same fixture runs against a frozen previous build and the corrected build.

| Case | Previous | Corrected |
|---|---|---|
| Exact translation, all 16 sites | Valid, 16 inliers | Valid, 16 inliers |
| Exact translation, eight eligible sites missed by old sampling | Invalid | Valid, eight inliers, <0.0001 px mean residual |
| Refit loses the sixth inlier | Invalid | Valid minimal model, six inliers, 0.07724 px mean residual |
| Fewer than six supported sites | Invalid | Invalid |
| Correspondences outside previous image | Invalid | Invalid |
| No supported sites | Invalid | Invalid |

## Debug changes

The existing isolated rejection mask and model grid remain separate views. A new
**SGF rejection reason** view in OptiScaler and the corresponding external ReShade
shader distinguishes the decisions hidden by an all-white mask:

| Colour | Meaning |
|---|---|
| Black | Geometry contributes, directly or blended |
| Gray | Camera geometry intentionally disabled |
| Magenta | Global camera model invalid |
| Orange | Model projection invalid or outside the previous image |
| Yellow | Geometric photometric residual rejected |
| Cyan | Fusion policy retained local optical flow |
| Red | No selected hypothesis |

The view uses existing GPU diagnostics, adds no normal-frame pass or CPU readback,
and never writes motion/confidence/history. Native view index: 10; ReShade index: 8.
Existing indices are preserved. DX11/Vulkan preview transport and DX12 all expose it.

## Camera-model switch

Both implementations offer **Use camera model (selective geometric fusion)**,
enabled by default. OptiScaler persists `[DlssNr] NativeGeometricFusion`; the ReShade
preset persists `MV_UseGeometry`. Disabling it bypasses the fitting and geometric
photometric/fusion work, copies the complete pre-fusion local field, and clears the
model with a distinct disabled state. Lightweight dispatches still copy the field
and update diagnostics. No stale model remains visible. Re-enabling fits a fresh
model without restarting the game. The original F5Low global motion candidate is
controlled separately; provider 5 remains selected when geometry is off.

Native GPU tests at both resolutions verify the off-mode motion field exactly equals
the local field, the model view is black, the reason view is gray, and on-mode recovers
a fresh valid model. A real x86 ReShade/Feeder/host64 fixture additionally verifies
the complete RGBA16F fused texture is byte-identical to the local texture in off mode,
with NR composition still active. This does not change game-provided engine vectors.

## Quality and performance

The full motion regression suite passes. Of 143 printed lines, 141 are identical.
The only changes are lower perceptual HDR grain mean errors: scRGB **0.226→0.209 px**,
PQ **0.221→0.197 px**. The remaining printed pan/edge/silhouette/HUD/sparse/scene-cut
tallies are unchanged. The newly recovered fusion results are intentionally not
claimed byte-identical to the previous build's full fused field.

RTX 3080 GPU timestamps, depth enabled, NR/debug excluded, 24 warmup + 240 measured
frames per run, three counterbalanced before/after runs per resolution; games closed:

| Resolution | Previous total ms | Corrected total ms | Previous fit ms | Corrected fit ms |
|---|---:|---:|---:|---:|
| 1920×1080 | 1.141282 | 1.117060 | 0.049289 | 0.052676 |
| 3413×1920 | 3.149103 | 3.190703 | 0.049664 | 0.056171 |

The fit adds only a few microseconds. Total timing differences are small relative to
run variation; no meaningful overall speedup or slowdown, or whole-game FPS change,
is established. The previous parallel-fitting optimization remains in place.

## Integration and build validation

- x64 OptiScaler Release DLL built; all 29 embedded shader entries match current source.
- Isolated debug checks pass at 641×361 and 1920×1080, including invalid-model magenta
  and preservation of the vector field across all 11 native views. D3D12 debug layer
  unavailable on this machine; this is hardware/readback validation.
- External ReShade counterpart: 32 entries × DXBC/SPIR-V × two sizes = 128 successful
  compilation checks. The shared debug helper is copied mechanically.
- Actual x64 Feeder NR processing in D3D11 and Vulkan, plus native F5Low NR in D3D11:
  600 frames per ApplyModel state. Eight matched input/output captures per state;
  ApplyModel on changes pixels, off is exact identity. All six applications exit 0.
- Actual x86 D3D11 test app with modified Feeder 1.19/provider 5 and the x64 host:
  600 frames delivered, NR evaluation/composition reached in the host, application
  exit 0. This is an integration fixture, not fresh Metal Gear Rising gameplay QA.
- No fresh exhaustive native-game DLSS/engine-vector or frame-generation compatibility
  claim. Existing vendor/backend build warnings and post-build copy commands with
  spaces in the checkout path remain outside this fitter fix.

## Scope and reproduction

The OptiScaler PR contains only OptiScaler, tests and documentation. It is based on
the previous `improved F5Low motion vectors` change (PR #196), which was still open
at preparation time. A new PR against Janblade main necessarily includes that
unmerged dependency; the new fix is a separate follow-up commit for review.
Feeder/ReShade source changes and binaries belong only to the separate user package.

From a Visual Studio 2022 x64 tools prompt, use Python 3 for the sampling test:

```bat
python tests\nr_geometry_sampling.py
cl /std:c++20 /EHsc /O2 tests\nr_geometry_fit_gpu.cpp OptiScaler\motion\OpticalFlow_Dx12.cpp OptiScaler\motion\SceneCut_Dx12.cpp d3d12.lib dxgi.lib d3dcompiler.lib /Fe:nr-geometry-fit.exe
nr-geometry-fit.exe
tests\run_improved_f5low_tests.cmd
```

The `legacy` argument is intended only when the fixture is compiled against the
frozen previous source. The normal fixture expects the corrected behavior.
