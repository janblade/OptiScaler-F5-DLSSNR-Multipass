# Superior private guide/carrier fusion

This optimization preserves the depth-aware reconstruction and cubic guided policy introduced on
`codex/superior-depth-aware-refinement`. The A/B baseline is **1077d416**, not janblade's point-sampled
implementation. No PR is created. All 50 pre-existing shader blobs remain byte-identical; one shader
is added. Frequency gains, CNN/Transformer choice, HDR/AutoExposure and full-frame guided are unchanged.

At native Model resolution with compressed render-resolution NR guides, private DLAA previously ran:

1. Uniform joint reconstruction into full-size depth and an intermediate raw-motion texture.
2. Fixed carrier preparation, reading that raw-motion texture and converting its units to temporal MV.

The fused shader performs the same operations in one dispatch and writes **depth, temporal MV and
carrier** through three UAVs. It removes the raw-motion texture, its write/read and an intervening
dispatch. For 3456x1952 private input, that discarded RG32F texture holds about 51.5 MiB of pixel data
(before allocation padding). This is a size calculation, not a VRAM utilization measurement.

The fused kernel retains FP32 guide arithmetic, the original finite-pair handling, exact depth/MV
coordinates, carrier point-coordinate calculation, unit multiplication, signed-zero handling, Hold
and the precise `0.5 + (model - proxy)` order before the FP16 carrier store. No taps, weights or
precision are removed. Private guide construction uses the same CPU constants as the old path.
The three outputs are transitioned to SRV state before NGX evaluation, exactly as before.

A private root signature and one descriptor heap provide the third UAV without changing any ordinary
NR root signature/table. Descriptor slots use the existing 96-slot constant ring and consume its shared
index, avoiding per-frame allocation or reuse of an in-flight constant slot. GPU lifetime recording is
retained. Initialization failure and minification retain the separate joint/sparse-correction/carrier
path. At scales such as 65%, compressed NR guides already match private input dimensions and this
extra reconstruction does not occur; that route stays unchanged.

## Correctness

`tools/test_superior_port.ps1 -GpuPrivateGuides` compares separate and fused GPU outputs bit for bit.
All **192 cases / 64,688,880 32-bit words** pass, including odd/partial groups, independent source origins,
layers, reversed Z, NaN/Inf, signed zeros, subnormals, extreme finite guides, carrier clamps, Hold and
output widths through the D3D12 16384-pixel limit. CPU policy/ABI tests pass. Rebuilding all 51 blobs
and headers matches byte for byte; the original 50 blobs are unchanged from the baseline.
This proves fixture parity, not testing of every game/runtime.

## GPU timestamps

RTX 4090, driver 616.56; 3840x2160 output, Balanced compression, sloped R32F depth and affine RG32F
motion at render resolution. Preflight was 3% utilization, 33.31 W, 480 MHz. The benchmark uses eight
alternating ABBA/BAAB blocks, eight warmup quartets followed by 40 measured quartets per block and four
GPU repetitions per timestamp: **640 samples per variant/configuration**.

The measured route includes colour packing, identical NR guide preparation/correction, private guide
reconstruction and carrier preparation (separate/fused), carrier decode, low field and final cached
guided resolve, with barriers. **NR and DLSS inference are excluded**. Both variants use the same
current reconstruction, linear carrier and cubic range sigma; only the private fusion
changes. Medians from earlier runs against janblade must not be subtracted from these timings to
claim a measured full-game gain or exact remaining overhead.

| Render | Output | Model resolution | Before ms | After ms | Paired delta ms | Delta stddev ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 1080p | SDR | 65% | 0.637184 | 0.637184 | -0.000064 | 0.083482 |
| 1080p | HDR soft knee | 65% | 0.570240 | 0.571776 | -0.000384 | 0.031680 |
| 1080p | SDR | 100% | 1.004288 | 0.888320 | -0.115840 | 0.001895 |
| 1080p | HDR soft knee | 100% | 0.888832 | 0.770560 | -0.117248 | 0.049243 |
| 1440p | SDR | 65% | 0.706688 | 0.707584 | -0.000320 | 0.046471 |
| 1440p | HDR soft knee | 65% | 0.624896 | 0.624640 | +0.000000 | 0.599682 |
| 1440p | SDR | 100% | 1.105280 | 0.993280 | -0.109824 | 0.034351 |
| 1440p | HDR soft knee | 100% | 0.983552 | 0.872448 | -0.111744 | 0.011428 |

Native 100% + Balanced saves approximately **0.116-0.117 ms at 1080p render** and **0.110-0.112 ms at
1440p render** in this measured route. The unchanged 65% controls have paired medians near zero.
One 65% HDR control has a large timing outlier/spread (0.600 ms standard deviation); raw samples are
retained rather than discarded. This is not a measurement of model inference or game frame time.

Two alternatives were tested and rejected before integration: shared-memory tiling preserved bits
but slowed this route by roughly 0.035-0.106 ms; register reuse of matching depth samples preserved
bits but produced no measurable saving (paired medians about +0.001 ms). Neither is in production.
Raw accepted measurements and quality summaries are under `validation/superior-private-guide-fusion-*`.
Use `-GpuPrivateGuidesTiming` to reproduce quality plus timing without needing external baseline files.
