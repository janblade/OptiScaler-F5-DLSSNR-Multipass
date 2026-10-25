// Included by DlssNr_Dx12.cpp. A private root/heap keeps the extra carrier UAV
// out of every ordinary NR shader's descriptor table and root signature.
namespace DlssNr::Superior::PrivateBytecode
{
#include "precompile/host_private_guides_Shader.h"
}

bool DlssNr_Dx12::PrivateGuidesReady()
{
    if (_privateGuidesAttempted)
        return _privateGuidesRoot && _privateGuidesPipeline && _privateGuidesHeap;
    _privateGuidesAttempted = true;
    D3D12_DESCRIPTOR_RANGE ranges[2] {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = kSrvCount;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 3;
    ranges[1].OffsetInDescriptorsFromTableStart = kSrvCount;
    D3D12_ROOT_PARAMETER parameters[2] {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = { 2, ranges };
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC description { 2, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    Microsoft::WRL::ComPtr<ID3DBlob> bytes, errors;
    if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &bytes, &errors)) ||
        FAILED(_device->CreateRootSignature(0, bytes->GetBufferPointer(), bytes->GetBufferSize(),
                                            IID_PPV_ARGS(&_privateGuidesRoot))))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline {};
    pipeline.pRootSignature = _privateGuidesRoot;
    pipeline.CS = { DlssNr::Superior::PrivateBytecode::host_private_guides_cso,
                    sizeof(DlssNr::Superior::PrivateBytecode::host_private_guides_cso) };
    if (FAILED(_device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&_privateGuidesPipeline))))
        return false;
    D3D12_DESCRIPTOR_HEAP_DESC heap {};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    // Six SRVs, three UAVs and one CBV per existing shared-ring slot. The CBV
    // descriptor is populated by CreateConstantsBuffer; b0 is bound directly.
    heap.NumDescriptors = DLSSNR_NUM_OF_HEAPS * 10;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    return SUCCEEDED(_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&_privateGuidesHeap)));
}

bool DlssNr_Dx12::DispatchSuperiorPrivateGuides(ID3D12GraphicsCommandList* cmd,
                                                const DlssNr::Spatial::Constants& constants, ID3D12Resource* proxy,
                                                ID3D12Resource* answer, ID3D12Resource* depth, ID3D12Resource* motion,
                                                ID3D12Resource* outDepth, ID3D12Resource* outMotion,
                                                ID3D12Resource* outCarrier)
{
    // A minifying private guide pass still needs the separate sparse correction.
    if (!_init || !cmd || !proxy || !answer || !depth || !motion || !outDepth || !outMotion || !outCarrier ||
        constants.mode != 104u || !constants.width || !constants.height || constants.padding[0] <= .5f ||
        DlssNr::Spatial::NeedsJointMinification(constants) || !PrivateGuidesReady())
        return false;
    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_NUM_OF_HEAPS;
    const UINT stride = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = _privateGuidesHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(slot) * 10 * stride;
    ID3D12Resource* inputs[] { proxy, depth, motion, answer, proxy, proxy };
    for (auto* input : inputs)
    {
        CreateShaderResourceView(_device, input, cpu);
        cpu.ptr += stride;
    }
    for (auto* output : { outDepth, outMotion, outCarrier })
    {
        CreateUnorderedAccessView(_device, output, cpu, 0);
        cpu.ptr += stride;
    }
    if (!CreateConstantsBuffer(_device, _constantBuffers[slot], constants, cpu))
        return false;
    ID3D12DescriptorHeap* heaps[] { _privateGuidesHeap };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(_privateGuidesRoot);
    cmd->SetPipelineState(_privateGuidesPipeline);
    auto gpu = _privateGuidesHeap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(slot) * 10 * stride;
    cmd->SetComputeRootDescriptorTable(0, gpu);
    cmd->SetComputeRootConstantBufferView(1, _constantBuffers[slot]->GetGPUVirtualAddress());
    cmd->Dispatch((constants.width + 7) / 8, (constants.height + 7) / 8, 1);
    return true;
}
