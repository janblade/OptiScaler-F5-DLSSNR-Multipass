#include "pch.h"

#include "VirtualUpscalerDriver.h"

#include "DepthFinderCore.h"

#include <NVNGX_Parameter.h>
#include <State.h>
#include <Config.h>
#include <upscalers/FeatureProvider_Dx12.h>
#include <upscalers/IFeature_Dx12.h>
#include <inputs/FG/Upscaler_Inputs_Dx12.h>

namespace native
{

namespace
{
// Our own handle id for the synthetic feature: never registered in the real NGX handle tables (HandleToFeature,
// Dx12Contexts), so it cannot collide with a real game's handles or be found by a real NGX call.
constexpr UINT kVirtualHandleId = 0x4E524631; // 'NRF1'
} // namespace

VirtualUpscalerDriver::VirtualUpscalerDriver() = default;

VirtualUpscalerDriver::~VirtualUpscalerDriver()
{
    // The feature must be destroyed before the parameters it was built with; the unique_ptr does that first.
    _feature.reset();

    if (_params != nullptr)
    {
        delete _params;
        _params = nullptr;
    }

    if (_output != nullptr)
    {
        _output->Release();
        _output = nullptr;
    }
}

bool VirtualUpscalerDriver::EnsureFeature(ID3D12GraphicsCommandList* cmd, uint32_t width, uint32_t height,
                                          bool depthReversed)
{
    if (_feature != nullptr && _featureWidth == width && _featureHeight == height &&
        _featureDepthReversed == depthReversed)
        return true;

    _feature.reset();

    if (_params != nullptr)
    {
        delete _params;
        _params = nullptr;
    }

    // Our own parameter block, built the same way NVSDK_NGX_D3D12_AllocateParameters' non-NVIDIA fallback builds one for
    // a real game (inputs/NVNGX_DLSS_Dx12.cpp) -- no real game call, no real NVIDIA NGX core needed.
    _params = new NVNGX_Parameters(API::DX12, false);

    _params->Set(NVSDK_NGX_Parameter_Width, width);
    _params->Set(NVSDK_NGX_Parameter_Height, height);
    _params->Set(NVSDK_NGX_Parameter_OutWidth, width);
    _params->Set(NVSDK_NGX_Parameter_OutHeight, height);
    _params->Set(NVSDK_NGX_Parameter_PerfQualityValue, 1);

    // Not jittered, not low-res: the trust mask's guide (TrustMaskDx12::BuildGuides) is already full (output)
    // resolution motion with no jitter applied, since render size == output size here (scale 1.0, no upscaling).
    // AutoExposure is required: we supply no NVSDK_NGX_Parameter_ExposureTexture (we have no such thing), and
    // FFXFeatureDx12::EvaluateInternal, finding AutoExposure() false and no exposure texture, returns true
    // without ever dispatching anything -- a silent no-op that looks identical to success in every log we have,
    // which is why the picture was black: nothing ever wrote to it. FSR computes its own exposure internally when
    // this flag is set, which is exactly our situation.
    unsigned int flags = NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

    if (depthReversed)
        flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;

    _params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);

    // Our own call: must not stand down the depth finder/optical flow pipeline that is feeding it.
    SyntheticUpscalerCallScope guard;

    std::unique_ptr<IFeature_Dx12> feature;

    if (!FeatureProvider_Dx12::GetFeature(Upscaler::FFX, kVirtualHandleId, _params, &feature) || feature == nullptr)
    {
        _error = "could not create the FSR backend";
        LOG_WARN("Story F: {}", _error);
        delete _params;
        _params = nullptr;
        return false;
    }

    if (!feature->Init(_device, cmd, _params))
    {
        _error = "the FSR backend failed to initialise";
        LOG_WARN("Story F: {}", _error);
        delete _params;
        _params = nullptr;
        return false;
    }

    _feature = std::move(feature);
    _featureWidth = width;
    _featureHeight = height;
    _featureDepthReversed = depthReversed;
    _error.clear();
    LOG_INFO("Story F: FSR backend ready, {}x{}, depth reversed {}", width, height, depthReversed);
    return true;
}

