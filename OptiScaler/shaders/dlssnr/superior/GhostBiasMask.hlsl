// The quarter-work disocclusion field, bilinearly enlarged to the working raster, as the private DLSS's
// bias-current-colour mask (1 = take the current frame, 0 = history as usual).
cbuffer Params : register(b0)
{
    uint mode;
    float strength;
    uint width, height;
};
Texture2D<float> Field : register(t0);
SamplerState LinearClamp : register(s0);
#include "VkFormat.hlsli"
VK_FORMAT("r32f") RWTexture2D<float> Mask : register(u0);
[numthreads(8, 8, 1)] void GhostBiasMask(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    uint qw, qh;
    Field.GetDimensions(qw, qh);
    if (qw == 0 || qh == 0)
    {
        Mask[id.xy] = 0;
        return;
    }
    // Exact alignment with CompactGhost's 4x4 cells even when width/height are not multiples of 4.
    const float2 uv = (float2(id.xy) + 0.5) / (4.0 * float2(qw, qh));
    const float mask = Field.SampleLevel(LinearClamp, uv, 0) * strength;
    Mask[id.xy] = isfinite(mask) ? saturate(mask) : 0;
}
