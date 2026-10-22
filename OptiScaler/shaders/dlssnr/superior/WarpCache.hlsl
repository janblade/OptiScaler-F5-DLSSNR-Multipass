// Runs once per generation/layout; all entries use the consumer's exact FP32 evaluation.
cbuffer Params : register(b0)
{
    uint gMode; float gUnused; uint gWidth, gHeight;
};
Texture2D<float4> gMotion : register(t3);
RWTexture2D<float4> gTarget : register(u0);
#include "WarpMap.hlsli"
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.y == 0u && id.x < 11u)
        gTarget[id.xy] = gMotion.Load(int3(id.xy, 0));
    if (id.y == 1u && id.x < gWidth)
        gTarget[id.xy] = SuperiorArithmeticPackedUv(float2((float(id.x) + 0.5) / float(gWidth), 0)).xxxx;
    if (id.y == 2u && id.x < gHeight)
        gTarget[id.xy] = SuperiorArithmeticPackedUv(float2(0, (float(id.x) + 0.5) / float(gHeight))).yyyy;
}
