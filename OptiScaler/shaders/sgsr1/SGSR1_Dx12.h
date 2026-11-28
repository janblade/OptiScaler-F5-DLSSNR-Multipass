#pragma once
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

#include <d3d12.h>
#include <d3dx/d3dx12.h>

#define SGSR1_NUM_OF_HEAPS 3

// DLSS-NR's dedicated enlarge pass for the "model resolution < 100%" case: Qualcomm's SGSR1
// (single-pass, edge-directed 12-tap Lanczos-like upsample + adaptive sharpen), used instead
// of the implicit HW-bilinear tap the resolve pass would otherwise get when its answer texture
// is smaller than native. One fixed shader -- no filter choice, unlike OS_Dx12's Scaler switch.
class SGSR1_Dx12 : public Shader_Dx12
{
  private:
    FrameDescriptorHeap _frameHeaps[SGSR1_NUM_OF_HEAPS];

    uint32_t InNumThreadsX = 8;
    uint32_t InNumThreadsY = 8;

  public:
    // reversibleMode/passthrough mirror DlssNrConstants::ReversibleMode/Passthrough (dlssnr.hlsl's
    // gReversibleMode/gPassthrough) -- SGSR1 needs them to undo/redo the Neutwo/Hybrid domain
    // curve around its edge-directed math; see sgsr1.hlsl's file header for why.
    //
    // edgeThreshold/edgeSharpness were upstream's own fixed constants (8/255, 2.0) until an
    // in-game A/B found the vote firing on noisy high-frequency content (skin, hair) it wasn't
    // tuned for, smoothing detail bilinear preserved -- retuned and now fixed at 0.300/2.00,
    // passed in from the caller rather than baked into the shader.
    bool Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource, ID3D12Resource* OutResource,
                 uint32_t reversibleMode, uint32_t passthrough, float edgeThreshold, float edgeSharpness);

    SGSR1_Dx12(std::string InName, ID3D12Device* InDevice);

    ~SGSR1_Dx12();
};
