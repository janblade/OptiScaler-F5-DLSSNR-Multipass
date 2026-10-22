# Native centre without guided reconstruction

With Superior Scaling, active Compress screen edges and native Model resolution, the uncompressed interior now carries
the stabilized NR edit directly. It bypasses only the radius-one guided filter. NR, private residual DLSS, low-frequency
shaping, exposure and final composition remain active. This is not the clean frame from before NR, nor a claim that
compressed NR has the same model context as a separate uncompressed native NR evaluation.

The interior packer translates native texels by whole pixels. The new resolve reads the matching stabilized signed
residual texel with `Load`, then applies the existing frequency shaping and composition. Side-by-side comparison zoom
uses bilinear sampling for fractional display queries. The outer compressed regions keep the full guided reconstruction.

## Transition

The transition is **four native pixels wide and lies before compression begins**, measured against the pixel footprint
rather than only its centre. For each compressed axis:

```
d = abs(pixelCentre - bandCentre) - halfBand + 0.5
t = clamp((d + 4) / 4, 0, 1)
s = t*t*(3 - 2*t)
```

An axis/side whose warp is uncompressed contributes zero. The two axes combine as `w = 1-(1-sx)*(1-sy)`.
This gives smooth weights at both ends and avoids a diagonal `max(x,y)` crease at rectangle corners.
If any part of a footprint reaches compression (`d >= 0`), guided weight is already exactly 1.
Farther inside, weight 0 skips the entire nine-tap filter. Only the transition blends the bilinear residual with the
guided residual, before the unchanged frequency shaping. A custom offset/shift or one-axis compression uses its actual
warp metadata, not an assumed centred rectangle.

Smooth weights remove a hard filter cutoff, but **four pixels cannot guarantee an imperceptible transition** for every
scene, NR strength, exposure and moving image. This is a deliberate spatial estimator change; visual game validation
remains necessary. The tests establish mask correctness and agreement with an independent estimator reference.

## Scope and fallback

The native centre uses seven separate, lazily created PSOs: generic HDR/SDR and five cached normal colour profiles.
There is no additional dispatch, output texture, per-frame allocation or mask upload. The existing warp lookup's unused
channels cache per-axis smooth weights, direct texel indices and unit-scale flags during its existing one-time build.
Native normal PSOs reuse those two axis loads instead of recomputing the mask per output pixel. The lookup's UV channel
and metadata retain exact equality with the previous builder. Failed native PSO creation falls back to the existing full-guided resolve.
Below-native, uncompressed, above-native/raw and non-Superior paths retain their previous PSOs. All 22 existing host
consumer shader binaries (excluding the updated lookup builder) are byte-identical to `1ff8c811`.
The separate high/medium Model-resolution correction and low packed-size
correction also remain unchanged.

## Verification

The GPU mask probe compares production mask weights and integer texel shifts against the CPU warp specification over
48 combinations of odd rasters, 50/65/100/150%, Light/Balanced/Strong, shifted custom and one-axis layouts. It checks
2,602,608 pixels: 412,877 direct, 29,524 transition and 2,160,207 full-guided. Every one of the 208,251 compressed native
footprints has full guided weight. Non-native cases all retain full guided reconstruction.

The complete host reference uses an independently transcribed scalar mask/mapping and the frozen v59 guided estimator,
with the new blend before shaping. Generic native and production cached native PSOs are compared across HDR mappings,
gamma/PQ input, exposure, skin protection, comparison/zoom, diagnostic views and FP16/FP32 storage.
Unchanged paths require exact equality; native edited outputs allow a small FP rounding tolerance because splitting
the reference's guided and shaping stages changes compiler contraction/cancellation. The test reports measured maxima;
it does not describe this intentional new native estimator as equal to the old full-guided image.

```powershell
./tools/compile_nr_interpass.ps1
./tools/test_superior_port.ps1 -GpuHost -GpuNativeCenter
./tools/test_superior_port.ps1 -GpuNativeCenterTiming
```

Timing compares the previous native full-guided cached PSOs from `1ff8c811` against the new native PSOs, using identical
resident inputs, warp lookup, coefficients and output barriers. It includes Light/Balanced/Strong at 100% and 65% controls,
SDR and HDR mappings 0/2/4/6, 3840x2160, Small Standard gains, Structure=Intensity=1. Proxy/residual/Low64/output are FP16;
the original guide and metadata are FP32. It measures final resolve only, excluding NR/private DLSS inference, packing,
carrier, Low64 preparation, CPU setup and one-time PSO creation. No complete-game saving is inferred from this fixture.

## Results on RTX 4090 / driver 616.56

All 15 native configurations improve their paired median GPU time. Savings are **0.0207-0.0417 ms** for the final
resolve alone. The 15 P65 controls retain exact output equality and their paired deltas are within 0.000128 ms of zero.
Examples at native 100%, HDR mapping 0 (milliseconds):

| Compress layout | Before median | After median | Paired median delta | Before / after stddev |
| --- | --- | --- | --- | --- |
| Light | 0.269568 | 0.239360 | -0.031488 | 0.031645 / 0.029814 |
| Balanced | 0.265088 | 0.236544 | -0.029568 | 0.037416 / 0.033128 |
| Strong | 0.254208 | 0.232832 | -0.022080 | 0.035307 / 0.028927 |

The unoptimized prototype recomputed mask geometry per pixel and regressed in several Strong profiles. That prototype
is not shipped; the final native normal PSOs read precomputed mask values from the existing lookup's spare channels.
Generic native PSOs retain arithmetic mapping for comparison/zoom and unusual colour configurations.

Validation passed 4,824 packed host comparisons / 1,046,248,416 components against the independent new reference.
Maximum reported native reference differences were 0.00000152 in FP32 and 0.0000611 in FP16 (at most two half ULPs,
including near-black cancellation). Native output intentionally differs from the previous fully guided centre.
The mask checks pass, and 58,176 cached axis coordinates, including timed 4K layouts, match the old lookup bit for bit.
The unchanged ordinary host comparison also passed 2,304 cases / 5,974,272 words exactly.

[Raw samples](validation/superior-native-center-timing.csv) and
[metadata, variability and paired block medians](validation/superior-native-center-timing.json).
