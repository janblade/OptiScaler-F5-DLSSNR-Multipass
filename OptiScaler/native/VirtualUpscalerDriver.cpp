#include "pch.h"

#include "VirtualUpscalerDriver.h"

#include "DepthFinderCore.h"

#include <NVNGX_Parameter.h>
#include <State.h>
#include <Config.h>
#include <Util.h>
#include <upscalers/FeatureProvider_Dx12.h>
#include <upscalers/IFeature_Dx12.h>
#include <inputs/FG/Upscaler_Inputs_Dx12.h>

namespace native
{

namespace
{
// Our feature's handle id: never in the real NGX handle tables (HandleToFeature, Dx12Contexts), so no game call finds it. It is
// in State::changeBackend, so the menu's backend switch reaches it like any other feature.
constexpr UINT kVirtualHandleId = 0x4E524631; // 'NRF1'

// Releases a texture once the GPU can no longer be using it, by the same delay Util::DelayedDestroy gives features.
struct ReleaseLater
{
    explicit ReleaseLater(ID3D12Resource* resource) : resource(resource) {}
    ~ReleaseLater() { resource->Release(); }
    ReleaseLater(const ReleaseLater&) = delete;
    ReleaseLater& operator=(const ReleaseLater&) = delete;
    ID3D12Resource* resource;
};

void ReleaseTexture(ID3D12Resource*& texture)
{
    if (texture == nullptr)
        return;

    Util::DelayedDestroy(std::make_unique<ReleaseLater>(texture));
    texture = nullptr;
}

void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after)
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
}

// UAVs cannot be sRGB; the bytes are the same, so the copy in and out keeps the picture exact.
DXGI_FORMAT UavFormat(DXGI_FORMAT view)
{
    switch (view)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    default:
        return view;
    }
}

bool IsUnrealGame()
{
    const auto& state = State::Instance();
    return state.NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL || state.gameEngine == GameEngineType::Unreal ||
           state.gameQuirks & GameQuirk::ForceUnrealEngine;
}

// The FSR and XeSS backends take each input in the state its *ResourceBarrier setting names (and hand it back in it), and in
// an Unreal game set colour and motion to RENDER_TARGET and UNORDERED_ACCESS on their first Evaluate. Setting those here,
// before UpscaleStart, makes frame generation's copies (which read the same settings) agree with the backend from the first
// frame. DLSS takes everything in its working state.
bool UsesBarrierSettings(Upscaler backend) { return backend != Upscaler::DLSS && backend != Upscaler::DLSSD; }

D3D12_RESOURCE_STATES SettingOr(const CustomOptional<int32_t, NoDefault>& setting, D3D12_RESOURCE_STATES working)
{
    return setting.has_value() ? (D3D12_RESOURCE_STATES) setting.value() : working;
}

Upscaler WantedBackend()
{
    const auto& cfg = *Config::Instance();
    const Upscaler wanted = cfg.Dx12Upscaler.has_value() ? cfg.Dx12Upscaler.value() : Upscaler::FFX;

    // Ray Reconstruction needs inputs we do not have; the config stores it as DLSS anyway.
    return wanted == Upscaler::DLSSD ? Upscaler::DLSS : wanted;
}
} // namespace

VirtualUpscalerDriver::VirtualUpscalerDriver() = default;

VirtualUpscalerDriver::~VirtualUpscalerDriver()
{
    Release();
    ReleaseTexture(_output);
    ReleaseTexture(_input);
    delete _params;
}

void VirtualUpscalerDriver::DropFeature(bool destroyFgContext)
{
    auto& state = State::Instance();

    // What NVSDK_NGX_D3D12_ReleaseFeature (destroyFgContext) and FeatureProvider_Dx12::ChangeFeature (not) do for a game's
    // feature.
    if (state.currentFG != nullptr && state.activeFgInput == FGInput::Upscaler)
    {
        state.fgChanged = true;
        state.clearCapturedHudlesses = true;

        if (destroyFgContext)
            state.currentFG->DestroyFGContext();
    }

    UpscalerInputsDx12::Reset();

    if (state.currentFeature == _feature.get())
        state.currentFeature = nullptr;

    Util::DelayedDestroy(std::move(_feature));
}

void VirtualUpscalerDriver::Release()
{
    if (_feature == nullptr)
        return;

    LOG_INFO("Virtual upscaler: releasing the {} backend", _backendName);
    DropFeature(true);
    State::Instance().changeBackend.erase(kVirtualHandleId);
    _backendName.clear();
}

