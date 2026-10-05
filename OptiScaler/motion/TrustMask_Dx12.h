#pragma once

// The trust mask of the native input producer (Story 3): for every pixel of the flow, how little the previous frame's history
// can be trusted there. 0 trusts it fully, 1 not at all; a model that reprojects history leans on the current frame in
// proportion. Like OpticalFlowDx12 it is self-contained (D3D12 and the HLSL compiler), so tests/nr_trust_mask_gpu.cpp tests
// it with synthetic scenes of known geometry.
//
// The checks follow the ideas of the validation in DLSS5-Feeder's DLSS5_Feed.fx (MIT License, Copyright (c) 2026 Jean-Laurent
// ROUZIES; portions Copyright (c) 2026 NIGos, MIT): follow the flow back to the previous frame and distrust the pixel
//   - where the depth there is not the depth here (a surface that was not visible: disocclusion), and where a much nearer
//     surface was at this very pixel a frame ago (a surface that has moved off and uncovered this one),
//   - where the motion there is not the motion here (flow that jumps from frame to frame),
//   - where the luma there is outside what the neighbourhood here holds (lighting or content that changed),
// plus a decaying memory of earlier distrust (a pixel that was distrusted last frame is trusted again gradually, which is the
// hysteresis). The implementation here is our own, written from those ideas; the thresholds are this module's.
//
// A scene cut shows as a frame in which nearly every pixel is distrusted; Dispatch() counts those on the GPU and a couple of
// frames later SceneCutSeen() reports it, so the owner can reset the producer's histories.

#include <d3d12.h>
#include <dxgiformat.h>
#include <cstdint>
#include <string>

class TrustMaskDx12
{
  public:
    TrustMaskDx12() = default;
    ~TrustMaskDx12();

    TrustMaskDx12(const TrustMaskDx12&) = delete;
    TrustMaskDx12& operator=(const TrustMaskDx12&) = delete;

    bool Init(ID3D12Device* device);

    struct Inputs
    {
        ID3D12Resource* flow = nullptr;          // OpticalFlowDx12::Flow(), RGBA16F, full-resolution pixels, prev minus current
        uint32_t flowWidth = 0, flowHeight = 0;
        float fullPerFlow = 2.0f;                // picture pixels per flow pixel
        ID3D12Resource* lumaNow = nullptr;       // OpticalFlowDx12::LumaOfLastFrame(), the flow's size
        ID3D12Resource* lumaBefore = nullptr;    // OpticalFlowDx12::LumaOfFrameBefore()
        // The scene's depth: one or more copies of the depth buffer (any size, all the same), readable as textures. With several
        // each is part of the scene (what one list drew) and the nearest surface over them all is used.
        static constexpr int kMaxDepths = 8;
        ID3D12Resource* depths[kMaxDepths] = {};
        int depthCount = 0;
        DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN; // a typed readable format of it: R32_FLOAT, R16_UNORM, R32_FLOAT_X8X24_TYPELESS...
        uint32_t depthWidth = 0, depthHeight = 0;
        bool depthReversed = true;               // near is 1.0
    };

    // Records the mask for this frame; every input must be in the NON_PIXEL_SHADER_RESOURCE state and stays so. The first
    // call after Reset() (and the first with a valid flow) has no history and returns a mask of ones.
    bool Dispatch(ID3D12GraphicsCommandList* list, const Inputs& in);

    // The guides the DLSS-NR seam takes (Story 4), at the picture's size: the depth (raw, in the convention it came in,
    // the nearest over the copies) as R32_FLOAT, and the flow bilinearly enlarged as RGBA16F in picture pixels towards
    // the previous frame. Both rest in NON_PIXEL_SHADER_RESOURCE. The inputs are those of Dispatch(). GuideDepth() is
    // null after a call without depth.
    bool BuildGuides(ID3D12GraphicsCommandList* list, const Inputs& in, uint32_t width, uint32_t height);
    ID3D12Resource* GuideDepth() const { return _guideDepthValid ? _guideDepth.resource : nullptr; }
    ID3D12Resource* GuideMotion() const { return _guideMotion.resource; }

