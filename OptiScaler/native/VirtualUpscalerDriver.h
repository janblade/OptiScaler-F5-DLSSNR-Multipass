#pragma once

// Story F spike: presents native input (NativeProducer's picture, the depth finder's depth and the optical flow) to a
// real OptiScaler upscaler backend (FFX/FSR) as a synthetic Evaluate call -- no real game call anywhere in the chain --
// so everything already keyed off a live upscaler feature (the menu's status and, above all, frame generation) can work
// in a game that never calls one. Separate from and mutually exclusive with the existing native-input seam
// (DlssNr::ApplyNativeInput, shaders/dlssnr/DlssNr_Late.inl): this does not touch it.
//
// Shaped to match native::NativeProducer::ApplyNrFn exactly, so it is used the same way ApplyNativeInput is: passed as
// the applyNr callback to NativeProducer::Run. No changes to NativeProducer, Dx12FrameSource or the frame acquisition
// path are needed.
//
// Jitter is always zero and render size always equals output size (scale 1.0): there is no upscaling here. FSR runs as
// a temporal stabiliser/AA pass on the native-resolution picture, not a reconstruction. See
// plans/2026-10-04-native-input-multi-api.md, Story F, for the design and the open questions this still carries.
//
// Known side effect, not yet addressed: FeatureProvider_Dx12::GetFeature sets Config::Instance()->Dx12Upscaler to
// whatever backend it created (FFX here) as a matter of course for a real game call -- our synthetic call does the
// same, which can overwrite the user's own upscaler choice in the config. Acceptable for a first spike (opt-in,
// off by default); revisit if the spike proceeds past stage 1.

#include "FrameContract.h"

#include <d3d12.h>
#include <dxgiformat.h>

#include <memory>
#include <string>

class IFeature_Dx12;
struct NVSDK_NGX_Parameter;

namespace native
{

class VirtualUpscalerDriver
{
  public:
    VirtualUpscalerDriver();
    ~VirtualUpscalerDriver();

    VirtualUpscalerDriver(const VirtualUpscalerDriver&) = delete;
    VirtualUpscalerDriver& operator=(const VirtualUpscalerDriver&) = delete;

    // Matches native::NativeProducer::ApplyNrFn. `color` arrives in `pictureState` and must be left there; `depth` and
    // `motion` (the trust mask's guides) arrive already in NON_PIXEL_SHADER_RESOURCE, per TrustMaskDx12::BuildGuides.
    // The device is taken from `cmd` (cached after the first call; this driver does not expect it to change in its
    // lifetime). True when the synthetic Evaluate ran.
    bool Run(ID3D12GraphicsCommandList* cmd, ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motion,
             bool depthReversed, bool reset, ColorSpace space, D3D12_RESOURCE_STATES pictureState);

    const std::string& Error() const { return _error; }

  private:
    bool EnsureFeature(ID3D12GraphicsCommandList* cmd, uint32_t width, uint32_t height, bool depthReversed);
    bool EnsureOutput(DXGI_FORMAT format, uint32_t width, uint32_t height);

    ID3D12Device* _device = nullptr;
    NVSDK_NGX_Parameter* _params = nullptr;
    std::unique_ptr<IFeature_Dx12> _feature;
    uint32_t _featureWidth = 0, _featureHeight = 0;
    bool _featureDepthReversed = false;

    ID3D12Resource* _output = nullptr;
    uint32_t _outputWidth = 0, _outputHeight = 0;
    DXGI_FORMAT _outputFormat = DXGI_FORMAT_UNKNOWN;

    std::string _error;
};

} // namespace native