bool VirtualUpscalerDriver::CreateFeature(ID3D12GraphicsCommandList* cmd, const Key& key, Upscaler backend)
{
    _params->Set(NVSDK_NGX_Parameter_Width, key.width);
    _params->Set(NVSDK_NGX_Parameter_Height, key.height);
    _params->Set(NVSDK_NGX_Parameter_OutWidth, key.width);
    _params->Set(NVSDK_NGX_Parameter_OutHeight, key.height);
    _params->Set(NVSDK_NGX_Parameter_PerfQualityValue, 1);

    // Not jittered and not low-res: the guides are output-sized, unjittered motion. AutoExposure because there is no exposure
    // texture to give: without both, FFXFeatureDx12::EvaluateInternal returns true having dispatched nothing.
    unsigned int flags = NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

    if (key.depthReversed)
        flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;

    if (key.hdr)
        flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;

    _params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);

    std::unique_ptr<IFeature_Dx12> feature;

    if (!FeatureProvider_Dx12::GetFeature(backend, kVirtualHandleId, _params, &feature) || feature == nullptr)
    {
        _error = std::format("could not create the {} backend", UpscalerDisplayName(backend));
        LOG_WARN("Virtual upscaler: {}", _error);
        return false;
    }

    if (!feature->Init(_device, cmd, _params))
    {
        _error = std::format("the {} backend failed to initialise", feature->Name());
        LOG_WARN("Virtual upscaler: {}", _error);
        Util::DelayedDestroy(std::move(feature));
        return false;
    }

    _feature = std::move(feature);
    return true;
}

bool VirtualUpscalerDriver::EnsureFeature(ID3D12GraphicsCommandList* cmd, const Key& key, bool rebuild)
{
    if (_feature != nullptr && !rebuild && key == _featureKey)
        return true;

    // Tried and failed for exactly this: not again every frame.
    if (_feature == nullptr && !rebuild && _failed && key == _failedKey)
        return false;

    const bool replacing = _feature != nullptr;

    if (replacing)
    {
        LOG_INFO("Virtual upscaler: rebuilding the {} backend", _backendName);
        DropFeature(false);
    }

    const auto backend = (Upscaler) key.backend;

    // Like the real path's fallback when a chosen backend will not initialise; FFX needs nothing from the game.
    if (!CreateFeature(cmd, key, backend) && (backend == Upscaler::FFX || !CreateFeature(cmd, key, Upscaler::FFX)))
    {
        _failed = true;
        _failedKey = key;
        return false;
    }

    _failed = false;
    auto& state = State::Instance();

    // GetFeature recorded what it built in Dx12Upscaler, which WantedBackend reads: the key matches it from the next frame on.
    _featureKey = key;
    _featureKey.backend = (int) WantedBackend();
    _backendName = _feature->Name();
    _error.clear();

    state.changeBackend[kVirtualHandleId] = false;

    if (replacing && state.currentFG != nullptr && state.activeFgInput == FGInput::Upscaler)
        state.currentFG->UpdateTarget();

    LOG_INFO("Virtual upscaler: {} ready, {}x{}, depth reversed {}, HDR {}", _backendName, key.width, key.height,
             key.depthReversed, key.hdr);
    return true;
}

bool VirtualUpscalerDriver::EnsureTexture(ID3D12Resource*& texture, Key& made, const Key& key,
                                          D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES restState, const char* what)
{
    if (texture != nullptr && made.width == key.width && made.height == key.height && made.format == key.format)
        return true;

    ReleaseTexture(texture);

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = key.width;
    desc.Height = key.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = key.format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;

    const HRESULT hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, restState, nullptr,
                                                        IID_PPV_ARGS(&texture));

    if (FAILED(hr))
    {
        texture = nullptr;
        _error = std::format("creating the {}x{} {} texture (format {}) failed: {:X}", key.width, key.height, what,
                             (int) key.format, (UINT) hr);
        LOG_WARN("Virtual upscaler: {}", _error);
        return false;
    }

    made = key;
    return true;
}

