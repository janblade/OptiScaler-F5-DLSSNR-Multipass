# Feeder finished-picture NR and Vulkan frame-generation integration

This is an opt-in integration for an already rendered ReShade/Feeder picture.
Unmarked native-game DLAA/SR calls keep their existing path. No F5Low motion
matching, filtering, confidence or camera-model algorithm is changed here.

## Problems and resulting behavior

- An ordinary zero-jitter DLAA carrier reconstructs an already finished picture a
  second time. In the tested Kernel/Feeder setup this produced repeated detail
  patterns even with NR disabled. Explicitly marked native-size Feeder frames now
  copy color to Output and enter the existing NR seams without that second DLAA.
- Bypassing the ordinary upscaler evaluation also bypassed its FG-input capture.
  The finished-picture path now forwards depth, motion and the NR-processed Output
  through `UpscalerInputsDx12::UpscaleStart/End`, restoring the producer's states.
- The Feeder's private D3D12 queue did not share the visible present clock. NR
  creation could remain pending indefinitely with FG off or a menu closed. The
  producer's queue and actual submission epoch now drive readiness/retirement.
- Global late/deferred NR modes could divert the marked call into an unrelated
  D3D12 present. The exception is restricted to the explicit Feeder contract.
- Vulkan's bridge copied the game image before ReShade executed its effects.
  Internal NR captures and FG counters therefore succeeded while the displayed
  frame still lacked NR. A final-picture handoff now replaces that early fallback
  with the complete post-ReShade RTV before the bridge presents.
- The Vulkan route menu mistook Feeder's D3D12 feature for an unconverted Vulkan
  game feature. It could request an unnecessary backend rebuild and show a false
  'turn on the game upscaler' warning. Live Feeder inputs are recognized explicitly.
- On Vulkan teardown, retained wrapped D3D12 objects could outlive their ReShade
  code module. The Feeder-only path pins that wrapper code until process exit;
  resource references and ordinary-game behavior are unchanged.

## NGX producer parameters

A compatible producer sets these on its native-size, non-SR evaluations:

| Parameter | Type | Meaning |
| --- | --- | --- |
| `OptiScaler.Feeder.FinishedPicture` | unsigned int | Version 1 opt-in |
| `OptiScaler.Feeder.InputState` | unsigned int | Declared shader-resource state |
| `OptiScaler.Feeder.Queue` | `void*` | Producer's D3D12 command queue |
| `OptiScaler.Feeder.SubmissionEpoch` | unsigned long long | Epoch of the command list being recorded |
| `OptiScaler.Feeder.FinishedPicture.Accepted` | unsigned int, output | Set to 1 after acceptance |

The epoch stays the same for both NR seams and repeated evaluations recorded on
one list. It advances only as the producer submits lists; it must not pretend that
a command list has reached the GPU before ExecuteCommandLists/Signal. A newly
created model is not used until a later submitted epoch makes it safe.

The consumer validates native-size color/output shape, format, sample count,
resource flags and allowed input states. Failed validation is an explicit failure,
not a silent fallback to the unwanted second DLAA reconstruction. Explicit Feeder
SR and all game calls without the marker retain ordinary upscaling.

## Final Vulkan picture ABI

`OptiScalerFeederPresentV1` is exported from the consumer. Its data layout is in
`OptiScaler/native/FeederPresentContract.h`. The native x64 companion Feeder invokes
it from `reshade_finish_effects`, after all techniques, including optional debug
techniques placed after the Feed technique.

1. Query with `buffer == nullptr` and the actual source VkSwapchain identity.
2. Capture the full-size final RTV into a D3D12-shared linear buffer imported into
   Vulkan. Match the actual final format and 256-byte row pitch, independently of
   the NR model/work resolution. Restore the RTV's original state.
3. Release external buffer ownership, flush the image-to-buffer copy and signal
   the imported ready timeline. Never promise an unsubmitted fence value.
4. Submit the buffer/fence handles, dimensions, row pitch, format, producer
   generation and the queried bridge generation.
5. The consumer validates the matching live bridge, imports handles on its OWN
   D3D12 device, and copies the buffer directly into the current output backbuffer.
   Private-device ID3D12Resource pointers are never exchanged across devices.
6. The consumer returns a shared completion fence/value after queuing its read.
   The producer GPU-waits that value before overwriting the shared buffer.

Imports are cached by generation. Resize/rebuild retires old imports before
release. The ordinary early bridge copy stays as fallback when there is no valid
companion final picture. No presented/released Vulkan WSI image is read after
vkQueuePresentKHR. The direct buffer copy avoids an extra intermediate texture
and an additional command-list/allocator ring.

## Companion changes outside this PR

This PR contains OptiScaler consumer changes only. The supplied companion Feeder
adds the NGX markers/clock, final-picture transport and runtime selection that
keeps Kernel/Feed effects on the Vulkan scene and prevents duplicate processing
on the D3D12 presentation runtime. Stock Feeder does not provide these extensions.

The ReShade Statistics-tab fix is separate from this consumer PR. The visible
D3D12 runtime has no effects to measure, so its stock tab was empty. The modified
ReShade 6.8 viewer displays immutable numeric timing snapshots from the Vulkan
scene, using its existing CPU/GPU technique and GPU pass measurements. The Feeder
binds that view with the optional `ReShadeSetStatisticsSourceV1` export. It does
not transfer texture views between incompatible graphics APIs or render effects
twice. Collection requests expire when Statistics closes. Shader times exclude
external NR, transport and FG work; they are not the entire frame time.

## Validation and limits

- Release x64 consumer and native Feeder compiled successfully.
- PCSX2 2.3.25 / Silent Hill 3 / Vulkan / RTX 3080, driver 616.56, 3413x1920:
  normal launch through the global ReShade installation, Kernel provider 3;
  displayed NR difference output and ordinary NR on/off comparisons with XeFG;
  live FG off/on, Native/Bridge menu radio controls, and FSR FG context/NR delivery.
- XeFG delivery counters increased by 600 displayed frames over 300 presentations
  in stable intervals, while final post-ReShade pictures continued to be forwarded.
- The user confirmed NR+FG and later the Statistics view functioning in the game.
- Native DX11, native Vulkan and DX11 x86-to-Host64 fixtures accepted the marked
  contract, retired model creation using submitted epochs, captured changed NR
  before/after images, and exited without compilation/device-removal errors.
- Extended bridge GPU regression: five formats (BGRA UNORM/sRGB, RGBA, RGB10A2,
  RGBA16F), byte-exact output for the original texture and new linear-buffer paths,
  invalid row-pitch rejection, ten resize cycles, hidden-surface recreation and
  no D3D12 debug-layer warnings/errors. Existing memory-growth bounds passed.
- Statistics registry regression covers warm-up, actual sample retention, request
  expiry, reset, concurrent publish/read, destruction and exact address reuse.

XeFG requires the bridge. The native Vulkan swapchain is not newly given XeFG
support. Leaving an already created bridge still requires a restart as documented
by the existing menu. DLSS-G was not acceptance-tested in this round. Timestamp
intervals may include synchronization/other GPU activity and no universal FPS
improvement is claimed. No motion-vector quality/performance claim is introduced
by this integration PR.