    // Drops the histories (a scene cut, a size change): the next mask is all distrust.
    void Reset()
    {
        _haveHistory = false;
        _share = -1.0f;

        for (auto& frame : _readbackFrame)
            frame = 0;
    }

    // R8_UNORM at the flow's size, NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE between Dispatches (a menu can show it).
    ID3D12Resource* Mask() const { return _mask[_maskIndex].resource; }

    // The share of pixels distrusted almost fully in a recent frame (a few frames old; -1 until there is one), and whether
    // that share looks like a scene cut.
    float DistrustedShare() const { return _share; }
    bool SceneCutSeen() const { return _share >= _settings.cutShare; }

    struct Settings
    {
        float depthTolerance = 0.10f;      // relative depth difference that starts to count as a disocclusion
        float flowTolerance = 1.4f;        // pixels of flow disagreement allowed, plus half the flow's own length
        float lumaTolerance = 0.25f;       // relative luma excursion allowed
        float revealTolerance = 0.25f;     // how much farther the surface is now than the one at this pixel a frame ago
        float decay = 0.5f;                // how much of last frame's distrust is kept
        int debugView = 0;                 // 0 the mask; 1 depth, 2 revealed, 3 flow consistency, 4 luma, 5 outside, one check alone
        float cutShare = 0.98f;            // share of fully distrusted pixels that is called a scene cut (real cuts reach 0.99, fast camera turns 0.95)
    };

    Settings& Tuning() { return _settings; }
    const std::string& Error() const { return _error; }

  private:
    struct Tex
    {
        ID3D12Resource* resource = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    };

    struct Constants
    {
        uint32_t sizeX, sizeY, depthX, depthY;
        float depthTolerance, flowTolerance, lumaTolerance, decay;
        uint32_t reversed, hasHistory;
        float fullPerFlow, revealTolerance;
        uint32_t depthCount, debugView, depthBoth, pad;
    };

    bool CreateTexture(Tex& tex, uint32_t width, uint32_t height, DXGI_FORMAT format, const wchar_t* name);
    bool EnsureSize(ID3D12Device* device, uint32_t width, uint32_t height);
    void ReleaseTextures();
    void Transition(ID3D12GraphicsCommandList* list, Tex& tex, D3D12_RESOURCE_STATES state);
    void Pass(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, ID3D12Resource* const (&srv)[8],
              const DXGI_FORMAT (&formats)[8], Tex& dst, DXGI_FORMAT dstFormat, uint32_t groupsX, uint32_t groupsY,
              const Constants& constants);

    ID3D12Device* _device = nullptr;
    ID3D12RootSignature* _rootSignature = nullptr;
    ID3D12PipelineState* _depthProxy = nullptr;
    ID3D12PipelineState* _clearCounter = nullptr;
    ID3D12PipelineState* _trust = nullptr;
    ID3D12PipelineState* _copyFlow = nullptr;
    ID3D12PipelineState* _guideDepthPso = nullptr;
    ID3D12PipelineState* _guideMotionPso = nullptr;
    Tex _guideDepth;
    bool _guideDepthValid = false; // set by BuildGuides: whether this call had depth to write into _guideDepth
    Tex _guideMotion;
    ID3D12DescriptorHeap* _heap = nullptr;
    UINT _descriptorSize = 0;
    UINT _heapCursor = 0;

    Tex _depth[2];  // a depth proxy at the flow's size: larger is farther
    Tex _mask[2];
    Tex _flowBefore;
    int _depthIndex = 0;
    int _maskIndex = 0;

    ID3D12Resource* _counter = nullptr;          // GPU counter of fully distrusted pixels, UAV
    D3D12_RESOURCE_STATES _counterState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    static constexpr int kReadbacks = 4;
    ID3D12Resource* _readback[kReadbacks] = {};
    uint64_t _readbackFrame[kReadbacks] = {};    // the frame number each readback slot holds (0 empty)
    uint64_t _frame = 0;
    float _share = -1.0f;

    bool _haveHistory = false;
    bool _hadDepth = false; // whether the last Dispatch had depth
    uint32_t _width = 0;
    uint32_t _height = 0;
    Settings _settings;
    std::string _error;
};