bool VirtualUpscalerDriver::EnsureOutput(DXGI_FORMAT format, uint32_t width, uint32_t height)
{
    if (_output != nullptr && _outputWidth == width && _outputHeight == height && _outputFormat == format)
        return true;

    if (_output != nullptr)
    {
        _output->Release();
        _output = nullptr;
    }

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // Created already in the state Evaluate expects its Output in (FFXFeatureDx12::EvaluateInternal does not barrier
    // Output itself -- the caller is expected to hand it over ready).
    const HRESULT hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                        IID_PPV_ARGS(&_output));

    if (FAILED(hr))
    {
        _error = std::format("creating the {}x{} output scratch texture failed: {:X}", width, height, (UINT) hr);
        LOG_WARN("Story F: {} (format {})", _error, (int) format);
        _output = nullptr;
        return false;
    }

    _outputWidth = width;
    _outputHeight = height;
    _outputFormat = format;
    return true;
}

bool VirtualUpscalerDriver::Run(ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, ID3D12Resource* depth,
                                ID3D12Resource* motion, bool depthReversed, bool reset, ColorSpace space,
                                D3D12_RESOURCE_STATES pictureState)
{
    if (cmd == nullptr || color == nullptr || depth == nullptr || motion == nullptr)
        return false;

    if (_device == nullptr)
    {
        Microsoft::WRL::ComPtr<ID3D12Device> device;

        if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))))
        {
            _error = "could not get the D3D12 device from the command list";
            return false;
        }

        _device = device.Get(); // kept raw, not AddRef'd further: lives exactly as long as the game's own device does
    }

    const D3D12_RESOURCE_DESC colorDesc = color->GetDesc();
    const auto width = (uint32_t) colorDesc.Width;
    const auto height = colorDesc.Height;

    // Our own call, for the whole sequence below: feature creation (if needed), UpscaleStart/End and Evaluate.
    SyntheticUpscalerCallScope guard;

    if (!EnsureFeature(cmd, width, height, depthReversed))
        return false;

    if (!EnsureOutput(colorDesc.Format, width, height))
        return false;

    _params->Set(NVSDK_NGX_Parameter_Color, color);
    _params->Set(NVSDK_NGX_Parameter_Depth, depth);
    _params->Set(NVSDK_NGX_Parameter_MotionVectors, motion);
    _params->Set(NVSDK_NGX_Parameter_Output, _output);
    _params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    _params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    _params->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    _params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    _params->Set(NVSDK_NGX_Parameter_Reset, reset ? 1u : 0u);
    _params->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
    _params->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    (void) space; // HDR encoding of the finished picture is not threaded through yet (SDR/scRGB both pass as-is); see
                  // plans/2026-10-04-native-input-multi-api.md, Story F, stage 3.

    const auto barrier = [cmd](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        if (before == after)
            return;

        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        cmd->ResourceBarrier(1, &b);
    };

    // `color` must be NON_PIXEL_SHADER_RESOURCE for Evaluate (FFXFeatureDx12::EvaluateInternal does not barrier its
    // inputs itself; the caller is expected to hand them over ready -- see the file for the (opt-in, off by default)
    // exceptions it makes for Unreal and a user override, neither of which apply here).
    barrier(color, pictureState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    State::Instance().currentFeature = _feature.get();

    // The exact sequence NVNGX_DLSS_Dx12.cpp's TryEvaluateOptiFeature uses for a real game call: UpscaleStart/UpscaleEnd
    // is what feeds frame generation (inputs/FG/Upscaler_Inputs_Dx12.cpp), then the feature itself.
    UpscalerInputsDx12::UpscaleStart(cmd, _params, _feature.get());
    UpscalerInputsDx12::UpscaleEnd(cmd, _params, _feature.get());

    const bool evaluated = _feature->Evaluate(cmd, _params);

    static int calls = 0;

    if (calls < 8 || calls % 300 == 0)
        LOG_INFO("Story F: Evaluate call {} -> {}, {}x{} colour format {}, depth format {} motion format {}, "
                 "reset {}, depth reversed {}",
                 calls, evaluated, width, height, (int) colorDesc.Format, (int) depth->GetDesc().Format,
                 (int) motion->GetDesc().Format, reset, depthReversed);

    ++calls;

    if (evaluated)
    {
        // Output -> color, the same "copy the result into the real target" pattern the frame sources use.
        barrier(color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        barrier(_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

        cmd->CopyResource(color, _output);

        barrier(color, D3D12_RESOURCE_STATE_COPY_DEST, pictureState);
        barrier(_output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    else
    {
        // Nothing wrote to color: put it back where it came from.
        barrier(color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, pictureState);
    }

    return evaluated;
}

} // namespace native
