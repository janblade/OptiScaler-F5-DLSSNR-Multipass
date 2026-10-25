#pragma once
#include "interpass_fixture.h"
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Spatial.h"
#include <cmath>

inline ComPtr<ID3D12Resource> UploadWarp(Runner& r, const DlssNr::Spatial::Layout& layout)
{
    const auto constants = pw::MakeShaderConstants(layout.warp);
    D3D12_RESOURCE_DESC d {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = 11;
    d.Height = d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    auto texture = r.resource(d, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 bytes;
    r.device->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
    auto upload = r.buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* data;
    check(upload->Map(0, nullptr, &data));
    std::memcpy(static_cast<char*>(data) + fp.Offset, &constants, sizeof(constants));
    upload->Unmap(0, nullptr);
    r.begin();
    D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    r.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    r.barrier(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    r.finish();
    return texture;
}
inline float TestFloat(uint32_t bits)
{
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
inline float TestHalf(uint32_t bits)
{
    const float sign = bits & 0x8000u ? -1.f : 1.f;
    const unsigned exponent = (bits >> 10) & 31, fraction = bits & 1023;
    return sign * (exponent == 31 ? (fraction ? NAN : INFINITY)
                   : exponent     ? std::ldexp(1.f + fraction / 1024.f, int(exponent) - 15)
                                  : std::ldexp(float(fraction), -24));
}
