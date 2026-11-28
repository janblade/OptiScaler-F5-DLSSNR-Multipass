#include "pch.h"
#include "SGSR1_Dx12.h"

#include "precompile/sgsr1_Shader.h"

using Microsoft::WRL::ComPtr;

struct alignas(256) Sgsr1Constants
{
    // x = 1/srcWidth, y = 1/srcHeight, z = srcWidth, w = srcHeight.
    float ViewportInfoX;
    float ViewportInfoY;
    float ViewportInfoZ;
    float ViewportInfoW;
    int32_t DstWidth;
    int32_t DstHeight;
    uint32_t ReversibleMode;
    uint32_t Passthrough;
    // Live-tunable versions of what used to be sgsr1.hlsl's own kEdgeThreshold/kEdgeSharpness
    // constants (8/255, 2.0) -- see SGSR1_Dx12.h's Dispatch() comment for why.
    float EdgeThreshold;
    float EdgeSharpness;
};

static Sgsr1Constants constants {};

bool SGSR1_Dx12::Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource, ID3D12Resource* OutResource,
                          uint32_t reversibleMode, uint32_t passthrough, float edgeThreshold, float edgeSharpness)
{
    if (!_init || _device == nullptr || InCmdList == nullptr || InResource == nullptr || OutResource == nullptr)
        return false;

    LOG_DEBUG("[{0}] Start!", _name);

    ScopedGpuTime_Dx12 scopedGpuTime(GpuTime.get(), InCmdList);

    _counter++;
    _counter = _counter % SGSR1_NUM_OF_HEAPS;
    FrameDescriptorHeap& currentHeap = _frameHeaps[_counter];

    CreateShaderResourceView(_device, InResource, currentHeap.GetSrvCPU(0));
    CreateUnorderedAccessView(_device, OutResource, currentHeap.GetUavCPU(0), 0);

    const auto srcDesc = InResource->GetDesc();
    const auto dstDesc = OutResource->GetDesc();
    const auto srcW = (uint32_t) srcDesc.Width;
    const auto srcH = (uint32_t) srcDesc.Height;
    const auto dstW = (uint32_t) dstDesc.Width;
    const auto dstH = (uint32_t) dstDesc.Height;

    constants.ViewportInfoX = 1.0f / (float) srcW;
    constants.ViewportInfoY = 1.0f / (float) srcH;
    constants.ViewportInfoZ = (float) srcW;
    constants.ViewportInfoW = (float) srcH;
    constants.DstWidth = (int32_t) dstW;
    constants.DstHeight = (int32_t) dstH;
    constants.ReversibleMode = reversibleMode;
    constants.Passthrough = passthrough;
    constants.EdgeThreshold = edgeThreshold;
    constants.EdgeSharpness = edgeSharpness;

    if (!CreateConstantsBuffer(_device, currentHeap, constants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_pipelineState);

    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    const UINT dispatchWidth = (dstW + InNumThreadsX - 1) / InNumThreadsX;
    const UINT dispatchHeight = (dstH + InNumThreadsY - 1) / InNumThreadsY;

    InCmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    return true;
}

SGSR1_Dx12::SGSR1_Dx12(std::string InName, ID3D12Device* InDevice) : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{0} start!", _name);

    CD3DX12_STATIC_SAMPLER_DESC sampler(0);
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    if (!SetupRootSignature(InDevice, 1, 1, 1, 0, 0, 1, &sampler))
    {
        LOG_ERROR("Failed to setup root signature");
        return;
    }

    // Precompiled only (matches FSR1's own EASU pass) -- no runtime-recompile fallback source,
    // since SGSR1 is not exposed as a user-selectable filter (nothing prompts a recompile
    // toggle to reach it).
    if (!Shader_Dx12::CreateComputePipeline(InDevice, &_pipelineState, sgsr1_Shader_cso, sizeof(sgsr1_Shader_cso),
                                            nullptr))
    {
        LOG_ERROR("[{0}] CreateComputePipeline error!", _name);
        return;
    }

    _init = InitHeaps(InDevice, _frameHeaps, SGSR1_NUM_OF_HEAPS);
}

SGSR1_Dx12::~SGSR1_Dx12()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    for (int i = 0; i < SGSR1_NUM_OF_HEAPS; i++)
    {
        _frameHeaps[i].ReleaseHeaps();
    }
}
