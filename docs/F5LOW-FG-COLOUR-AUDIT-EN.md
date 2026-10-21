# F5Low — frame generation debug and colour encoding audit

Date: 2026-10-20. Installed modified x64 build. Scope: the native OptiScaler diagnostic transport and NR colour-history transitions.

## Confirmed defect and correction

The PCSX2 reproduction generated valid flow and a diagnostic image while the menu remained at “Waiting”. The producer had a ReShade D3D12 device proxy; the FG presenter exposed the original device. The old raw pointer comparison rejected that pair. A controlled run without that proxy had not reproduced the defect.

`native/NativeDebugDevice.h` now compares COM `IUnknown` identities and, for proxy/original pairs, the allocation devices obtained from the producer submission fence and menu completion fence. It continues to reject a genuinely different allocation device. Matching adapter LUIDs alone is deliberately insufficient.

The helper is used by native DX12 and the DX11/Vulkan D3D12 presentation leases. The menu passes its own completion fence into those leases. Producer waits, menu read-completion fences, strong texture references and descriptor/allocator lifetime protection remain active. There is no new texture copy or GPU pass in this correction.

The final production DLL contains no private tracing environment switch, forced menu visibility, forced page or forced diagnostic mode. The user confirmed arrows and normals/depth with XeFG, in the preview and fullscreen; their screenshot is retained as `user-confirmed-normals-depth-xefg.png`.

## Colour encoding

The source-shader WARP regression and a separate RTX hardware test of the **shipped AOT bytecode** both passed. The AOT gamma 2.2/PQ cases returned zero code-value error for the tested 8-bit/10-bit round trips. The deliberately incorrect 10-bit PQ keep buffer produced a 329-code error, confirming test sensitivity. These are model-answer proxy tests; they do not require a neural model to produce identical edits under different input interpretations.

sRGB and gamma 2.2 use different transfer functions, especially in dark tones. Changing the incoming interpretation changes what NR sees and can change its edited result. The steady appearance difference alone does not demonstrate a broken inverse conversion.

A separate defect was present: changing the resolved encoding did not invalidate temporal NR history. `DlssNrColourEncoding::HistoryState` now tracks the effective encoding, shader conversion and linear/HDR domain. A domain change resets model/reuse histories and the scene-cut detector. An unchanged resolved domain, including Auto to forced sRGB, causes no extra reset. This applies to native, Feeder and Host64 NR callers through the shared dispatch.

For the tested PCSX2 Vulkan SDR output, use **Auto**, which resolves to tone-mapped sRGB. The actual swapchain declares `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR` and B8G8R8A8_UNORM. This recommendation follows the declared presentation domain; it is not a measurement of Silent Hill 3's internal PS2 transfer curve. Vulkan reference: https://docs.vulkan.org/refpages/latest/refpages/source/VkColorSpaceKHR.html

## Verification and limits

| Check | Result / scope |
|---|---|
| Device ownership regression | 13 checks passed: direct identity, proxy identities, both sides wrapped, unrelated allocation device, nulls and COM reference lifetime. |
| Encoding / history unit regression | Passed, including encoding changes and unchanged effective domains. |
| Source colour shader / shipped AOT | Both passed; source uses WARP, AOT uses RTX hardware. `colour-roundtrip.log`, `colour-aot-roundtrip.log`. |
| Native DX11 runtime | No FG, XeFG disabled, XeFG enabled and FSR FG enabled executed to normal exit with valid native flow. 600 submitted application frames per case; bridge accounting can differ. |
| Native Vulkan runtime | No FG, XeFG disabled, XeFG enabled and FSR FG enabled executed to normal exit with valid native flow. 400 application frames per case. |
| Actual PCSX2 / XeFG | Clean production DLL, NR model active, ReShade preset techniques empty. Arrows captured fullscreen. User confirmed normals/depth in preview and fullscreen. |
| DX11 shader / Feeder | 55 controlled frames passed: exact Feeder XY, point depth mapping, sky exclusion, tiny geometry, absent-depth fallback, validation transitions, seven finite debug views. |
| DX11 x86 client / Host64 | The same 55-frame resource controls passed through the real client/host path with the updated x64 DLL. |
| Vulkan shader / Feeder | The same 55-frame resource controls passed. |
| DisplayDepth reference | Independent original DisplayDepth RGB comparison passed on all three shader routes: 617,283 channels per route; zero channels above 1/255 error. |
| DLSSG attempt | Streamline rejected the DLSSG feature on this RTX 3080; presentation fell back to a plain swapchain. Normal process exit is **not** a DLSSG success. |

The controlled runtime cases exercise the OptiFG upscaler-input route. Real game-provided DLSS/FSR FG inputs, DLSSG/MFG generation, and interpolated-frame image quality are not certified by these tests. The synthetic FG logs still contain frame-id jump/duplicate warnings; those are not resolved by this menu resource-identity correction. Host64 controls verify delivery and consumption of the shader inputs, not display of interpolated Host frames in a 32-bit game window.

## Full synthetic motion-vector cost

RTX 3080, 3413×1920. Pristine original commit: `aeb439482c13dd7364ee2bef16fa41b902be6f98`. Separate GPU prewarmup, four alternating runs per variant per scene, 64 warmup frames and 160 measured frames in each run. The table aggregates run medians. Games and Host processes were closed; telemetry and binary hashes are retained in the run JSON/CSV files.

| Scope / scene | Original (ms) | Current (ms) | Delta (ms) | Delta (%) |
|---|---:|---:|---:|---:|
| Core / stationary | 3.500032 | 2.258944 | -1.241088 | -35.459% |
| Core / moving | 3.504128 | 3.048960 | -0.455168 | -12.989% |
| Core + depth alignment / stationary | 3.498496 | 2.293760 | -1.204736 | -34.436% |
| Core + depth alignment / moving | 3.502080 | 3.088384 | -0.413696 | -11.813% |

The interval includes luma/pyramid, matching, Lucas–Kanade refinement, median/filtering/camera stages where present, trust mask and guide reconstruction. The second pair also includes current per-frame raw-depth alignment from 2048×2048, scale Y 1.143 and offsets −4/+117; the original receives the same already-aligned input outside its interval. NR inference, diagnostic rendering, CPU readback and API/Feeder/Host transport are excluded.

These are cumulative current-versus-pristine results. This repair changes UI device recognition and NR history invalidation; all 15 compared motion implementation files match the prior candidate. It introduces no new per-pixel motion decision, confidence computation, weight, matching window or LK sampling coordinate. These measurements do not establish game FPS or complete shader/Host route latency.

## Installation

The same clean x64 DLL was installed in `F:/pcsx2-v2.3.25_nightly/winmm.dll` and `F:/Games/Metal Gear Rising - Revengeance/host64/winmm.dll`. Shader/Feeder binaries and presets were preserved. Temporary PCSX2 test overrides were restored to the exact prior OptiScaler.ini bytes. Automatic depth selection and user depth calibration remain available.

DLL SHA-256: `ace6dd2b48ae9efe720d03e734ca440bec74b218ad3aaccd62ab3d8ad416b7cd`.

Evidence: `installation-verification.json`, `device-colour-regression.log`, `fg-matrix-summary.json`, `dx11-fg-matrix-summary.json`, `depth-controls-*`, `user-confirmation.json`, `synthetic-total-summary.json`, `calibrated-total-summary.json` and the associated raw runs/telemetry. No PR or package was created.
