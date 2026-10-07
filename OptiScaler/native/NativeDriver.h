#pragma once

// The game-side half of the native input producer that D3D12 and D3D11 share: one frame step on a native::IFrameSource,
// and the menu's status and tuning that go with it.
//
// A driver per API (native/NativeDriverDx12.cpp, native/NativeDriverDx11.cpp) owns an adapter (Dx12FrameSource,
// Dx11FrameSource) and one NativeDriver, and keeps only what is its API's: when the step runs (the frame generation
// hooks, the threads), the depth finder's frame close, the menu's pictures. Everything else lives here: the [DlssNr]
// gate, Acquire, the producer made again when its device changes, the virtual upscaler's life, the consumer (DLSS-NR or
// the virtual upscaler), the bookkeeping, the log lines and the status line.

#include "FrameContract.h"
#include "NativeProducer.h"

#include <cstdint>
#include <memory>
#include <string>

namespace native
{

class VirtualUpscalerDriver;

class NativeDriver
{
  public:
    enum class Status
    {
        Off,
        Waiting, // the game calls an upscaler
        Failed,
        Running
    };

    struct RunResult
    {
        bool ran = false;       // the producer was run (and the source's frame given back)
        bool submitted = false; // its work was recorded and sent: the source's picture is the result
    };

    // `tag` starts every log line ("Native motion", "Native motion (D3D11)"); `readyNote` is added to the line that
    // says the producer is ready.
    NativeDriver(const char* tag, const char* readyNote) : _tag(tag), _readyNote(readyNote) {}

    // Not copyable, and the virtual upscaler is never destroyed here (see _virtualUpscaler).
    NativeDriver(const NativeDriver&) = delete;
    NativeDriver& operator=(const NativeDriver&) = delete;

    // The first thing a present does: false while [DlssNr] NativeMotion is off (the virtual upscaler is released and
    // the status goes back to Off).
    bool Enabled();

    // Once the producer could not start nothing more is tried (the status line says why).
    bool Failed() const { return _status == Status::Failed; }

    // One frame: Acquire on `source` (its SetPresent is done), the producer, Return. `flowPreview` asks the producer
    // for the flow picture the menu shows.
    RunResult RunFrame(IFrameSource& source, bool flowPreview);

    // ImGui: one line for the mode in effect (waiting/failed, NR or the stabiliser on this picture, or why not).
    void DrawStatus();

    // ImGui: the optical flow's tuning, with NativeDebugView on and a producer made. Applies at once.
    void DrawFlowTuning();

    // ImGui: which one of the trust mask's checks to look at (the mask picture shows it).
    void DrawTrustViewCombo();

    // For the API drivers' menu pictures and status text.
    NativeProducer* Producer() { return _producer.get(); }
    Status GetStatus() const { return _status; }
    uint64_t Frame() const { return _frame; }
    uint64_t Cuts() const { return _cuts; }
    // The mask ran within the last 30 frames. A frame can have no mask (no motion estimate yet); the picture then keeps
    // the last one rather than going black.
    bool TrustRecent() const { return _trustFrame != 0 && _frame - _trustFrame < 30; }

  private:
    void ReleaseVirtualUpscaler();
    void LogDiagnostics();

    const char* _tag;
    const char* _readyNote;

    std::unique_ptr<NativeProducer> _producer;

    // Made on first use and never destroyed at exit: its destructor would tear down an upscaler backend under the
    // loader lock. Deleted only when the producer's device changes.
    VirtualUpscalerDriver* _virtualUpscaler = nullptr;

    uint64_t _frame = 0;      // frames the producer submitted
    bool _nativeRan = false;  // DLSS-NR or the virtual upscaler ran on native input in the last frame
    uint64_t _trustFrame = 0; // the last frame the trust mask was recorded in
    uint64_t _cuts = 0;       // scene cuts the mask reported
    Status _status = Status::Off;
    std::string _failure;

    // A line every 300 presents says where the frames go, so a "waiting for the motion estimate" that never ends can be
    // told apart: no picture (Acquire not ready), flow never valid, trust mask not running, or the backend not
    // applying.
    uint64_t _diagPresents = 0, _diagReady = 0, _diagWaiting = 0, _diagFlowValid = 0, _diagTrust = 0, _diagNative = 0,
             _diagSubmitted = 0;
    uint64_t _stopped = 0; // frames the producer gave up on
    uint64_t _seen = 0,
             _ran = 0; // frames with a valid flow, and those the trust mask ran in: the log shows it every 600
};

} // namespace native
