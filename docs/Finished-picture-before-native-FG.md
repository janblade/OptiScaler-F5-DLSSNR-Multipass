# Finished Picture before native Streamline frame generation

Base: janblade `release/0.1.35` at `789f8820` (the newest upstream branch when work started on 2026-10-27). Working branch: `codex/finished-picture-before-native-fg`. The feature is the ordinary Finished Picture path; there is no A/B selector or additional INI/profile key.

## Problem and resulting behaviour

[PR #69](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/pull/69) protected Crimson Desert from a suspected present/render-queue deadlock and paused Finished Picture while the game's native DLSS-G was active. Its lower presentation hook sits below Streamline and may see generated frames rather than the real application picture. Merely removing the pause can leave NR skipping nearly every presentation because the guides are still being rendered.

This change carries over the application-frame handoff and FG-off recovery from the author's tested Wilsjo2 experiment (`b63fee7b`), without that experiment's mode selector or configuration changes:

1. A compatible native Streamline plugin's swapchain-creation hook registers the **application render queue** as COM private data on its chain. The game's and OptiScaler's local plugin keep separate atomic hook pointers. The source integration's local-runtime exception remains limited to KCD2 without replacement NGX FG.
2. On the real app-facing `Present` / `Present1`, get the current application-buffer index and resource through that plugin's own hooks. Compose NR on the saved render queue **before** invoking the original plugin present/interpolation. The buffer starts and ends in PRESENT.
3. Registered chains are excluded from the lower presentation path, including asynchronous/generated presents. A successful application-buffer handoff never also invokes ordinary-picture composition.
4. If FG is off and a registered plugin declines its application buffer, use the ordinary game picture on the same render queue before its pass-through present. Keep the registration so enabling FG again does not admit lower generated frames. A successful Off call overrides the recent evaluate from the previous FG frame. Failed/uncertain active-FG transitions do not authorize reading an asynchronous display buffer. A still-valid application buffer always takes priority, even with FG off.
5. Without a registered compatible handoff, the existing ordinary Finished Picture presentation path remains available with its readiness checks. This is not a promise to process before FG for a different, unsupported frame-generation API/runtime.

No private DLSS, Superior gain, carrier, depth/MV, exposure or frame-generation algorithm is replaced. All composition reuses the current janblade implementation, including SDR/scRGB/HDR10 conversion, residual-only Finished Picture, its existing native/virtual entry points and current multi-view restrictions. Only the native-game-FG pause is removed from capture/evaluation/composition/native input. No new GPU pass or texture is added to the ordinary composition itself.

## Queue, slot and lifetime invariants

- Never wait at presentation for unfinished work from another queue. Same-queue order is sufficient; another queue's capture must already be complete. Device removal is not ready.
- The app handoff keeps the Wilsjo2 policy: choose the newest ready capture by serial, without using generated-present counts as application-frame age. The ordinary fallback retains janblade's newest-eligible-only policy and its existing one-present epoch window. Other older/equal submitted slots are consumed without releasing in-flight resources.
- Captures, deferred residuals and final composition remain under the shared NR lock. **All swapchain/buffer/colour-space queries happen before taking that lock**, including the ordinary and FG-off fallback paths: Streamline may hold its own lock while command submission enters the NR lock.
- Preserve the existing command-list submission tracking, allocator/fence lifetime and resize rule: only work that touched the back buffer may make ResizeBuffers wait; capture-only guide copies must not introduce the old dependency cycle.
- Skip/test-present, NR-off, Finished-Picture-off, shutdown, device/API/format checks stay in force. Missing exports or a declined creation hook do not register a handoff. Rotation and resize reacquire the current app buffer rather than caching a stale texture.

## Validation

`tests/run_nr_finished_fg_tests.ps1` builds/runs the actual production hook-wrapper body with only application-header seams replaced, plus existing independent regressions. It covers:

- default app handoff with no FG, separate native/local plugins, Hwnd and local legacy queue registration, incompatible/missing exports and declined creation;
- 64 FG Off/On and NR Off/On cycles, menu-like pass-through, explicit Off while the previous FG evaluate is recent, unknown/recent/failed FG transitions;
- declined index and GetBuffer independently, missing registration, skip/test/disabled/shutdown guards, and no duplicated wrapper edits;
- production public routing entry points compiled separately against real DXGI buffers: 64 lower/generated presents excluded, app handoff and FG-off pass-through each compose once, correct PQ metadata, buffer queries outside the NR lock and composition under that lock;
- real DXGI ResizeBuffers/reacquisition with registration retained, SDR/PQ application buffers, and PQ/10-bit colour-space metadata;
- real D3D12/WARP rotating-buffer readback: an edit on the old asynchronous display buffer is overwritten by the later FG copy, while an edit to the selected app buffer survives;
- real WARP queue readiness with a present-gated producer, same-queue ordering, newest/newest-ready slot policies, device removal and resize wait classification;
- existing SDR/scRGB/HDR10 colour/residual conversion, alpha/invalid data, NR profile key completeness, multi-view routing and active/offset colour staging.

The Finished Picture WARP suite is also available in `.github/workflows/finished_picture_checks.yml`. The current upstream changed-lines format check and a full Release x64 build are required before delivery. The initial before-FG port changed no HLSL/blob. The subsequent [performance follow-up](Finished-picture-performance.md) adds an optional direct PQ encoder, active guide copies and GPU-stage measurements, with the original shaders and compatible fallbacks preserved. These checks do not execute proprietary game frame generation or prove motion/depth alignment in every game; whole-game cost/pacing remains an in-game measurement.

### Author's Crimson Desert report (2026-10-27)

The author tested the preceding experimental **before-FG implementation** in Crimson Desert for several hours with the game's native frame generation and Finished Picture NR. They repeatedly enabled/disabled NR while native FG was on, and enabled/disabled FG while Finished Picture NR was on. Crimson Desert automatically turns FG off when entering the game menu; repeated menu entry/exit exercised those transitions. **No crashes were observed.**

This directly targets the game/scenario that motivated #69. It is a user-reported in-game stability result for the preceding experiment, not a claim that this latest-upstream integration has already received the same hours of game testing, nor a timing/quality guarantee for every game. A future PR should cite #69 and distinguish this report from the automated WARP/build results above. No PR has been created for this branch yet.
