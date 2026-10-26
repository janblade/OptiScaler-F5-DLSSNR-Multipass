// Private DLAA only: identical uniform joint reconstruction and fixed carrier,
// with no materialized raw-motion texture between the two stages.
#include "JointGuides.hlsl"
Texture2D<float4> gPrivateAnswer : register(t3);
RWTexture2D<float4> gPrivateCarrier : register(u2);

void PrivateGuideAt(uint2 id, out float depth, out float2 raw)
{
    const float2 nativePixel=(float2(id)+0.5)*(gNativeWorkSize.xy/float2(gWidth,gHeight));
    float2 query,gradient;
    bool validDepth;
    depth=JointDepth(nativePixel,query,gradient,validDepth);
    const int2 lo=int2(gMotionRect.xy),hi=int2(gMotionRect.xy+gMotionRect.zw)-1;
    const int2 mp=clamp(int2(gMotionRect.xy+(nativePixel/gNativeWorkSize.xy)*gMotionRect.zw),lo,hi);
    raw=SuperiorSurfaceMotion(nativePixel,mp,query,validDepth?depth:asfloat(0x7fc00000u));
    depth=validDepth&&isfinite(depth)?depth:(gUnused>0.5?0.0:1.0);
    raw=all(isfinite(raw))?raw:0.0;
}
float3 PrivateFinite(float3 value,float3 fallback)
{
    return float3(isfinite(value.x)?value.x:fallback.x,
                  isfinite(value.y)?value.y:fallback.y,
                  isfinite(value.z)?value.z:fallback.z);
}
[numthreads(8,8,1)]
void PrivateGuidesMain(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=gWidth||id.y>=gHeight)return;
    float depth; float2 raw;
    PrivateGuideAt(id.xy,depth,raw);
    gTarget0[id.xy]=depth;
    // Preserve the carrier's point coordinate calculation too. Normally mp is
    // id; the fallback keeps the same result even for an unusual floating-point phase.
    const uint2 size=uint2(gWidth,gHeight);
    const float2 uv=(float2(id.xy)+0.5)/float2(size);
    const uint2 mp=min(uint2(uv*size),size-1);
    if(any(mp!=id.xy)) { float ignored; PrivateGuideAt(mp,ignored,raw); }
    const float2 mv=raw*float2(gPadding0.y,gPadding1.x);
    gTarget1[id.xy]=all(isfinite(mv))?mv:0.0;
    const float3 proxy=PrivateFinite(gSource0.Load(int3(id.xy,0)).rgb,0.0);
    const float3 model=PrivateFinite(gPrivateAnswer.Load(int3(id.xy,0)).rgb,proxy);
    precise float3 residual=model-proxy;
    precise float3 encoded=0.5+residual;
    gPrivateCarrier[id.xy]=float4(clamp(encoded,-1.0,2.0),1.0);
}
