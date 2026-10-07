// Not built with the precompiled header: no game headers in here, so tests/nr_native_producer_gpu.cpp compiles it alone.
#include "NativeProducer.h"

#include <algorithm>

namespace native
{

NativeProducer::~NativeProducer()
{
    // The queue may still be running our list; the owner waits (WaitFor) before destroying when it matters.
    for (auto& allocator : _allocators)
        if (allocator != nullptr)
            allocator->Release();

    for (auto& readback : _cutReadback)
        if (readback != nullptr)
            readback->Release();

    if (_list != nullptr)
        _list->Release();
    if (_fence != nullptr)
        _fence->Release();
    if (_event != nullptr)
        CloseHandle(_event);
}

void NativeProducer::WaitFor(UINT64 value)
{
    if (_fence == nullptr || value == 0 || _fence->GetCompletedValue() >= value)
        return;

    _fence->SetEventOnCompletion(value, _event);
    WaitForSingleObject(_event, 1000);
}

bool NativeProducer::Init(ID3D12Device* device)
{
    _flow = std::make_unique<OpticalFlowDx12>();
    _trust = std::make_unique<TrustMaskDx12>();

    if (!_flow->Init(device))
    {
        _error = _flow->Error();
        _flow.reset();
        _trust.reset();
        return false;
    }

    if (!_trust->Init(device))
    {
        _error = _trust->Error();
        _flow.reset();
        _trust.reset();
        return false;
    }

    for (auto& allocator : _allocators)
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
        {
            _error = "creating a command allocator";
            return false;
        }

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _allocators[0], nullptr,
                                         IID_PPV_ARGS(&_list))))
    {
        _error = "creating the command list";
        return false;
    }

    _list->Close();

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence))))
    {
        _error = "creating the fence";
        return false;
    }

    // One row of the scene-cut flag per ring slot (a texture copy's rows are 256 bytes apart).
    D3D12_HEAP_PROPERTIES readbackHeap {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readbackDesc {};
    readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDesc.Width = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    readbackDesc.Height = 1;
    readbackDesc.DepthOrArraySize = 1;
    readbackDesc.MipLevels = 1;
    readbackDesc.SampleDesc.Count = 1;
    readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (auto& readback : _cutReadback)
        if (FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
        {
            _error = "creating the scene-cut readback";
            return false;
        }

    _event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    _device = device;
    return true;
}

void NativeProducer::Reset()
{
    if (_flow)
        _flow->Reset();
    if (_trust)
        _trust->Reset();
}

NativeProducer::Result NativeProducer::Run(ID3D12CommandQueue* queue, const FrameInput& input, const Options& options,
                                           const ApplyFn& apply, FrameOutput& output)
{
    Result result;
    output = FrameOutput {};

    if (!Ready() || queue == nullptr || input.picture == nullptr || _list == nullptr)
    {
        result.stoppedAt = !Ready()                   ? "not ready"
                           : queue == nullptr         ? "no queue"
                           : input.picture == nullptr ? "no picture"
                                                      : "no list";
        return result;
    }

    // The previous work may still be reading the textures a new size replaces.
    if (input.width != _width || input.height != _height)
    {
        WaitFor(_signalled);
        _width = input.width;
        _height = input.height;
        Reset();
    }

    // The adapter knows this frame does not continue the last one (a loading screen, a resolution change).
    if (input.cutHint)
        Reset();

    const UINT slot = (UINT) (_frame % kRing);
    WaitFor(_values[slot]);

    // The flow's own cut flag of the frames the GPU has finished since (usually the last one): a cut found there resets
    // the consumer's history now, a couple of frames before the trust mask's count could say so. Until the consumer has
    // run with the reset it stays asked for.
    for (UINT s = 0; s < kRing; ++s)
    {
        if (!_cutPending[s] || _fence->GetCompletedValue() < _values[s])
            continue;

        _cutPending[s] = false;
        const D3D12_RANGE range { 0, sizeof(uint32_t) };
        uint32_t* data = nullptr;

        if (SUCCEEDED(_cutReadback[s]->Map(0, &range, (void**) &data)) && data != nullptr)
        {
            const bool cut = *data != 0;
            const D3D12_RANGE none { 0, 0 };
            _cutReadback[s]->Unmap(0, &none);

            if (cut && _frame >= _cutQuietUntil)
            {
                result.sceneCut = true;
                result.distrustedShare = 1.0f;
                _consumerResetPending = true;
                _cutQuietUntil = _frame + kRing + 2;
            }
        }
    }

    const HRESULT allocatorHr = _allocators[slot]->Reset();
    const HRESULT listHr = FAILED(allocatorHr) ? allocatorHr : _list->Reset(_allocators[slot], nullptr);

    if (FAILED(listHr))
    {
        result.stoppedAt = FAILED(allocatorHr) ? "allocator reset" : "list reset";
        result.stoppedHr = (long) listHr;
        result.deviceRemoved = _device != nullptr ? (long) _device->GetDeviceRemovedReason() : 0;
        return result;
    }

    ID3D12GraphicsCommandList* list = _list;

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = input.picture;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = input.pictureState;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const bool needsBarrier = input.pictureState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    if (needsBarrier)
        list->ResourceBarrier(1, &barrier);

    // The flow's match sees where surfaces end through the depth, when it is one copy of the whole scene (several
    // copies each hold part of it, and one alone would show edges that are not there).
    const bool flowDepth = input.depthCount == 1 && input.depth[0] != nullptr && input.depthView != DXGI_FORMAT_UNKNOWN;
    const OpticalFlowDx12::Encoding encoding = input.colorSpace == ColorSpace::ScRgb ? OpticalFlowDx12::Encoding::ScRgb
                                               : input.colorSpace == ColorSpace::Pq  ? OpticalFlowDx12::Encoding::Pq
                                                                                     : OpticalFlowDx12::Encoding::Srgb;
    const bool recorded =
        _flow->Dispatch(list, input.picture, input.pictureFormat, flowDepth ? input.depth[0] : nullptr,
                        flowDepth ? input.depthView : DXGI_FORMAT_UNKNOWN, input.depthReversed, encoding);

    // This frame's cut flag out to the readback, read once the GPU is done with the frame.
    bool cutCopied = false;

    if (ID3D12Resource* flag = recorded ? _flow->SceneCutFlag() : nullptr)
    {
        D3D12_RESOURCE_BARRIER toCopy {};
        toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy.Transition.pResource = flag;
        toCopy.Transition.Subresource = 0;
        toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &toCopy);

        D3D12_TEXTURE_COPY_LOCATION dst {};
        dst.pResource = _cutReadback[slot];
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT };
        D3D12_TEXTURE_COPY_LOCATION src {};
        src.pResource = flag;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        const D3D12_BOX texel0 { 0, 0, 0, 1, 1, 1 };
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, &texel0);

        std::swap(toCopy.Transition.StateBefore, toCopy.Transition.StateAfter);
        list->ResourceBarrier(1, &toCopy);
        cutCopied = true;
    }

    TrustMaskDx12::Inputs nativeInputs;
    bool nativeReady = false;
    bool nativeReset = false;

    if (recorded && _flow->FlowValid())
    {
        result.flowValid = true;

        if (options.flowPreview)
            _previewReady = _flow->Visualise(list, options.previewMaxSpeed) || _previewReady;

        // The trust mask uses the scene's depth as the adapter copied it this frame; without any it runs on flow and luma.
        TrustMaskDx12::Inputs in;
        in.flow = _flow->Flow();
        in.flowWidth = _flow->FlowWidth();
        in.flowHeight = _flow->FlowHeight();
        in.fullPerFlow = (float) input.width / (float) _flow->FlowWidth();
        in.lumaNow = _flow->LumaOfLastFrame();
        in.lumaBefore = _flow->LumaOfFrameBefore();
        in.sceneCut = _flow->SceneCutFlag();

        if (input.depthCount > 0 && input.depthView != DXGI_FORMAT_UNKNOWN)
        {
            in.depthCount = (std::min)(input.depthCount, (int) TrustMaskDx12::Inputs::kMaxDepths);

            for (int i = 0; i < in.depthCount; ++i)
                in.depths[i] = input.depth[i];

            in.depthFormat = input.depthView;
            in.depthWidth = input.depthWidth;
            in.depthHeight = input.depthHeight;
            in.depthReversed = input.depthReversed;
        }

        result.trustRan = _trust->Dispatch(list, in);
        nativeInputs = in;
        nativeReady = result.trustRan;

        // A hard cut seen by the trust mask's count, a few frames after it: the consumer's history is reset, unless the
        // flow's own flag already asked for that for this cut. With the flow's detector on, the flow and the mask carry
        // on (the cut frame had no motion, and the frames since match the new scene) and the consumer runs on this
        // frame too: skipping it showed one frame without it, a flash over the whole picture. Without the detector, as
        // before: the flow and the mask start over.
        if (_trust->SceneCutSeen() && _frame >= _cutQuietUntil)
        {
            result.sceneCut = true;
            result.distrustedShare = _trust->DistrustedShare();
            nativeReset = true;

            if (_flow->Tuning().sceneCutDetector)
                _cutQuietUntil = _frame + kRing + 2;
            else
            {
                Reset();
                nativeReady = false;
            }
        }
    }

    // Back to the state the adapter expects the picture in, then the consumer on it with the guides, on this same list.
    if (needsBarrier)
    {
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
    }

    if (nativeReady && options.apply && apply)
    {
        if (_trust->BuildGuides(list, nativeInputs, input.width, input.height))
        {
            NativeFrame frame;
            frame.color = input.picture;
            frame.colorFormat = input.pictureFormat;
            frame.pictureState = input.pictureState;
            frame.space = input.colorSpace;
            frame.depth = _trust->GuideDepth();
            frame.motion = _trust->GuideMotion();
            frame.depthReversed = nativeInputs.depthReversed;
            frame.reset = nativeReset || _consumerResetPending;
            result.nativeRan = apply(list, frame);

            if (result.nativeRan)
                _consumerResetPending = false;
        }
    }

    const HRESULT closeHr = list->Close();

    if (SUCCEEDED(closeHr))
    {
        if (input.ready.fence != nullptr)
            queue->Wait(input.ready.fence, input.ready.value);

        ID3D12CommandList* lists[] = { list };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(_fence, ++_signalled);
        _values[slot] = _signalled;
        _cutPending[slot] = cutCopied;
        ++_frame;

        output.done = SyncPoint { _fence, _signalled };
        output.processed = result.nativeRan;
        result.submitted = true;
    }

    if (!result.submitted && result.stoppedAt == nullptr)
    {
        result.stoppedAt = "close";
        result.stoppedHr = (long) closeHr;
        result.deviceRemoved = _device != nullptr ? (long) _device->GetDeviceRemovedReason() : 0;

        // A list whose recording failed stays open, and every later Reset of it fails: make a fresh one so the next
        // frame can try again.
        _list->Release();
        _list = nullptr;

        const HRESULT freshHr = _device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _allocators[slot],
                                                           nullptr, IID_PPV_ARGS(&_list));

        if (SUCCEEDED(freshHr))
            _list->Close();
    }

    return result;
}

} // namespace native