bool VirtualUpscalerDriver::Run(ID3D12GraphicsCommandList* cmd, const NativeFrame& frame)
{
    if (cmd == nullptr || frame.color == nullptr || frame.depth == nullptr || frame.motion == nullptr)
        return false;

    if (_device == nullptr)
    {
        Microsoft::WRL::ComPtr<ID3D12Device> device;

        if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))))
        {
            _error = "could not get the D3D12 device from the command list";
            return false;
        }

        _device = device.Get(); // not kept alive by us: lives as long as the game's device
    }

    if (_params == nullptr)
        _params = new NVNGX_Parameters(API::DX12, false);

    const D3D12_RESOURCE_DESC colorDesc = frame.color->GetDesc();

    Key key;
    key.width = (uint32_t) colorDesc.Width;
    key.height = colorDesc.Height;
    key.format = UavFormat(frame.colorFormat != DXGI_FORMAT_UNKNOWN ? frame.colorFormat : colorDesc.Format);
    key.depthReversed = frame.depthReversed;
    key.hdr = frame.space == ColorSpace::ScRgb; // PQ is a perceptual encoding, as sRGB is: passed as it comes
    key.backend = (int) WantedBackend();

    auto& state = State::Instance();
    auto& cfg = *Config::Instance();

    // Our own call, for all of it: GetFeature and Evaluate both tell the depth finder a game is calling an upscaler.
    SyntheticUpscalerCallScope guard;

    // A backend switch from the menu, or a backend asking to be rebuilt (FeatureProvider_Dx12::ChangeFeature's job for a game's
    // feature).
    bool rebuild = false;

    if (const auto change = state.changeBackend.find(kVirtualHandleId);
        _feature != nullptr && change != state.changeBackend.end() && change->second)
    {
        if (state.newBackend != Upscaler::Reset)
        {
            key.backend = (int) (state.newBackend == Upscaler::DLSSD ? Upscaler::DLSS : state.newBackend);
            state.newBackend = Upscaler::Reset;
        }

        change->second = false;
        rebuild = true;
    }

    if (!EnsureFeature(cmd, key, rebuild))
        return false;

    const bool copyIn = colorDesc.Format != key.format;

    if (!EnsureTexture(_output, _outputKey, key, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "output"))
        return false;

    if (copyIn && !EnsureTexture(_input, _inputKey, key, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, "input"))
        return false;

    // Frame generation's Upscaler input only has a device once NVSDK_NGX_D3D12_Init gave it one, which a game with no upscaler
    // never calls.
    UpscalerInputsDx12::Init(_device);

    const auto backend = _feature->GetUpscalerType();
    const bool usesSettings = UsesBarrierSettings(backend);

    if (usesSettings && IsUnrealGame())
    {
        if (!cfg.ColorResourceBarrier.has_value())
            cfg.ColorResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_RENDER_TARGET);

        if (!cfg.MVResourceBarrier.has_value())
            cfg.MVResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr auto kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // Frame generation's copies (UpscaleStart/UpscaleEnd) read the settings whatever the backend.
    const auto fgMotion = SettingOr(cfg.MVResourceBarrier, kRead);
    const auto fgDepth = SettingOr(cfg.DepthResourceBarrier, kRead);
    const auto fgOutput = SettingOr(cfg.OutputResourceBarrier, kWrite);

    const auto inColor = usesSettings ? SettingOr(cfg.ColorResourceBarrier, kRead) : kRead;
    const auto inMotion = usesSettings ? fgMotion : kRead;
    const auto inDepth = usesSettings ? fgDepth : kRead;
    const auto inOutput = usesSettings ? fgOutput : kWrite;

    // The colour the backend reads: the picture itself, or a typed copy of it. The picture stays in COPY_SOURCE until the
    // result is copied back over it.
    ID3D12Resource* color = frame.color;
    D3D12_RESOURCE_STATES pictureNow = frame.pictureState;

    if (copyIn)
    {
        Transition(cmd, frame.color, pictureNow, D3D12_RESOURCE_STATE_COPY_SOURCE);
        pictureNow = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cmd->CopyResource(_input, frame.color);
        Transition(cmd, _input, D3D12_RESOURCE_STATE_COPY_DEST, inColor);
        color = _input;
    }
    else
    {
        Transition(cmd, frame.color, pictureNow, inColor);
        pictureNow = inColor;
    }

    _params->Set(NVSDK_NGX_Parameter_Color, color);
    _params->Set(NVSDK_NGX_Parameter_Depth, frame.depth);
    _params->Set(NVSDK_NGX_Parameter_MotionVectors, frame.motion);
    _params->Set(NVSDK_NGX_Parameter_Output, _output);
    _params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    _params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    _params->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    _params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    _params->Set(NVSDK_NGX_Parameter_Reset, frame.reset ? 1u : 0u);
    _params->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
    _params->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);

    Transition(cmd, frame.motion, kRead, fgMotion);
    Transition(cmd, frame.depth, kRead, fgDepth);
    Transition(cmd, _output, kWrite, fgOutput);

    state.currentFeature = _feature.get();

    UpscalerInputsDx12::UpscaleStart(cmd, _params, _feature.get());
    UpscalerInputsDx12::UpscaleEnd(cmd, _params, _feature.get());

    Transition(cmd, frame.motion, fgMotion, inMotion);
    Transition(cmd, frame.depth, fgDepth, inDepth);
    Transition(cmd, _output, fgOutput, inOutput);

    bool evaluated = false;
    {
        ScopedSkipHeapCapture skip {};
        evaluated = _feature->Evaluate(cmd, _params);
    }

    if (_evaluations < 8 || _evaluations % 300 == 0)
        LOG_INFO("Virtual upscaler: Evaluate {} -> {}, {} {}x{}, colour format {}{}, reset {}, depth reversed {}",
                 _evaluations, evaluated, _backendName, key.width, key.height, (int) key.format,
                 copyIn ? " (copied)" : "", frame.reset, _featureKey.depthReversed);

    ++_evaluations;

    // The backend hands its inputs back in the states it took them in.
    Transition(cmd, frame.motion, inMotion, kRead);
    Transition(cmd, frame.depth, inDepth, kRead);

    if (copyIn)
        Transition(cmd, _input, inColor, D3D12_RESOURCE_STATE_COPY_DEST);

    if (evaluated)
    {
        Transition(cmd, _output, inOutput, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(cmd, frame.color, pictureNow, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(frame.color, _output);
        Transition(cmd, frame.color, D3D12_RESOURCE_STATE_COPY_DEST, frame.pictureState);
        Transition(cmd, _output, D3D12_RESOURCE_STATE_COPY_SOURCE, kWrite);
    }
    else
    {
        Transition(cmd, frame.color, pictureNow, frame.pictureState);
        Transition(cmd, _output, inOutput, kWrite);
    }

    return evaluated;
}

} // namespace native
