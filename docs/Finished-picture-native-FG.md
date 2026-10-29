# Finished Picture before native frame generation

Finished Picture can process the application's real picture before compatible native
Streamline DLSS-G interpolates it. This addresses the missing handoff described in
[PR #69](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/pull/69), while retaining
its protection against circular render/present queue waits. No new setting is required.

## Routing and compatibility

- A supported Streamline 2 plugin registers the application's render queue through its
  handled swapchain creation hook. Present reads the app buffer through that same plugin's
  index/buffer hooks, submits NR on the saved render queue, then invokes the original Present.
- Native and local hook pointers are separate. The bundled local-runtime exception is limited
  to Kingdom Come: Deliverance II without replacement NGX FG. Unknown, SL1 and future native
  major ABIs, missing exports and declined creation hooks are not assumed compatible.
  Replacement NGX FG does not opt into these native hooks.
- Registered chains exclude lower/generated Finished Picture composition. The native
  motion/NR producer, which reads lower display buffers, pauses while native FG may be active,
  including on registered chains, with an explicit status and a one-time flow-history reset.
  With FG Off its ordinary path resumes. Native FG without
  a usable handoff keeps the conservative pause; generated frames must never enter NR history.
  Evaluate and guide acquisition check a lock-free live-registration count before recording
  copies, pre-SR NR or private DLSS. Swapchain-owned COM tokens decrement the count on
  destruction/re-registration; evaluate never queries DXGI while holding the NR mutex.
- A successful explicit FG Off permits ordinary-picture composition on the registered render
  queue if the plugin declines its app buffer. Failed/uncertain Off and Auto/Dynamic without
  an app buffer skip NR with a waiting status. A valid app buffer takes priority even with FG Off.
- Without native FG, unsupported/unregistered chains keep the ordinary presentation path.
  This is not support for every frame-generation API. D3D11, Vulkan, NR models, Superior,
  exposure settings and existing multi-view restrictions are unchanged.

## Guide freshness and queue safety

Always choose the **newest eligible capture**, then check readiness. Never substitute older
completed depth/MV for a newer unfinished capture. Same-queue submission order is sufficient;
another queue must already have completed its capture. Presentation never waits for a future
cross-queue signal. Device removal is not readiness.

At the start of each real app Present, snapshot the newest submitted, non-expired capture
serial under the NR mutex. Selection and interval expiry both use that same watermark.
A worker's newer capture recorded/submitted while the picture is queried or composed stays
pending for the next boundary. An already armed but unsubmitted next-frame capture does not
advance the watermark or block current submitted guides. This also applies to FG-off app
pass-through, menus, disabled NR and declined buffers. Expiry discards eligibility, not
in-flight resources; submission/reset and fences continue protecting reuse. Skipped app
pictures break NR history. Ordinary lower presentation still ignores unsubmitted recordings
and keeps its short epoch window. Generated-present counts are not used as app-frame age.

Swapchain/buffer/colour-space queries happen outside the NR mutex. Captures, selection and
composition use that mutex; GPU fences still protect slot reuse. Resize waits only for work
that actually touched the back buffer, not capture-only work.

## Optional writeback optimizations

Active depth/MV rectangles are copied without resampling, preserving allocation, independent
origins, formats and units. Depth/stencil, unknown/multiplane formats and full rectangles retain
full copies. Outside a partial copy the clone contains undefined/stale texels: spatial packing
and NGX use the active rectangle; debug dumps may show old padding. New readers must respect
the active region or request a full copy.

HDR10 conversion keeps the existing shared PQ/matrix math and FP16 NR intermediate:

- Typed-load/store-capable UAV pictures can receive the final compute conversion directly.
  An explicit UAV barrier orders the write when the arrival/return state is already UAV;
  other states retain their transition barriers.
- Eligible non-UAV R10G10B10A2 render targets use an RGB-only full-screen triangle on Finished
  Picture's **private DIRECT list**, preserving alpha. Shared native/virtual application lists
  never opt into graphics-state changes. Unsupported cases retain compute plus copy.

The shared compositor descriptor/constant ring is preserved. PSOs/capability checks are cached
per compositor. First use may create a PSO under the NR lock and cause a one-time hitch.
There is no added full-resolution allocation or steady-state diagnostics pass. The previously
declined tiny compute specialization is not included.

`EncodePQ` saturates NaN/Inf before encoding, just like the reference; a NaN input does **not**
necessarily preserve the original RGB. Tests check reference equivalence, not an unreachable
invalid-result fallback.

## Verification and limits

Run `tests/run_nr_finished_fg_tests.ps1` for production hook lookup/wrappers, native-input
guards, evaluate/acquisition early pause, live registration lifetime, real slot selection
and bounded capture expiry with parallel next-frame recording, WARP queue/resize/toggle tests, colour,
profiles and views. Production PQ tests execute the actual compositor methods, shader helper
methods, root-signature/constant/descriptor setup, format capability checks and compute/raster
PSOs, including ring wrap and same-state UAV ordering. NR/optical-flow inference and the
proprietary Streamline runtime are outside the isolated harness.

The independent colour reference is the unchanged committed Finished Colour mode-1 shader
plus copy. Byte-identical outputs were observed on WARP and RTX 4090, including HDR10 alpha,
odd/1x1 images and FP16 special values. This is a measured scope, not a cross-vendor guarantee:
RT and UAV float-to-UNORM conversion/code generation can differ by one LSB on other hardware.
Rebuild/check the three optional blobs with `tools/compile_nr_finished_pq.ps1 [-Check]`;
header checks accept LF/CRLF while CSO comparison remains byte-exact.

Earlier RTX 4090 / driver 616.56 synthetic 4K timestamp tests used resident textures, warm-up,
8 ABBA/BAAB blocks and 640 retained samples per variant. Varied finite HDR10 final non-UAV
write measured **0.168 -> 0.109 ms**; the combined guide/conversion fixture measured
**0.326 -> 0.228 ms**, excluding NR/DLSS inference. These content-dependent stage measurements
are not whole-game gains and individual/combined savings must not be added together.

After the review corrections, the same GPU also passed the actual compositor wrapper tests.
A 4K finite HDR10 UAV-arrival fixture (8 ABBA/BAAB blocks, 640 samples per variant) measured
**0.115968 ms** median for the valid reference compute-plus-copy path (p10/p90
0.111360/0.121600) and **0.060928 ms** for direct write including the required UAV barrier
(p10/p90 0.060416/0.066048). This compares with the unoptimized reference, **not** with the
previous direct path that omitted the barrier. It does not measure an additional speedup from
this correctness fix. No new GPU pass is added to ordinary SDR/scRGB or PRESENT-state writeback.

The author tested the before-FG implementation in **Crimson Desert**, including repeated NR
On/Off, native FG On/Off and game-menu transitions (the menu disables FG automatically), with
no crashes in those dedicated transition tests. Separate sessions had GPU timeouts, including
with NR off; the available kernel dump was inconclusive. This does not establish the cause or
rule out the wider integration. Those detailed transition observations precede the review
corrections. On 2026-10-29 the author additionally tested the corrected DLL (`d0e138ac`)
and reported that it works. The author has not tested it in any Unreal Engine title; the
parallel-recording regression is covered by the isolated tests, not by a UE game run.
