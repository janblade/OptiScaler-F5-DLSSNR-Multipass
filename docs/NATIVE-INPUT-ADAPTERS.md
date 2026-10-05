# Native input: how a game-API adapter is built

DLSS-NR can run in a game that makes no DLSS, FSR or XeSS call. OptiScaler finds the scene's depth itself, estimates the
picture's motion with its own optical flow, builds a trust mask, and runs DLSS-NR on the finished picture. That whole
pipeline runs on **one D3D12 device**. This page is for whoever adds another game API (D3D11, D3D10, D3D9): everything above
the adapter is shared, and an adapter is two small pieces.

```
 game (any API)
   |  hooks: observe, never change, until the processed picture is returned
   v
 Adapter  =  depth observer (hooks -> depth events)  +  IFrameSource (picture and depth -> D3D12)
   |                                                        |
   | depth events (plain numbers)                           | FrameInput / FrameOutput
   v                                                        v
 native::DepthFinderCore                              native::NativeProducer
   counts, picks the scene's depth,                     flow -> trust mask -> guides -> DLSS-NR
   warm-up, stand-down                                    (on its own command list)
```

Files: `OptiScaler/native/DepthFinderCore.{h,cpp}` (events in, pick out), `OptiScaler/native/FrameContract.h` (the frame
interface), `OptiScaler/native/NativeProducer.{h,cpp}` (the shared pipeline), `OptiScaler/resource_tracking/GenericDepth_Select.h`
(the heuristic). The D3D12 adapter, `resource_tracking/GenericDepth_Dx12.cpp` plus `native/Dx12FrameSource`, is the reference.

## 1. The depth observer

Hook the API's calls that bind a depth buffer, clear it and draw into it, and report them to `DepthFinderCore` as plain numbers:

| Event | When |
|---|---|
| `OnDepthBound(context, hadDepth, buffer)` | the context's render targets are set; `buffer` is the depth buffer (null when none or unknown) |
| `OnDepthClear(context, buffer, value)` | a depth clear (flags that clear depth only) |
| `OnDraw(context, vertices, instances)` | every draw (index count for indexed draws) |
| `OnIndirect(context, maxCount)` | every indirect draw |
| `OnViewport(context, width)` | the main viewport changes |
| `OnContextEnd(context)` | a D3D12 command list closes; immediate-context APIs have no such event |
| `BeginPresent` / `EndPresent` | around the adapter's own end-of-frame work, once per presented frame |

A *context* is whatever records draws in order and binds one depth buffer at a time: a D3D12 command list, or the immediate
context of D3D11, D3D10 and D3D9 (use one constant id). A *buffer* has a stable `id` (its pointer will do), size and format.

The core answers some events with a `SnapshotRequest`: copy the picked buffer **now**, into a slot of your own (at a clear, when
the context moves off the buffer, or when a D3D12 list closes while bound). Only the adapter can copy; the core only decides
when. Keep the copies of one frame (`FrameInput::depth[]`, up to 8) and report the picked buffer's view format.

`DepthFinderCore::NoteUpscalerCall()` must be called whenever the game's own upscaler is used (every API's feature provider
already has a place for it): a game that has an upscaler needs no finder, and the core stands down while one is in use.

## 2. The frame source

Implement `native::IFrameSource` (`FrameContract.h`):

- `Acquire(FrameInput&)`: fill `picture` (a **D3D12** resource the producer may read and write, its typed view format, its
  state, its colour space), the depth copies as D3D12 resources, and a `SyncPoint` the producer waits on. Return
  `WaitingForUpscaler` while the game calls an upscaler, `Unavailable` when nothing can be handed over.
- `Return(input, output)`: the producer processed the picture in place; make it ready for the game's present. A zero-copy adapter
  (D3D12) does nothing; a shared-texture adapter copies the picture back into the game's back buffer after waiting on `output.done`.
- `OnResize()`: drop what depends on the size or the device.

Rules for every adapter:
1. Never change the game's picture unless the producer returned a processed one. A failure leaves the picture as the game drew it
   and says why in the status line.
2. Everything that crosses is a D3D12 resource or a fence point, never a pointer into the game's API (so a later bridge can carry
   the same contract across processes).
3. Convert the colour once, in your copy, to a format the producer reads (`pictureFormat`, `colorSpace`). The producer supports
   SDR and scRGB; PQ is reported, not processed.
4. Report depth the game does not let you read as `DepthReadability::NotReadable`, not as a pick that fails later.
5. Free every shared handle on resize, device loss and exit.

## 3. What every adapter must prove

Each API gets a small *fake game* (`tests/fakegame/`, from Story B on) drawing a known moving scene with a hardware depth
format, a shadow map and a HUD. The same checks run for every API: the pick is the scene's depth; the picture handed over
equals the game's back buffer bit for bit; with the producer as a pass-through the returned picture is bit-exact; ten
resizes leak nothing; and a fake upscaler call stands the finder down. `tests/nr_depth_finder_core_smoke.cpp` covers the
core with recorded event sequences, `tests/nr_native_producer_gpu.cpp` the producer through the contract with a synthetic scene.

## 4. Settings and menu

The existing `[DlssNr]` keys are API-agnostic: `NativeDepthFinder`, `NativeMotion`, `NativeInput`, `NativeDebugView`,
`NativeDepthWarmupFrames`. An adapter adds no key and no menu section; it may set the "Source" text of the native input status.
