#include "pch.h"

#include "Dx11FrameSource.h"

#include <dlssnr/DlssNrFeature_Dx12.h>
#include <resource_tracking/GenericDepth_Dx11.h>
#include <with_dx12/with_dx12.h>

namespace native
{

namespace
{
ColorSpace ToColorSpace(DXGI_COLOR_SPACE_TYPE type)
{
    switch (type)
    {
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        return ColorSpace::ScRgb;
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        return ColorSpace::Pq;
    default:
        return ColorSpace::Srgb;
    }
}
} // namespace

void Dx11FrameSource::SetPresent(IDXGISwapChain* swapChain, ID3D11Device* device11)
{
    _swapChain = swapChain;

    if (device11 != _device11)
    {
        _device11 = device11;
        _context11.Reset();
        _context4.Reset();
        _device12 = nullptr;
        _queue12 = nullptr;
        _fence.Reset();
        _picture.Reset();
        _depth.Reset();
    }
}

bool Dx11FrameSource::EnsureDevices(ID3D11Device* device11)
{
    if (_context11 == nullptr)
    {
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        device11->GetImmediateContext(&context);

        if (context == nullptr || FAILED(context.As(&_context4)))
        {
            _error = "the D3D11 device's immediate context does not support ID3D11DeviceContext4 (needs Windows 10 "
                     "1703 or later)";
            return false;
        }

        _context11 = context;
    }

    if (_device12 == nullptr || _queue12 == nullptr)
    {
        if (!WithDx12::PrepareD3D12ForD3D11(device11, D3D_FEATURE_LEVEL_11_0))
        {
            _error = "could not make the D3D12 device paired with this D3D11 device";
            return false;
        }

        _device12 = WithDx12::GetD3D12Device();
        _queue12 = WithDx12::GetD3D12CommandQueue();

        if (_device12 == nullptr || _queue12 == nullptr)
        {
            _error = "the paired D3D12 device or queue is not ready";
            return false;
        }
    }

    if (!_fence.Ready())
    {
        if (!_fence.Create(_device12, device11))
        {
            _error = _fence.Error();
            return false;
        }
    }

    return true;
}

AcquireStatus Dx11FrameSource::Acquire(FrameInput& input)
{
    _backBuffer.Reset();

    if (_swapChain == nullptr || _device11 == nullptr)
        return AcquireStatus::Unavailable;

    if (GenericDepthDx11::GameCallsUpscaler())
        return AcquireStatus::WaitingForUpscaler;

    if (!EnsureDevices(_device11))
        return AcquireStatus::Unavailable;

    if (FAILED(_swapChain->GetBuffer(0, IID_PPV_ARGS(&_backBuffer))))
    {
        _error = "could not get the swap chain's buffer";
        return AcquireStatus::Unavailable;
    }

    D3D11_TEXTURE2D_DESC desc {};
    _backBuffer->GetDesc(&desc);

    const DXGI_FORMAT pictureFormat = SharedPictureFormat(desc.Format);

    if (!_picture.Matches(desc.Width, desc.Height, pictureFormat))
    {
        // DLSS-NR's compose pass writes its result straight back into `color` through a UAV (the same in-place
        // pattern as the D3D12 finished-picture path): shader-resource-only here left the D3D12 side unable to
        // create that view, so the model ran (GPU time, "successful" dispatch) but its write never landed anywhere.
        if (!_picture.Create(_device11, desc.Width, desc.Height, pictureFormat,
                             D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS) ||
            !_picture.Open(_device12))
        {
            _error = _picture.Error();
            static bool loggedPictureFail = false;

            if (!loggedPictureFail)
            {
                loggedPictureFail = true;
                LOG_WARN("Native motion (D3D11): sharing the picture failed: {}", _error);
            }

            return AcquireStatus::Unavailable;
        }
    }

    _context11->CopyResource(_picture.Tex11(), _backBuffer.Get());

    const auto depthSnap = GenericDepthDx11::BestSnapshot();
    bool depthReady = false;

    if (depthSnap.valid)
    {
        bool haveTexture = _depth.Matches(depthSnap.width, depthSnap.height, depthSnap.typelessFormat);

        if (!haveTexture)
            haveTexture = _depth.Create(_device11, depthSnap.width, depthSnap.height, depthSnap.typelessFormat,
                                        D3D11_BIND_SHADER_RESOURCE);

        if (haveTexture && _depth.Open(_device12))
        {
            depthReady = true;
            _context11->CopyResource(_depth.Tex11(), depthSnap.resource);
        }
        else
        {
            static bool loggedDepthFail = false;

            if (!loggedDepthFail)
            {
                loggedDepthFail = true;
                LOG_WARN("Native motion (D3D11): sharing the depth copy failed ({}x{}, typeless format {}): {}",
                         depthSnap.width, depthSnap.height, (int) depthSnap.typelessFormat, _depth.Error());
            }
        }
    }
    else
    {
        static uint64_t noDepthSnapStreak = 0;

        if (++noDepthSnapStreak % 300 == 0)
            LOG_WARN("Native motion (D3D11): the depth finder has no snapshot for this frame ({} frames running without "
                     "one)",
                     noDepthSnapStreak);
    }

    const uint64_t signalValue = _fence.Next();
    _context4->Signal(_fence.Fence11(), signalValue);
    _context11->Flush();

    input = FrameInput {};
    input.api = Api::D3D11;
    input.frame = ++_frame;
    input.picture = _picture.Res12();
    input.pictureFormat = pictureFormat;
    input.pictureState = D3D12_RESOURCE_STATE_COMMON;
    input.colorSpace = ToColorSpace(DlssNr::NativeInputColourSpace(_swapChain, desc.Format));
    input.width = desc.Width;
    input.height = desc.Height;

    if (depthReady)
    {
        input.depth[0] = _depth.Res12();
        input.depthCount = 1;
        input.depthView = depthSnap.viewFormat;
        input.depthWidth = depthSnap.width;
        input.depthHeight = depthSnap.height;
        input.depthReversed = depthSnap.reversed;
    }

    input.ready = SyncPoint { _fence.Fence12(), signalValue };
    return AcquireStatus::Ready;
}

void Dx11FrameSource::Return(const FrameInput& input, const FrameOutput& output)
{
    (void) input;

    if (_backBuffer == nullptr)
        return;

    // output.done names NativeProducer's own internal fence (shared with no one -- it only tells the D3D12 adapter, which
    // reads nothing back across an API boundary, that a frame was submitted). Waiting on it directly here was a bug: it
    // checked output.done.fence for non-null but then waited on OUR shared fence for OUR value, two unrelated counters
    // that happened to track closely enough to let the wait return immediately without ever actually waiting for the
    // producer's GPU work -- so the D3D11 copy-back could run before DLSS-NR's write ever landed, and did.
    // A fresh signal on the private queue, right after the producer's ExecuteCommandLists (already submitted when Run()
    // returned, so this Signal is ordered after it on the GPU regardless of CPU timing), gives the D3D11 context
    // something real to wait on.
    if (output.done.fence != nullptr && _queue12 != nullptr)
    {
        const uint64_t doneValue = _fence.Next();
        _queue12->Signal(_fence.Fence12(), doneValue);
        _context4->Wait(_fence.Fence11(), doneValue);
    }

    _context11->CopyResource(_backBuffer.Get(), _picture.Tex11());
    _backBuffer.Reset();
}

void Dx11FrameSource::OnResize()
{
    _picture.Reset();
    _depth.Reset();
    _backBuffer.Reset();
}

} // namespace native
