#pragma once

// The composition pass for Neural Rendering.
//
// Neural Rendering is two things, and only one of them is a shader. The model is an NGX feature --
// created and evaluated, not dispatched -- and that stays where it is. This is the other half: the
// pass that builds the tone-mapped proxy the model is shown, and then transfers the model's answer
// back onto the real frame.
//
// It is an ordinary compute shader with a constant struct, so it belongs here alongside RCAS and
// Output Scaling rather than owning a bespoke root signature and descriptor ring of its own.
//
// One shader, three modes, because all three read and write the same set of resources and differ
// only in what they compute:
//
//   Encode   the frame -> a tone-mapped proxy, plus an untouched copy to transfer against later
//   Down     the proxy -> a smaller proxy, when the model is asked to work below full resolution
//   Resolve  proxy + model answer + untouched copy -> the frame, edited

#include "DlssNr_Common.h"

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

// Three dispatches are recorded per frame and several frames can be in flight at once, more so with
// frame generation. Each dispatch needs descriptors and constants the GPU is not still reading, so
// there has to be enough for three passes times the deepest pipeline we might sit behind.
// Descriptor and constant slots, consumed one per dispatch and reused round-robin with no fence.
//
// A frame records the meter, its reduce, eye adaptation, the exposure courier, encode, downsample, one clamp per
// extra model pass, resolve and, during a Tune run, the stats pass: about ten with the default passes. The model
// layers themselves are NGX evaluates and do not consume this ring; their A/B resources and feature histories are
// persistent. Forty-eight slots leave four or more such frames before descriptor/constant reuse; many more passes
// leave fewer.
#define DLSSNR_NUM_OF_HEAPS 48

class DlssNr_Dx12 : public Shader_Dx12, public DlssNr_Common
{
  private:
    FrameDescriptorHeap _frameHeaps[DLSSNR_NUM_OF_HEAPS];

    // One constant buffer per heap, not one for the class.
    //
    // The shared buffer in the base class suits a shader that dispatches once a frame. Three
    // dispatches recorded onto one command list all map and overwrite the same upload buffer before
    // any of them executes, so every pass ends up reading whichever constants were written last --
    // encode and downsample would run with the resolve's parameters.
    ID3D12Resource* _constantBuffers[DLSSNR_NUM_OF_HEAPS] = {};

    uint32_t _heapIndex = 0;

    // The shader reads five inputs and writes two, and not every mode uses all of them. Unused slots
    // still need a view bound -- an unbound descriptor is not an empty read, it is a read from
    // nothing -- so a stand-in is written into whichever are spare.
    static constexpr uint32_t kSrvCount = 5;
    static constexpr uint32_t kUavCount = 2;

    uint32_t _numThreadsX = 8;
    uint32_t _numThreadsY = 8;

    // Finished Picture's second compute PSO, built from dlssnr_finished_color.hlsl's own blob,
    // reusing this class's root signature and descriptor table. Kept separate so the main
    // dlssnr.hlsl blob is never regenerated (a current dxc produces materially different DXIL
    // from the committed one). Null on backends/builds where the finished-colour shader is absent.
    ID3D12PipelineState* _finishedColorPipelineState = nullptr;

    // "Tune for this scene"'s stats pass (dlssnr_detail_stats.hlsl), built on first use the same way. Only
    // dispatched while a calibration runs.
    ID3D12PipelineState* _detailStatsPipelineState = nullptr;

    // Automatic exposure's eye adaptation pass (dlssnr_exposure_adapt.hlsl), built on first use the same way. Not
    // retried once it failed to build: the meter then writes the exposure directly, as before.
    ID3D12PipelineState* _exposureAdaptPipelineState = nullptr;
    bool _exposureAdaptPipelineFailed = false;

  public:
    DlssNr_Dx12(std::string InName, ID3D12Device* InDevice);
    ~DlssNr_Dx12();

    // The pass. Resources in, and nothing read from anywhere the caller cannot see.
    //
    // This is the whole filter: it brings the model up if it is not already, builds the feature and
    // rebuilds it when the tuning or the resolution changes, evaluates it, and runs the compute passes
    // that show it the frame and bring its answer back. One call, like any other shader here.
    //
    // Sizes come from the resources. Everything the pass cannot work out for itself is in
    // DlssNrFrameInfo; everything the user chose stays in Config. colour and output may be the same
    // resource. timingQueue is the queue this list will be executed on, when the caller knows it.
    void Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour, ID3D12Resource* depth,
                  ID3D12Resource* motion, ID3D12Resource* output, const DlssNrFrameInfo& frame,
                  ID3D12CommandQueue* timingQueue = nullptr);

    // Records one pass. Resources that a given mode does not read may be null; a stand-in is bound in
    // their place so every descriptor in the table is valid.
    // One compute pass. The public entry below drives three of these plus the model.
    bool DispatchPass(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                  ID3D12Resource* InSource, ID3D12Resource* InModel, ID3D12Resource* InOriginal,
                  ID3D12Resource* InMotion,
                  // Vestigial. Fed to the slot the removed edit accumulator read its history from;
                  // nothing reads it now and every caller passes nullptr. Kept only so the binding
                  // table keeps its shape -- not evidence that temporal accumulation exists.
                  ID3D12Resource* InPrevEdit, ID3D12Resource* OutTarget,
                  ID3D12Resource* OutKeep);

    // One compute pass of Finished Picture's shader (dlssnr_finished_color.hlsl). Same descriptor
    // table shape as DispatchPass; binds _finishedColorPipelineState instead of _pipelineState.
    // t4/u1 are bound with a stand-in for parity. Returns false (no-op) if that PSO is absent.
    bool DispatchResidualPass(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                              ID3D12Resource* InSource, ID3D12Resource* InModel,
                              ID3D12Resource* InOriginal, ID3D12Resource* InMotion,
                              ID3D12Resource* OutTarget);

    // One stats pass of "Tune for this scene" (dlssnr_detail_stats.hlsl): 64x64 thread groups, one per tile, into
    // the 128x64 RGBA32F grid. Same descriptor table shape as DispatchPass. False (no-op) if the PSO cannot be built.
    bool DispatchDetailStats(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                             ID3D12Resource* InOutput, ID3D12Resource* InPrevOutput, ID3D12Resource* InInput,
                             ID3D12Resource* InPrevInput, ID3D12Resource* InProxy, ID3D12Resource* OutGrid);

    // Automatic exposure's eye adaptation (dlssnr_exposure_adapt.hlsl, DlssNr_ExposureAdapt.h): one thread eases the
    // 1x1 OutEased toward the 1x1 InReading. ExposureAdaptReady builds the PSO on first use and says whether there is
    // one; DispatchExposureAdapt is false (no-op) without it. Same descriptor table shape as DispatchPass.
    bool ExposureAdaptReady();
    bool DispatchExposureAdapt(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                               ID3D12Resource* InReading, ID3D12Resource* OutEased);
};
