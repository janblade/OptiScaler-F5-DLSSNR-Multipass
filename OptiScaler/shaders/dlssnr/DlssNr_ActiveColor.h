#pragma once

#include <d3d12.h>
#include <optional>

namespace DlssNr
{
struct ColorExtent
{
    unsigned int width;
    unsigned int height;
    // Where the active rectangle starts in the allocation: a view of a multi-monitor layout that shares one texture
    // with its neighbours (DlssNr_Views.h). The guides carry their own bases (DlssNr_Guides.h).
    unsigned int x = 0;
    unsigned int y = 0;
};

// NGX reports the active image separately from the allocation. No preset names or standard
// resolutions belong here. A rectangle away from the origin needs its size: without one there is nothing to stage.
inline std::optional<ColorExtent> PreSrColorExtent(const D3D12_RESOURCE_DESC& allocation,
                                                  unsigned int renderWidth, unsigned int renderHeight,
                                                  unsigned int baseX = 0, unsigned int baseY = 0)
{
    if (allocation.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        allocation.SampleDesc.Count != 1 || allocation.DepthOrArraySize != 1 ||
        allocation.Width == 0 || allocation.Width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        allocation.Height == 0 || allocation.Height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        return std::nullopt;

    if (renderWidth == 0 && renderHeight == 0)
    {
        if (baseX != 0 || baseY != 0)
            return std::nullopt;

        return ColorExtent { (unsigned int) allocation.Width, allocation.Height };
    }

    // 64-bit sums: a base near the limit must not wrap round into range.
    if (renderWidth == 0 || renderHeight == 0 || (unsigned long long) baseX + renderWidth > allocation.Width ||
        (unsigned long long) baseY + renderHeight > allocation.Height)
        return std::nullopt;

    return ColorExtent { renderWidth, renderHeight, baseX, baseY };
}

// Both resources must be in COPY_SOURCE/COPY_DEST respectively. An explicit box is essential:
// a whole-resource copy either has mismatched dimensions or overwrites the game's padding.
inline void CopyActiveColor(ID3D12GraphicsCommandList* commands, ID3D12Resource* destination,
                            ID3D12Resource* source, ColorExtent active)
{
    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = source;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = destination;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box { 0, 0, 0, active.width, active.height, 1 };
    commands->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
}

// The game's active rectangle, wherever it starts, into a compact texture at the origin.
inline void CopyActiveColorIn(ID3D12GraphicsCommandList* commands, ID3D12Resource* compact, ID3D12Resource* game,
                              ColorExtent active)
{
    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = game;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = compact;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box { active.x, active.y, 0, active.x + active.width, active.y + active.height, 1 };
    commands->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
}

// And back: only that rectangle of the game's texture is written, never a neighbouring view or the padding.
inline void CopyActiveColorOut(ID3D12GraphicsCommandList* commands, ID3D12Resource* game, ID3D12Resource* compact,
                               ColorExtent active)
{
    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = compact;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = game;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box { 0, 0, 0, active.width, active.height, 1 };
    commands->CopyTextureRegion(&dst, active.x, active.y, 0, &src, &box);
}
} // namespace DlssNr
