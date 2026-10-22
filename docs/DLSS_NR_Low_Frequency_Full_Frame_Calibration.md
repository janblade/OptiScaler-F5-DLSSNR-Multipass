# DLSS Neural Rendering — Full-Frame Low-Frequency Gain Calibration

## Purpose

This document defines a repeatable method for estimating the **Low Frequency residual gain** when transferring DLSS Neural Rendering (NR) output from **P50** to **P100**. The measurement uses the **entire image** and compares a reconstructed P50 model edit against an independently evaluated native-resolution NR100 reference.

**Separate the two tuning decisions:**

- **Low Frequency gain:** estimate numerically from a full-frame, RAW-domain residual fit.
- **High/Mid Frequency gain:** select through visual evaluation in the actual game. A coefficient that minimizes image-wide pixel error does not necessarily produce the most pleasing edge, texture, or detail appearance.

The numerical High/Mid coefficient is useful during the fit because it accounts for higher-frequency contributions, but it is **not** the final recommended High/Mid setting.

## 1. Capture requirements

Capture the following eight images from the **same game frame**, without changing the scene between P50 and P100 evaluations:

| Input or result | Resolution | Required files |
|---|---|---|
| `proxy_p50` | P50 | `proxy_p50.raw` |
| `proxy_p100` | P100 | `proxy_p100.raw` |
| Standard NR | P50 and P100 | `standard_nr50.raw`, `standard_nr100.raw` |
| Natural NR | P50 and P100 | `natural_nr50.raw`, `natural_nr100.raw` |
| Cinematic NR | P50 and P100 | `cinematic_nr50.raw`, `cinematic_nr100.raw` |

Use the P100 result from a **real NR evaluation at P100**, not an upscaled P50 output. The P50 and P100 proxy images should refer to the same frame and have spatially matched guides, including depth/motion-vector configuration. Record the model versions, pass count, input preparation, working scale, scaling method, and all reconstruction settings in `manifest.txt`.

Example captured dimensions:

- P100: `3840 × 2160`
- P50: `1920 × 1080`
- Format: `DXGI_FORMAT_R16G16B16A16_FLOAT` (DXGI format 10, 4 × FP16)
- Tight row pitches: `30720` and `15360` bytes respectively

**Perform every measurement using the raw pixel values.** Convert FP16 to FP32 for calculations. Do not use 8-bit images, apply tone mapping, or insert an sRGB/linear conversion that was not used by the real NR input/output pipeline. PNGs can be used for visual inspection only.

## 2. Validate the capture before fitting

1. Parse the manifest to obtain each texture's dimensions, pixel format, and row pitch. Do not assume a tightly packed image when the manifest specifies padding.
2. Decode each RAW row according to that row pitch and convert the RGB channels to FP32.
3. Verify that all RGB samples are finite and report the channel minima, maxima, and percentiles; check the alpha channel as an additional integrity check.
4. Confirm that all P50/P100 model outputs correspond to the same frame and the same NR preset.
5. Verify spatial alignment by comparing `proxy_p50` against an area-downsample of `proxy_p100`. For an exact 2× reduction with the captures used here, the expected difference is small (previous captures were approximately `0.00012` RGB RMSE). A large mismatch invalidates the gain fit.
6. Inspect the raw residuals for unexpected clipping, offsets, or capture corruption.

Do **not** crop, mask, or omit any image region from the calibration. The fitting domain is the **complete P100 RGB image**, including all four quadrants.

## 3. Calculate the model residuals

For each style, compute:

```text
E50 = NR50 - Proxy50
T   = E100 = NR100 - Proxy100
```

`T` is the true native-resolution edit produced by the same NR preset. The goal is to predict the **edit**, rather than to fit the complete NR image or blur the native-resolution proxy.

The final reconstruction concept is:

```text
Result100 = Proxy100 + PredictedEdit100
```

`Proxy100` remains the unmodified native-resolution base image throughout this analysis.

## 4. Reconstruct the P50 residual at P100

Use the exact reconstruction algorithm and parameters intended for the final in-game test. For the calibration discussed here:

