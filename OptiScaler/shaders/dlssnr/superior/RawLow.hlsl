// Reuse v59's constant ABI and 8x16 / 36x68 cooperative low cache. The native
// compressed path has an ordinary NR answer, so cache N-P instead of decoded E.
#define DLSSNR_SUPERIOR_SCALING 1
#define DLSSNR_SUPERIOR_LOW 1
#include "precompile/dlssnr.hlsl"

[numthreads(8,16,1)]
void RawLowMain(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID)
{
    uint w,h,mw,mh;
    gSource.GetDimensions(w,h);
    gModel.GetDimensions(mw,mh);
    const bool valid = w > 0u && h > 0u && w == mw && h == mh;
    if (valid)
    {
        const float2 size = float2(w,h), footprint = size / float2(gWidth,gHeight);
        const float2 center = (float2(group.xy * uint2(8u,16u)) + .5) * footprint;
        const float2 step = .25 * footprint;
        const uint lane = local.y * 8u + local.x;
        [loop] for (uint i=lane;i<36u*68u;i+=128u)
        {
            const float2 uv = clamp(center + (float2(i%36u,i/36u)-3.5)*step,.5,size-.5)/size;
            gLow64Tile[i] = SanitizeFinite3(gModel.SampleLevel(gLinear,uv,0).rgb -
                                           gSource.SampleLevel(gLinear,uv,0).rgb,0.0);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (id.x>=gWidth || id.y>=gHeight) return;
    if (!valid) {gTarget[id.xy]=float4(0,0,0,1);return;}
    static const float weights[8]={7,11,15,17,17,15,11,7};
    float3 sum=0;
    [unroll] for (uint y=0;y<8;++y)
    [unroll] for (uint x=0;x<8;++x)
    {
        const uint2 p=local.xy*4u+uint2(x,y);
        sum += SanitizeFinite3(gLow64Tile[p.y*36u+p.x],0.0)*(weights[x]*weights[y]);
    }
    gTarget[id.xy]=float4(sum*(1.0/10000.0),1.0);
}
