#pragma once

// The API-neutral half of the native input producer: given a FrameInput (a finished picture and the scene's depth copies, as
// D3D12 resources) it runs the optical flow, the trust mask and the guides on one command list of its own, and hands the
// picture and the guides to a consumer. It knows nothing of the game's API (native/FrameContract.h); an adapter supplies the
// frames.
//
// The consumer is not linked in here: the caller passes it in as a function (DLSS-NR's DlssNr::ApplyNativeInput, or an
// upscaler backend through native::VirtualUpscalerDriver, in the game; a stand-in in tests/nr_native_producer_gpu.cpp), so
// this class and everything it uses build and run without either.

#include "FrameContract.h"

#include "../motion/OpticalFlow_Dx12.h"
#include "../motion/TrustMask_Dx12.h"

#include <functional>
#include <memory>
#include <string>

namespace native
{

// What the producer hands its consumer for one frame. The consumer works on the picture in place.
struct NativeFrame
{
    ID3D12Resource* color = nullptr;                          // the picture: in pictureState, and left in it
    DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;            // a typed format to view it as (the resource may be typeless)
    D3D12_RESOURCE_STATES pictureState = D3D12_RESOURCE_STATE_COMMON;
    ColorSpace space = ColorSpace::Srgb;
    ID3D12Resource* depth = nullptr;  // the guides (TrustMaskDx12::BuildGuides): picture-sized, unjittered, and in
    ID3D12Resource* motion = nullptr; // NON_PIXEL_SHADER_RESOURCE, which the consumer must leave them in. depth
                                      // is null on a frame without depth.
    bool depthReversed = false;
    bool reset = false;               // the frame does not continue the last one
};

// How a hard scene cut is handled, end to end. Three signals can report one cut; Run() makes it one Result::sceneCut
// and one reset of the consumer.
//
// Signal 1, the flow's detector (OpticalFlowDx12, Tuning().sceneCutDetector, on by default). On the GPU it compares the
// brightness histogram of this picture with the last one's; past sceneCutThreshold it sets a flag on that very frame.
// The flow then matches nothing on that frame (zero motion, no confidence) and the trust mask, given the flag in
// Inputs::sceneCut, is distrust everywhere. The consumer sees neither (it takes the guides only), so its history still
// holds the last scene until signal 2 resets it.
//
// Signal 2, the flag's readback. Run() copies the flag into a readback slot of the frame's ring entry and reads it once
// the GPU has finished that frame, usually in the next Run(). A set flag outside the quiet window (below) reports the
// cut (sceneCut, distrustedShare 1) and asks for the consumer's reset (_consumerResetPending): the next time the
// consumer runs, NativeFrame::reset is set, and the request stays until it has run. This restarts the consumer's
// history a couple of frames before signal 3 could.
//
// Signal 3, the mask's late count. TrustMaskDx12 counts on the GPU the frames in which at least cutShare of the pixels
// are fully distrusted (only frames that had a history count) and reports it through SceneCutSeen() a couple of frames
// later. Seen outside the quiet window it reports the cut and asks for the consumer's reset for this frame. With the
// detector on, the flow and the mask carry on, the consumer runs on this frame too (skipping it showed one frame
// without it, a flash over the whole picture) and a quiet window opens. With the detector off, the flow and the mask
// are reset instead and the consumer does not run until they have a flow again; the reset is then kept asked for
// (_consumerResetPending) until it does. This is the only signal then.
//
// The quiet window (_cutQuietUntil, kRing + 2 frames after a reported cut) is what keeps the late count of a cut that
// signal 2 already reported from being a second cut: a signal inside it is ignored. A cut hint from the adapter
// (FrameInput::cutHint) and a size change are not cuts here: they reset the flow and the mask only, with no report. The
// caller counts Result::sceneCut and logs it (native/NativeDriver.cpp).
class NativeProducer
{
  public:
    // Runs the consumer on `frame`, on `cmd`. True when it ran.
    using ApplyFn = std::function<bool(ID3D12GraphicsCommandList* cmd, const NativeFrame& frame)>;

    struct Options
    {
        bool apply = false;           // run the consumer (needs flow; depth is optional); off: flow and trust mask only
        bool flowPreview = false;     // also draw the flow picture for the menu
        float previewMaxSpeed = 24.0f; // pixels per frame that show as full brightness
    };

    struct Result
    {
        bool submitted = false;  // the work was recorded and sent to the queue (the output is then valid)
        bool flowValid = false;
        bool trustRan = false;   // the trust mask ran in this frame (depth or not; it degrades gracefully with none)
        bool sceneCut = false;   // the mask saw a hard cut: the histories were reset
        float distrustedShare = 0.0f;
        bool nativeRan = false;  // the consumer ran on the picture
        const char* stoppedAt = nullptr; // when nothing was submitted: the step that gave up (for the log)
        long stoppedHr = 0;              // and the HRESULT it got, when it had one
        long deviceRemoved = 0;          // the device's removed reason at that moment (0: not removed)
    };

    NativeProducer() = default;
    ~NativeProducer();

    NativeProducer(const NativeProducer&) = delete;
    NativeProducer& operator=(const NativeProducer&) = delete;

    // Makes the flow, the mask and the command objects on `device`. False with a reason in Error().
    bool Init(ID3D12Device* device);
    bool Ready() const { return _flow != nullptr && _trust != nullptr; }
    ID3D12Device* Device() const { return _device; }
    const std::string& Error() const { return _error; }

    // One frame. `queue` is a direct queue of Device(); the work goes onto it after waiting for input.ready, and output.done is
    // the point it finishes at. The picture is processed in place.
    Result Run(ID3D12CommandQueue* queue, const FrameInput& input, const Options& options, const ApplyFn& apply,
               FrameOutput& output);

    // Forget the previous frames (the game calls an upscaler, a resolution change, a scene cut).
    void Reset();

    // For the menu.
    OpticalFlowDx12* Flow() { return _flow.get(); }
    TrustMaskDx12* Trust() { return _trust.get(); }
    bool PreviewReady() const { return _previewReady; }

  private:
    static constexpr uint32_t kRing = 3;

    void WaitFor(UINT64 value);

    std::unique_ptr<OpticalFlowDx12> _flow;
    std::unique_ptr<TrustMaskDx12> _trust;
    ID3D12Device* _device = nullptr;
    ID3D12CommandAllocator* _allocators[kRing] = {};
    ID3D12GraphicsCommandList* _list = nullptr;
    ID3D12Fence* _fence = nullptr;
    HANDLE _event = nullptr;
    UINT64 _values[kRing] = {};
    UINT64 _signalled = 0;
    uint64_t _frame = 0;
    uint64_t _cutQuietUntil = 0; // a cut's consumer reset was asked for: the late count of the same cut is not another

    // The flow's scene-cut flag of each frame in the ring, copied out to be read once the GPU has finished that frame.
    ID3D12Resource* _cutReadback[kRing] = {};
    bool _cutPending[kRing] = {};
    bool _consumerResetPending = false; // a cut was found and the consumer has not run since
    uint32_t _width = 0;
    uint32_t _height = 0;
    bool _previewReady = false;
    std::string _error;
};

} // namespace native