**Production implementation note:** the equations below describe the calibration model, not a
bit-exact CPU replacement for the optimized shader. For this port, generate `R` with the production
shader in `OptiScaler/shaders/dlssnr/superior/HostResolve.hlsli` (or faithfully reproduce its tap
rounding, clamping, separate `exp2` weights and accumulation order). In particular, production uses
`weighted / weightSum` when `weightSum > 1e-8`, otherwise a bilinear residual fallback; it does not
use the illustrative `max(weightSum, 1e-6)` denominator below. Reproduce the actual Low64 kernel,
texture formats and sample alignment as well. Calibration should follow the shipping reconstruction,
including its low-weight fallback, rather than introduce a different estimator.

| Control in the OptiScaler UI | Value | Meaning |
|---|---:|---|
| Guided radius | **1** | 3×3 neighborhood, 9 taps |
| Guided range sigma | **0.015** | RGB guide similarity in the raw model domain |
| Guided spatial sigma | **3.0** | Spatial weighting, measured in P50 pixel units |
| Guided strength | **1.0** | Fully guided reconstruction |

**Important:** `3×3` means **Guided radius = 1 in the UI**, not 3. Spatial sigma 3.0 does not increase the 3×3 sampling footprint.

At each P100 pixel `p`, use the corresponding continuous P50 texel coordinate:

```text
u50 = (p + 0.5) * (P50Size / P100Size) - 0.5
```

For every P50 tap `q` in the 3×3 neighborhood, calculate a positive joint-bilateral weight:

```text
dRGB2 = dot(Proxy50(q) - Proxy100(p),
            Proxy50(q) - Proxy100(p)) / 3

dXY2 = dot(q - u50, q - u50)

w(q,p) = exp(-0.5 * (
             dRGB2 / (0.015 * 0.015)
           + dXY2  / (3.0 * 3.0)))

R(p) = sum_q(w(q,p) * E50(q)) / max(sum_q(w(q,p)), 1e-6)
```

Clamp neighborhood coordinates at texture borders. These equations operate in the same unmodified RAW domain as the captured model textures. With Guided strength 1.0, `R` is purely the guided result. If the UI strength is changed, reproduce the exact runtime blend with bilinear residual reconstruction before fitting.

## 5. Construct the Low Frequency component

Build a low-pass residual image using **area averaging of `E50`**, followed by bilinear upsampling to P100:

```text
L50_low = AreaDownsample(E50, factor = 8)
L       = BilinearUpsample(L50_low, P100Size)
```

For a 1920×1080 P50 residual, this creates a `240×135` low-resolution image and then samples it on the 3840×2160 grid. This represents the same low-frequency scale as **P50 mip3**. For an exact integer 8× reduction, averaging 8×8 P50 texel blocks reproduces the corresponding area-average low-frequency source.

The low-pass operation must be applied to **`NR50 − Proxy50`**, not to `NR50` alone and never to `Proxy100`. Use consistent pixel-center/sample alignment between the offline calculation and the production shader.

## 6. Estimate the Low gain using the full image

Model the predicted edit as two complementary frequency components:

```text
HighMidBand = R - L
LowBand     = L

PredictedEdit100 = GainHighMid * HighMidBand
                 + GainLow     * LowBand
```

This is equivalent to:

```text
PredictedEdit100 = GainHighMid * R
                 + (GainLow - GainHighMid) * L
```

Fit **both coefficients jointly** using ordinary least squares (LS) on **every P100 pixel and all three RGB channels**:

```text
minimize over (GainHighMid, GainLow):

sum_over_entire_frame_and_RGB(
    PredictedEdit100 - T
)^2
```

Do not choose High/Mid manually *before* this estimation. During the numerical fit it is a second, freely adjustable coefficient that accounts for the high/mid component and allows the Low gain to be measured within the correct two-band model.

An efficient implementation does **not** need to build a giant matrix. Set `A = R − L`, `B = L`, then accumulate five scalar dot products over the complete frame:

```text
aa = sum(A * A)
ab = sum(A * B)
bb = sum(B * B)
at = sum(A * T)
bt = sum(B * T)

[aa ab] [GainHighMid] = [at]
[ab bb] [GainLow    ]   [bt]
```

Solve this 2×2 system in FP64 for numerical stability. The pixel arrays may remain FP32. For a numerically singular or poorly conditioned system, flag the capture rather than silently returning unstable gains. The primary calibration output is **`GainLow`**.

Example reference implementation:

