#include "pch.h"

#include "Dx12FrameSource.h"

#include <dlssnr/DlssNrFeature_Dx12.h>
#include <resource_tracking/GenericDepth_Dx12.h>

namespace native
{

namespace
{
// The colour format to read the swap chain's buffer through (a typeless buffer needs a typed view).
DXGI_FORMAT ViewFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return format;
    }
}

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

void Dx12FrameSource::SetPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue)
{
    _swapChain = swapChain;
    _queue = queue;
}

void Dx12FrameSource::Release()
{
    if (_backBuffer != nullptr)
    {
        _backBuffer->Release();
        _backBuffer = nullptr;
    }
}

AcquireStatus Dx12FrameSource::Acquire(FrameInput& input)
{
    Release();

    if (_swapChain == nullptr || _queue == nullptr)
        return AcquireStatus::Unavailable;

    // The game has an upscaler of its own: this is not the case the producer is for, and the pictures before and after a
    // change would be compared across it.
    if (GenericDepthDx12::GameCallsUpscaler())
        return AcquireStatus::WaitingForUpscaler;

    IDXGISwapChain3* chain3 = nullptr;
    UINT index = 0;

    if (SUCCEEDED(_swapChain->QueryInterface(IID_PPV_ARGS(&chain3))))
    {
        index = chain3->GetCurrentBackBufferIndex();
        chain3->Release();
    }

    if (FAILED(_swapChain->GetBuffer(index, IID_PPV_ARGS(&_backBuffer))))
        return AcquireStatus::Unavailable;

    const D3D12_RESOURCE_DESC desc = _backBuffer->GetDesc();

    input = FrameInput {};
    input.api = Api::D3D12;
    input.frame = ++_frame;
    input.picture = _backBuffer;
    input.pictureFormat = ViewFormat(desc.Format);
    input.pictureState = D3D12_RESOURCE_STATE_PRESENT;
    input.colorSpace = ToColorSpace(DlssNr::NativeInputColourSpace(_swapChain, desc.Format));
    input.width = (uint32_t) desc.Width;
    input.height = desc.Height;

    // The scene's depth as the finder copied it this frame (none yet: the producer then runs the flow only).
    const auto depth = GenericDepthDx12::BestSnapshot();

    if (depth.valid)
    {
        input.depthCount = (std::min)(depth.copyCount, (int) FrameInput::kMaxDepthCopies);

        for (int i = 0; i < input.depthCount; ++i)
            input.depth[i] = depth.copies[i];

        input.depthView = depth.viewFormat;
        input.depthWidth = depth.width;
        input.depthHeight = depth.height;
        input.depthReversed = depth.reversed;
    }

    return AcquireStatus::Ready;
}

void Dx12FrameSource::Return(const FrameInput& input, const FrameOutput& output)
{
    (void) input;
    (void) output;

    // Zero-copy: the producer worked on the back buffer in place, on the queue the game presents from, so the order of the queue
    // already puts its work before the present.
    Release();
}

} // namespace native
