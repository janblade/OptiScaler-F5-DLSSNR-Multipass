#pragma once
#include <d3d12.h>
#include <climits>
#include <wrl/client.h>
#include "precompile/dlssnr_finished_pq_vs_Shader.h"
#include "precompile/dlssnr_finished_pq_ps_Shader.h"

namespace DlssNr
{
inline bool CanRasterFinishedPq(D3D12_COMMAND_LIST_TYPE type, const D3D12_RESOURCE_DESC& source,
                                const D3D12_RESOURCE_DESC& picture)
{
    const auto plain = [](const D3D12_RESOURCE_DESC& d)
    {
        return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.MipLevels == 1 && d.DepthOrArraySize == 1 &&
               d.SampleDesc.Count == 1 && d.Width && d.Height;
    };
    return type == D3D12_COMMAND_LIST_TYPE_DIRECT && plain(source) && plain(picture) && source.Width == picture.Width &&
           source.Height == picture.Height && source.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
           (source.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0 &&
           (source.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0 &&
           picture.Format == DXGI_FORMAT_R10G10B10A2_UNORM &&
           (picture.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 &&
           (picture.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0;
}

// Owned by the compositor. Descriptor slots share its existing in-flight ring.
// Only call on its private DIRECT list, never on an application's shared list.
class FinishedPqRaster
{
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> targets;
    unsigned count;
    UINT rtvStride = 0;
    bool attempted = false;

    bool Ready(ID3D12Device* device)
    {
        if (attempted)
            return pipeline && targets;
        attempted = true;
        D3D12_DESCRIPTOR_RANGE range {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER parameter {};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameter.DescriptorTable = { 1, &range };
        D3D12_ROOT_SIGNATURE_DESC signature {};
        signature.NumParameters = 1;
        signature.pParameters = &parameter;
        Microsoft::WRL::ComPtr<ID3DBlob> blob;
        if (FAILED(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)) ||
            FAILED(
                device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root))))
            return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d {};
        d.pRootSignature = root.Get();
        d.VS = { dlssnr_finished_pq_vs_cso, sizeof(dlssnr_finished_pq_vs_cso) };
        d.PS = { dlssnr_finished_pq_ps_cso, sizeof(dlssnr_finished_pq_ps_cso) };
        auto& blend = d.BlendState.RenderTarget[0];
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        d.DepthStencilState.FrontFace.StencilFailOp = d.DepthStencilState.FrontFace.StencilDepthFailOp =
            d.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
        d.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        d.DepthStencilState.BackFace = d.DepthStencilState.FrontFace;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
        d.SampleDesc.Count = 1;
        if (FAILED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pipeline))))
            return false;
        D3D12_DESCRIPTOR_HEAP_DESC heap {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = count;
        if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&targets))))
            return false;
        rtvStride = device->GetDescriptorHandleIncrementSize(heap.Type);
        return true;
    }

  public:
    explicit FinishedPqRaster(unsigned slots) : count(slots) {}
    FinishedPqRaster(const FinishedPqRaster&) = delete;
    FinishedPqRaster& operator=(const FinishedPqRaster&) = delete;

    // False records no commands/transitions, so the existing compute/copy fallback is intact.
    bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, ID3D12Resource* linear, ID3D12Resource* picture,
                D3D12_RESOURCE_STATES pictureState, ID3D12DescriptorHeap* inputHeap, D3D12_CPU_DESCRIPTOR_HANDLE srvCpu,
                D3D12_GPU_DESCRIPTOR_HANDLE srvGpu, unsigned slot)
    {
        if (!device || !cmd || !linear || !picture || !inputHeap || slot >= count)
            return false;
        const auto src = linear->GetDesc(), dst = picture->GetDesc();
        if (!CanRasterFinishedPq(cmd->GetType(), src, dst) || !Ready(device))
            return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = src.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(linear, &srv, srvCpu);
        D3D12_RENDER_TARGET_VIEW_DESC rtv {};
        rtv.Format = dst.Format;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        const D3D12_CPU_DESCRIPTOR_HANDLE target { targets->GetCPUDescriptorHandleForHeapStart().ptr +
                                                   SIZE_T(slot) * rtvStride };
        device->CreateRenderTargetView(picture, &rtv, target);
        const auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            if (before == after)
                return;
            D3D12_RESOURCE_BARRIER barrier {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
            cmd->ResourceBarrier(1, &barrier);
        };
        transition(linear, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(picture, pictureState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12DescriptorHeap* heaps[] = { inputHeap };
        cmd->SetDescriptorHeaps(1, heaps);
        cmd->SetGraphicsRootSignature(root.Get());
        cmd->SetPipelineState(pipeline.Get());
        cmd->SetGraphicsRootDescriptorTable(0, srvGpu);
        const D3D12_VIEWPORT viewport { 0, 0, float(dst.Width), float(dst.Height), 0, 1 };
        const D3D12_RECT scissor { 0, 0, LONG(dst.Width), LONG(dst.Height) };
        cmd->RSSetViewports(1, &viewport);
        cmd->RSSetScissorRects(1, &scissor);
        cmd->OMSetRenderTargets(1, &target, FALSE, nullptr);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        cmd->DrawInstanced(3, 1, 0, 0);
        // Do not leave a render-target binding alive across the transition or the next slot.
        cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        transition(picture, D3D12_RESOURCE_STATE_RENDER_TARGET, pictureState);
        transition(linear, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        return true;
    }
};
} // namespace DlssNr
