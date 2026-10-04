#pragma once

// The API-neutral half of the native input producer: given a FrameInput (a finished picture and the scene's depth copies, as
// D3D12 resources) it runs the optical flow, the trust mask and the guides on one command list of its own, and has DLSS-NR run
// on the picture. It knows nothing of the game's API (native/FrameContract.h); an adapter supplies the frames.
//
// DLSS-NR itself is not linked in here: the caller passes it in as a function (DlssNr::ApplyNativeInput in the game, a stand-in
// in tests/nr_native_producer_gpu.cpp), so this class and everything it uses build and run without the model.

#include "FrameContract.h"

#include "../motion/OpticalFlow_Dx12.h"
#include "../motion/TrustMask_Dx12.h"

#include <functional>
#include <memory>
#include <string>

namespace native
{

class NativeProducer
{
  public:
    // Runs DLSS-NR on `color` (in `pictureState`, left in it) with the guides, on `cmd`. True when the model ran.
    using ApplyNrFn = std::function<bool(ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, ID3D12Resource* depth,
                                         ID3D12Resource* motion, bool depthReversed, bool reset, ColorSpace space,
                                         D3D12_RESOURCE_STATES pictureState)>;

    struct Options
    {
        bool applyNr = false;         // run DLSS-NR (needs depth); off: flow and trust mask only
        bool flowPreview = false;     // also draw the flow picture for the menu
        float previewMaxSpeed = 24.0f; // pixels per frame that show as full brightness
    };

    struct Result
    {
        bool submitted = false;  // the work was recorded and sent to the queue (the output is then valid)
        bool flowValid = false;
        bool trustRan = false;   // the trust mask ran in this frame (there was depth)
        bool sceneCut = false;   // the mask saw a hard cut: the histories were reset
        float distrustedShare = 0.0f;
        bool nativeRan = false;  // DLSS-NR ran on the picture
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
    Result Run(ID3D12CommandQueue* queue, const FrameInput& input, const Options& options, const ApplyNrFn& applyNr,
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
    uint32_t _width = 0;
    uint32_t _height = 0;
    bool _previewReady = false;
    std::string _error;
};

} // namespace native