```python
import numpy as np

# R, L, T: aligned H x W x 3 arrays, RGB float32,
#          covering the COMPLETE P100 frame.
A = R - L
B = L

def dot(x, y):
    return np.einsum('hwc,hwc->', x, y, dtype=np.float64)

normal = np.array([
    [dot(A, A), dot(A, B)],
    [dot(A, B), dot(B, B)],
], dtype=np.float64)
rhs = np.array([dot(A, T), dot(B, T)], dtype=np.float64)

gain_highmid_fit, gain_low = np.linalg.solve(normal, rhs)
print(f'Low Frequency gain: {gain_low:.6f}')
```

This code assumes that the 3×3 guided reconstruction `R` and low-frequency component `L` have already been generated exactly as described above.

**Calibration options must match runtime behavior.** For this baseline, disable extra shadow-confidence attenuation and other experimental modification stages while determining the frequency gains. If such operations are indispensable in the production path, explicitly include their effect in the predicted basis and recalibrate; otherwise the measurements describe a different pipeline.

## 7. Use the numerical result correctly

1. Record the full-frame fitted `GainLow` for each NR style and capture.
2. Set **Low Frequency gain** to the measured value as the calibration starting point.
3. Tune **High/Mid Frequency gain visually** in the actual game, independently for each preset. Compare fine textures, outlines, foliage, thin structures, large edges, and the naturalness of the NR edits. Avoid judging only a single static crop.
4. Check the chosen combination during camera movement as well as in static scenes; watch for halos, edge thickening, texture inflation, shimmering, and instability.
5. Verify that broad brightness/color adjustments remain appropriate. If the low-frequency edits feel excessive or weak, reassess the Low calibration using the same full-frame method and several representative frames.

The fitted High/Mid value is an **intermediate regression parameter**, not an automatically selected visual default. Changing High/Mid after fitting means the final pair is no longer the jointly LS-optimal pair; this is intentional because the two controls serve different purposes. The fitted Low remains an objective baseline and should be validated with the visually chosen High/Mid in-game.

## 8. Record results across scenes

Measure each capture independently with the same settings. A suggested recording format:

| Capture ID | NR preset | Guided radius | Range sigma | Spatial sigma | Strength | Fitted Low | Visually selected High/Mid | Notes |
|---|---|---:|---:|---:|---:|---:|---:|---|
| Scene A | Natural | 1 | 0.015 | 3.0 | 1.0 | … | … | Static + motion check |

For a wider calibration, capture representative dark/bright scenes, detailed foliage, thin geometry, characters, and different lighting conditions. Preserve the **complete frame** in every individual LS fit. Compare Low results between scenes and consider a combined calibration only after confirming that the captures use the same model configuration, working scale, transfer method, and pass count.

For reference, the two earlier complete-frame capture analyses, performed with the settings in this document, produced:

| NR preset | 2026-09-30 Low | 2026-10-17 Low |
|---|---:|---:|
| Standard | 0.660884 | 0.860752 |
| Natural | 0.787420 | 0.837632 |
| Cinematic | 0.784353 | 0.861340 |

These are **capture-specific measured values**, not universal defaults. They establish examples of the full-frame calibration output.

## 9. Verification checklist

- [ ] True NR50 and true NR100 results exist for the same frame and style.
- [ ] RAW FP16 decoding, row pitch, channel order, and alignment are verified.
- [ ] Residuals use `NR − Proxy` at their own resolutions.
- [ ] Guided radius is **1 in the UI (3×3)**.
- [ ] Range sigma `0.015`, spatial sigma `3.0`, strength `1.0` are reproduced exactly.
- [ ] Low component is an area-averaged P50 residual at mip3 scale, sampled at P100.
- [ ] Both gain coefficients are fitted jointly, using **all RGB pixels in the full frame**.
- [ ] **Low** is taken from the numerical fit; **High/Mid** is finalized by visual testing.
- [ ] Final image and motion quality are checked in-game.
- [ ] Offline calibration adds no new mandatory GPU work to the shipping runtime: it only determines constants for the existing residual reconstruction and low-band path.

## Interpretation

**Use a full-frame, RAW-domain two-band LS fit to measure Low Frequency gain. Treat the High/Mid coefficient produced by that fit as a mathematical helper, then choose the actual High/Mid setting by visual assessment in the game.** This preserves a repeatable measurement method for broad model edits without forcing fine- and mid-frequency visual character to follow a single pixel-error metric.
