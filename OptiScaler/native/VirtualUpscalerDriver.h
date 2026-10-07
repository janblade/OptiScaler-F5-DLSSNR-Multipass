#pragma once

// Presents native input (the producer's picture, and the depth finder's depth and the optical flow as guides) to a real
// OptiScaler upscaler backend as a synthetic Evaluate call, for a game that never calls one. Everything keyed off a live
// upscaler feature -- the menu's status, backend switching and, above all, frame generation with the Upscaler input -- then
// works as it does for a game's own call. An alternative to DLSS-NR's native input (DlssNr::ApplyNativeInput); the two are
// mutually exclusive.
//
// The call follows the real one (inputs/NVNGX_DLSS_Dx12.cpp, NVSDK_NGX_D3D12_EvaluateFeature and TryEvaluateOptiFeature):
// DLSS-NR's EvaluateBeforeUpscale, currentFeature, UpscalerInputsDx12::UpscaleStart and UpscaleEnd (which feed frame
// generation), Evaluate, then DLSS-NR's EvaluateAfterUpscale. It must run before frame generation presents, on the game's
// queue: native/NativeDriverDx12.cpp drives it from FGHooks::FGPresent when frame generation owns the swapchain.
//
// Jitter is zero and render size equals output size: there is no upscaling, the backend runs as a temporal stabiliser on the
// native-resolution picture.
//
// The feature is kept out of the real NGX handle tables. Like a game's own feature it makes FeatureProvider_Dx12 record the
// backend it built in Config::Dx12Upscaler, which is also the backend it builds: the user's choice, FFX when there is none.

#include "NativeProducer.h"

#include <d3d12.h>
#include <dxgiformat.h>

#include <memory>
#include <string>

class IFeature_Dx12;
struct NVNGX_Parameters;
enum class Upscaler;

namespace native
{

// True when `feature` (State::currentFeature) is Optical F5Low's own virtual upscaler, not a game's.
bool IsVirtualUpscalerFeature(const void* feature);

class VirtualUpscalerDriver
{
  public:
    VirtualUpscalerDriver();
    ~VirtualUpscalerDriver();

    VirtualUpscalerDriver(const VirtualUpscalerDriver&) = delete;
    VirtualUpscalerDriver& operator=(const VirtualUpscalerDriver&) = delete;

    // A NativeProducer::ApplyFn: one synthetic upscaler call on `frame`, recorded on `cmd`, with the picture written back in
    // place. True when the backend evaluated.
    bool Run(ID3D12GraphicsCommandList* cmd, const NativeFrame& frame);

    // Drops the backend the way a game releasing its feature does: frame generation is told, currentFeature is cleared, and the
    // feature is destroyed once the GPU can no longer be using it. Nothing happens when there is none.
    void Release();

    bool Active() const { return _feature != nullptr; }
    const std::string& Error() const { return _error; }
    const std::string& BackendName() const { return _backendName; }

  private:
    struct Key
    {
        uint32_t width = 0;
        uint32_t height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // typed and UAV-capable: the output's, and the input copy's
        bool depthReversed = false;
        bool hdr = false;
        int backend = 0; // an Upscaler

        bool operator==(const Key&) const = default;
    };

    bool EnsureFeature(ID3D12GraphicsCommandList* cmd, const Key& key, bool rebuild);
    bool CreateFeature(ID3D12GraphicsCommandList* cmd, const Key& key, Upscaler backend);
    void DropFeature(bool destroyFgContext);
    bool EnsureTexture(ID3D12Resource*& texture, Key& made, const Key& key, D3D12_RESOURCE_FLAGS flags,
                       D3D12_RESOURCE_STATES restState, const char* what);
    ID3D12Resource* NeutralDepth(ID3D12GraphicsCommandList* cmd, const Key& key);

    ID3D12Device* _device = nullptr;
    NVNGX_Parameters* _params = nullptr; // one block for the driver's life: features read it at Init only
    std::unique_ptr<IFeature_Dx12> _feature;
    Key _featureKey;
    std::string _backendName;
    bool _failed = false;
    Key _failedKey;

    ID3D12Resource* _output = nullptr; // rests in UNORDERED_ACCESS
    Key _outputKey;
    ID3D12Resource* _input = nullptr; // a typed copy of the picture when the picture itself is typeless; rests in COPY_DEST
    Key _inputKey;
    ID3D12Resource* _neutralDepth = nullptr; // all far, for frames with no depth; rests in NON_PIXEL_SHADER_RESOURCE
    Key _neutralKey;
    bool _neutralFilled = false;
    bool _neutralReversed = false;
    ID3D12DescriptorHeap* _rtvHeap = nullptr;

    uint64_t _evaluations = 0;
    std::string _error;
};

} // namespace native
