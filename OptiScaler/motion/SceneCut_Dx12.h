#pragma once

// The scene-cut detector on the GPU: is this frame a hard cut from the last one? Brightness histograms of a luma in a
// 3x3 grid of tiles are compared with the last frame's (symmetric KL divergence, the smallest over a range of sideways
// shifts, so a brightness step of the whole picture -- a fade, eye adaptation -- is not a cut); past a threshold the
// frame is one. Decided on the GPU, so work recorded after it on the same list can act on it in that very frame.
//
// Two callers: the optical flow (motion/OpticalFlow_Dx12.cpp) hands it the luma it already made; DLSS-NR's game input
// (shaders/dlssnr/DlssNr_SceneCut.inl) hands it the colour, and it makes a small luma of its own first.
//
// Self-contained: it needs only D3D12 and the precompiled shaders (native/F5LowShaderBytecode.h), so
// tests/nr_scene_cut_gpu.cpp drives it with synthetic images, with no game.
//
// Its passes set their own root signature and descriptor heap on the list: record them where that is allowed (a list of
// our own, or inside an upscaler's evaluate, after which the game sets its state again).

#include <d3d12.h>
#include <dxgiformat.h>
#include <cstdint>
#include <string>

class SceneCutDx12
{
  public:
    // What the colour's values mean, for DetectColor's luma: gamma-encoded SDR, linear scRGB (1.0 = 80 nits) or PQ.
    enum class Encoding
    {
        Srgb,
        ScRgb,
        Pq
    };

    // DetectColor's luma is at most this wide: the colour shrunk by a whole number each way.
    static constexpr uint32_t kColorLumaWidth = 640;

    SceneCutDx12() = default;
    ~SceneCutDx12();

    SceneCutDx12(const SceneCutDx12&) = delete;
    SceneCutDx12& operator=(const SceneCutDx12&) = delete;

    // Makes the pipelines and the textures, which do not depend on any picture's size. False with a reason in Error().
    bool Init(ID3D12Device* device);

    // Records the detector for `luma` (R32_FLOAT, at least 3x3, NON_PIXEL_SHADER_RESOURCE, which it stays in). After it
    // Flag() and Distrust() hold this frame's answer. The first frame, and the first after Reset(), has nothing to compare
    // with and is never a cut. `threshold`: the divergence past which the frame is a cut (0 alike .. 1 nothing alike).
    bool Detect(ID3D12GraphicsCommandList* list, ID3D12Resource* luma, float threshold);

    // The same for a colour (any size, readable as colorFormat, NON_PIXEL_SHADER_RESOURCE, which it stays in): its luma
    // first (see kColorLumaWidth), perceptual for each encoding the way the flow makes its own (motion/Luma_Hlsl.h).
    bool DetectColor(ID3D12GraphicsCommandList* list, ID3D12Resource* color, DXGI_FORMAT colorFormat, Encoding encoding,
                     float whiteNits, float threshold);

    // Forget the last frame (a reset the caller knows of, a skipped frame, a size change): the next Detect only stores.
    void Reset() { _prevValid = false; }

    // The answer of the last Detect, both in the NON_PIXEL_SHADER_RESOURCE state between Detects:
    // - Flag(): 2x1 R32_UINT, texel 0 is 1 on a cut, texel 1 the float bits of the divergence it was judged by.
    // - Distrust(): 1x1 R8_UNORM, 1.0 on a cut, else 0: a history-distrust input (DlssNrFrameInfo::HistoryDistrust),
    //   which is read by uv and so may be any size.
    ID3D12Resource* Flag() const { return _flag.resource; }
    ID3D12Resource* Distrust() const { return _distrust.resource; }

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
        uint32_t sizeX, sizeY;
        uint32_t hasPrevious; // the last frame's histograms are there to compare with
        float threshold;
        uint32_t colorX, colorY; // SceneLuma: the colour's size
        uint32_t step;           // SceneLuma: colour pixels per luma pixel, each way
        float whiteNits;         // SceneLuma: an HDR picture's white, in nits
    };

    bool CreateTexture(Tex& tex, uint32_t width, uint32_t height, DXGI_FORMAT format, const wchar_t* name);
    void Transition(ID3D12GraphicsCommandList* list, Tex& tex, D3D12_RESOURCE_STATES state);
    void Pass(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, ID3D12Resource* source, DXGI_FORMAT format,
              Tex* target, uint32_t groupsX, uint32_t groupsY, const Constants& constants);
    void Release();

    ID3D12Device* _device = nullptr;
    ID3D12RootSignature* _root = nullptr;
    ID3D12PipelineState* _hist = nullptr;
    ID3D12PipelineState* _diverge = nullptr;
    ID3D12PipelineState* _lumaPso[3] = {}; // [Encoding]
    ID3D12DescriptorHeap* _heap = nullptr;
    UINT _descriptorSize = 0;
    UINT _heapCursor = 0;

    Tex _state;    // R32_UINT, 256 wide: the nine tiles' counts, the last frame's nine smoothed histograms, scratch
    Tex _flag;     // R32_UINT, 2x1
    Tex _distrust; // R8_UNORM, 1x1
    Tex _luma;     // R32_FLOAT: DetectColor's luma, made at the first colour's size and again when that changes

    struct Retired
    {
        ID3D12Resource* resource = nullptr;
        uint32_t frames = 0; // DetectColor calls left before it is released
    };
    Retired _retired[4]; // luma textures a size change replaced

    bool _prevValid = false; // _state holds the last frame's histograms
    std::string _error;
};
