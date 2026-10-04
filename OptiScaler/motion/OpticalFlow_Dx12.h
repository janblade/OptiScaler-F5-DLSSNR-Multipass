#pragma once

// Dense optical flow on the GPU for the native input producer (DLSS-NR in a game that makes no upscaler call, so there are no
// motion vectors to take from one). Our own implementation of the standard method: a luma pyramid, block matching from the
// coarsest level down with a small search around the coarser level's answer, a few Lucas-Kanade gradient steps for the sub-pixel part, and a 3x3
// median over the result. At each level the candidates are the coarser level's answer at the nearest cells, the last
// frame's flow at the same place and no motion; the best of them is refined by a small search. The result is smoothed
// where the matching is not sure (flat or grainy areas) without crossing the edges of moving things: each neighbour counts
// by how much picture structure its match had, how close its motion is and how close its brightness is. Nothing is taken from any shader of another project.
//
// Self-contained: it needs only D3D12 and the HLSL compiler, so tests/nr_optical_flow_gpu.cpp drives it with synthetic images
// and known motion, with no game.
//
// Convention: for each pixel of the current frame the flow is the offset, in full-resolution pixels, to where the same
// content was in the previous frame (the usual game motion-vector direction, "from here to where it came from"). The flow
// texture is half the colour's resolution (R16G16B16A16_FLOAT, x and y in .xy, the match's confidence 0..1 in .z); a
// consumer samples it bilinearly.
//
// Dispatch() records onto a command list the caller owns and changes the descriptor heaps bound on it, so it belongs on a
// list of our own (the finished-picture seam), not on a game's list.

#include <d3d12.h>
#include <dxgiformat.h>
#include <cstdint>
#include <string>

class OpticalFlowDx12
{
  public:
    static constexpr int kLevels = 6; // pyramid levels (1/2 .. 1/64); the coarsest reaches about 250 pixels of motion

    OpticalFlowDx12() = default;
    ~OpticalFlowDx12();

    OpticalFlowDx12(const OpticalFlowDx12&) = delete;
    OpticalFlowDx12& operator=(const OpticalFlowDx12&) = delete;

    // Compiles the shaders and makes the pipelines. False with a reason in Error().
    bool Init(ID3D12Device* device);

    // Records the flow from the previous Dispatch's frame to this one. `color` must be in a shader-readable state
    // (NON_PIXEL_SHADER_RESOURCE) and stays so. After it returns, Flow() is readable and FlowValid() says whether it holds
    // a flow (false on the first frame, after Reset() and after a size change).
    bool Dispatch(ID3D12GraphicsCommandList* list, ID3D12Resource* color, DXGI_FORMAT colorFormat);

    // Forget the previous frame (a scene cut, a resolution change): the next Dispatch only stores its picture.
    void Reset() { _havePrevious = false; }

    // A picture of the flow for a menu: hue is the direction, brightness the speed up to maxSpeed pixels, black is still.
    // R8G8B8A8_UNORM at the flow's size, left in the PIXEL_SHADER_RESOURCE state. Call after Dispatch() when FlowValid().
    bool Visualise(ID3D12GraphicsCommandList* list, float maxSpeed);
    ID3D12Resource* Preview() const { return _preview.resource; }

    // The half-resolution luma (R32_FLOAT, NON_PIXEL_SHADER_RESOURCE) of the frame last passed to Dispatch() and of the one
    // before it; the trust mask compares them. The previous one is only meaningful when FlowValid().
    ID3D12Resource* LumaOfLastFrame() const { return _pyramid[1 - _current][0].resource; }
    ID3D12Resource* LumaOfFrameBefore() const { return _pyramid[_current][0].resource; }

    // The flow, in the NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE state between Dispatches.
    ID3D12Resource* Flow() const { return _flow.resource; }
    bool FlowValid() const { return _flowValid; }
    uint32_t FlowWidth() const { return _flow.width; }
    uint32_t FlowHeight() const { return _flow.height; }

    // Search radius, in pixels of each level, around the coarser level's answer (coarsest level searches kCoarseRadius).
    // Penalty added per pixel of distance from that answer, which keeps flat areas from picking noise.
    struct Settings
    {
        int radius = 1;
        int coarseRadius = 4;
        float lambda = 0.01f;
        bool useHistory = true;         // last frame's flow as a candidate
        int coarseCells = 4;            // how many of the coarser level's nearest cells are candidates (1..4)
        int smoothRadius = 2;           // the edge-aware smoothing of the result, in half-resolution pixels (0 = off)
        float confidenceKnee = 0.004f;  // how much picture structure counts as a trustworthy match
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
        uint32_t sizeX, sizeY, auxX, auxY;
        int32_t radius;
        uint32_t hasPrediction;
        float lambda;
        float scale;
        uint32_t hasHistory;
        float knee;
        uint32_t coarseCells, pad1;
    };

    bool CreateTexture(Tex& tex, uint32_t width, uint32_t height, DXGI_FORMAT format, const wchar_t* name);
    bool EnsureSize(uint32_t width, uint32_t height);
    void ReleaseTextures();
    void Transition(ID3D12GraphicsCommandList* list, Tex& tex, D3D12_RESOURCE_STATES state);
    void Pass(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso, ID3D12Resource* src0, DXGI_FORMAT format0,
              ID3D12Resource* src1, DXGI_FORMAT format1, ID3D12Resource* src2, DXGI_FORMAT format2, Tex& dst,
              DXGI_FORMAT dstFormat, const Constants& constants, ID3D12Resource* src3 = nullptr,
              DXGI_FORMAT format3 = DXGI_FORMAT_UNKNOWN);

    ID3D12Device* _device = nullptr;
    ID3D12RootSignature* _rootSignature = nullptr;
    ID3D12PipelineState* _luma = nullptr;
    ID3D12PipelineState* _down = nullptr;
    ID3D12PipelineState* _match = nullptr;
    ID3D12PipelineState* _median = nullptr;
    ID3D12PipelineState* _smooth = nullptr;
    ID3D12PipelineState* _visualise = nullptr;
    ID3D12DescriptorHeap* _heap = nullptr;
    UINT _descriptorSize = 0;
    UINT _heapCursor = 0;

    Tex _pyramid[2][kLevels]; // luma, 1/2 .. 1/64 of the colour; one set is the current frame, the other the previous
    Tex _levelFlow[2][kLevels]; // this frame's flow at each level, and the last frame's (a candidate for this one)
    Tex _flowMedian; // the median's result, which the smoothing reads
    Tex _flow;
    Tex _preview;
    int _current = 0;
    bool _havePrevious = false;
    bool _flowValid = false;
    uint32_t _width = 0;
    uint32_t _height = 0;

    Settings _settings;
    std::string _error;
};
