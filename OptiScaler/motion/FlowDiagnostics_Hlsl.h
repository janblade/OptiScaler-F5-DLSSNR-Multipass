#pragma once
#include "MotionArrows_Hlsl.h"
#include "FlowDebugLegend_Hlsl.h"

// Display-only code. No estimator pass reads these resources or changes its field.
#define F5LOW_DIAGNOSTICS_HLSL F5LOW_MOTION_ARROWS_HLSL F5LOW_DEBUG_LEGEND_HLSL R"HLSL(
Texture2D<float4> DebugColor : register(t1);
Texture2D<float4> DebugCurrentLuma : register(t2);
Texture2D<float4> DebugSceneDepth : register(t3);
Texture2D<float> DebugPreviousLuma : register(t4);
Texture2D<float4> DebugGuide : register(t5);
Texture2D<float4> DebugMatch : register(t6);
Texture2D<uint> DebugAge : register(t7);


float DebugHistoryRejection(float2 uv){return DebugGuide.SampleLevel(Linear,uv,0).x;}

// 0 is weak and 1 is strong. Preserve the original structure diagnostic; a
// validated camera prior can separately support a texture-poor correspondence.
float DebugMotionSupport(float2 uv,float2 motion,float structure) {
    if(hasGlobal!=2u)return saturate(structure);
    float4 x=DebugMatch.Load(int3(0,0,0)),y=DebugMatch.Load(int3(1,0,0));
    if(x.w<=0.5)return saturate(structure);
    float3 p=float3(1.0,2.0*uv-1.0);
    float2 difference=motion-float2(dot(x.xyz,p),dot(y.xyz,p));
    float geometric=saturate(y.w)/(1.0+4.0*dot(difference,difference));
    return max(saturate(structure),geometric);
}

float3 DebugVectorColour(float2 backward)
{
    float magnitude = length(backward);
    return magnitude > 0.0 ? ArrowDirectionColor(-backward / magnitude, magnitude, scale) : 0.0;
}

float DebugPhotoError(float2 uv, float2 motion)
{
    float current[9], previous[9]; float meanC=0.0, meanP=0.0;
    [unroll] for(int k=0;k<9;++k)
    {
        float2 offset=float2(k%3-1,k/3-1)/float2(size);
        current[k]=DebugCurrentLuma.SampleLevel(Linear,uv+offset,0).x;
        previous[k]=DebugPreviousLuma.SampleLevel(Linear,uv+motion/float2(aux)+offset,0);
        meanC+=current[k];meanP+=previous[k];
    }
    meanC/=9.0;meanP/=9.0;float error=0.0,contrast=0.0;
    [unroll] for(int j=0;j<9;++j)
    {error+=abs((current[j]-meanC)-(previous[j]-meanP));contrast+=abs(current[j]-meanC);}
    return saturate(error/(contrast+9.0/255.0));
}

// DisplayDepth reference: crosire/reshade-shaders, Shaders/DisplayDepth.fx
// and ReShade.fxh (CC0). Display-only sampling must use the original depth,
// with its UV calibration, rather than the estimator's point-resampled copy.
float DebugDepthRaw(float2 uv) {
    float2 t=uv/float2(zeroMargin,lookRange)+float2(lookDistance,whiteNits);
    return DebugSceneDepth.SampleLevel(Linear,t,0).x;
}
float DebugDepthAt(float2 uv,bool linearize) {
    float d=DebugDepthRaw(uv)*photoTolerance;
    if(linearize){d=reversed!=0u?1.0-d:d;d/=evidenceNoise-d*(evidenceNoise-1.0);}
    return d;
}
float3 DebugDepthNormal(float2 uv) {
    float2 pixel=1.0/float2(aux);
    float2 north=uv-float2(0.0,pixel.y),east=uv+float2(pixel.x,0.0);
    float3 centerPosition=float3(uv-0.5,1.0)*DebugDepthAt(uv,true);
    float3 northPosition=float3(north-0.5,1.0)*DebugDepthAt(north,true);
    float3 eastPosition=float3(east-0.5,1.0)*DebugDepthAt(east,true);
    float3 n=cross(centerPosition-northPosition,centerPosition-eastPosition);
    // DisplayDepth writes UNORM, where a degenerate NaN becomes black. Keep
    // that result explicit before the FX intermediate, which may be floating point.
    return dot(n,n)>0.0?normalize(n)*0.5+0.5:float3(0.0,0.0,0.0);
}
float3 DebugDepthDither(float2 uv,float d) {
    float grid=frac(dot(uv,float2(aux)*float2(1.0/16.0,10.0/36.0)+0.25));
    const float shift=0.25/255.0;
    float3 rgb=float3(shift,-shift,shift);
    return d.xxx+lerp(2.0*rgb,-2.0*rgb,grid);
}

[numthreads(8,8,1)]
void Visualise(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=size.x || id.y>=size.y)return;
    float2 uv=(float2(id.xy)+0.5)/float2(size);
    float4 flow=radius==6?float4(0,0,0,0):FlowIn.Load(int3(id.xy,0));
    float4 output=float4(DebugVectorColour(radius==2?flow.xy*2.0:flow.xy),1.0);
    if(radius==1)output=float4(saturate(flow.z).xxx,1.0);
    if(radius==4)output=float4(DebugPhotoError(uv,flow.xy).xxx,1.0);
    // Three distinct signals, not an uncalibrated sum: R is the estimator's
    // texture-structure weight, G is local post-filter photometric support,
    // B is the actual trust of previous history. A bright channel is strong.
    if(radius==5) {
        float textureSupport=saturate(flow.z);
        float photometricSupport=1.0-DebugPhotoError(uv,flow.xy);
        float historyConfidence=hasHistory!=0 ? 1.0-DebugGuide.SampleLevel(Linear,uv,0).x : 0.0;
        output=float4(textureSupport,photometricSupport,saturate(historyConfidence),1.0);
    }
    if(radius==0 && hasHistory!=0)output=float4(DebugVectorColour(DebugGuide.SampleLevel(Linear,uv,0).xy),1.0);
    if(radius==10)
    {
        float2 before=DebugMatch.Load(int3(id.xy,0)).xy*2.0;
        output=float4(DebugVectorColour(flow.xy-before),1.0);
    }
    if(radius==17 || radius==18) {
        float rejected=hasHistory!=0 ? saturate(DebugHistoryRejection(uv)) : 1.0;
        if(radius==17) output=float4(rejected.xxx,1.0);
        else {
            // Conservative diagnostic only. No new vector/history rejection is
            // inserted into the estimator or the consumer by selecting this view.
            float support=min(max(saturate(flow.w),DebugMotionSupport(uv,flow.xy,flow.z)),min(1.0-DebugPhotoError(uv,flow.xy),1.0-rejected));
            float3 colour=support<0.5
                ?lerp(float3(1.0,0.25,0.02),float3(1.0,0.90,0.02),support*2.0)
                :lerp(float3(1.0,0.90,0.02),float3(0.05,0.95,0.15),(support-0.5)*2.0);
            if(hasPrediction!=0)colour=lerp(DebugColor.SampleLevel(Linear,uv,0).rgb,colour,0.90);
            output=float4(colour,1.0);
        }
    }
    if(radius==12) {
        float age=hasAge!=0 ? float(DebugAge.Load(int3(id.xy,0))) : 0.0;
        output=float4(saturate(age/max(float(stillFrames),1.0)).xxx,1.0);
    }
    if(radius>=11 && radius<=16 && radius!=12)
    {
        output=float4(1.0,0.0,1.0,1.0);
        if(hasAge!=0) {
            const uint divisor=2u<<coarseCells;
            const int2 dims=int2((aux+divisor-1u)/divisor);
            const int2 at=min(int2(uv*float2(dims)),dims-1);
            float4 candidate=DebugColor.Load(int3(at,0));
            float4 refine=DebugCurrentLuma.Load(int3(at,0));
            float4 events=DebugSceneDepth.Load(int3(at,0));
            if(radius==11) output=float4(DebugVectorColour(refine.zw*float(2u<<coarseCells)),1.0);
            if(radius==12) output=float4(saturate(events.x/8.0).xxx,1.0);
            if(radius==13) {uint origin=uint(candidate.x)%32u;output=float4(origin==0u?float3(0,0,0):origin==1u?float3(0,0.8,1):origin==11u?float3(1,0.7,0):float3(0.4,1,0.3),1);}
            if(radius==14) output=float4(saturate(candidate.w/max(candidate.z+0.001,0.001)*0.5).xxx,1.0);
            if(radius==15) output=float4(float((uint(events.w)&1u)!=0u),saturate(events.z/3.0),float((uint(events.w)&2u)!=0u),1.0);
            if(radius==16) output=float4(saturate(events.y).xxx,1.0);
        }
    }
    if(radius==6) {
        if(depthMatching==0u){float checker=float(((id.x/16u)+(id.y/16u))&1u);output=float4(lerp(float3(0.06,0.0,0.06),float3(0.3,0.0,0.3),checker),1.0);}
        else if(uv.x<0.5)output=float4(DebugDepthNormal(uv),1.0);
        else output=float4(DebugDepthDither(uv,DebugDepthAt(uv,inverseRefinement!=0u)),1.0);
    }
    if(radius==8) {
        float value=0.0;
        if(depthMatching!=0u){
            int2 at=min(int2(uv*float2(depthSize)),int2(depthSize)-1);
            float raw=DebugSceneDepth.Load(int3(at,0)).x;
            float here=max(reversed!=0?raw:1.0-raw,1e-6);
            static const int2 neighbours[4]={int2(1,0),int2(-1,0),int2(0,1),int2(0,-1)};
            [unroll]for(int k=0;k<4;++k){float otherRaw=DebugSceneDepth.Load(int3(clamp(at+neighbours[k],0,int2(depthSize)-1),0)).x;float other=max(reversed!=0?otherRaw:1.0-otherRaw,1e-6);value=max(value,abs(other-here)/max(min(other,here),1e-6));}
            value=smoothstep(0.05,0.1,value);
        }
        output=float4(value.xxx,1.0);
    }
    if(radius==7)
    {
        float2 pixel=(float2(id.xy)+0.5)*float2(aux)/float2(size);
        float spacing=max(knee,12.0);
        float aa=max(0.8,max(float(aux.x)/size.x,float(aux.y)/size.y)*0.65);
        float2 nearest=round(pixel/spacing);
        float grid=(1.0-smoothstep(1.25,1.25+aa,length(pixel-nearest*spacing)))*lookDistance;
        float4 best=0.0;
        [unroll]for(int y=-1;y<=1;++y)
        [unroll]for(int x=-1;x<=1;++x)
        {
            float2 root=(nearest+float2(x,y))*spacing;
            if(any(root<0.0)||any(root>=float2(aux)))continue;
            int2 at=clamp(int2(root*float2(size)/float2(aux)),0,int2(size)-1);
            float2 backward=hasHistory!=0 ? DebugGuide.SampleLevel(Linear,root/float2(aux),0).xy : FlowIn.Load(int3(at,0)).xy;
            if(!all(abs(backward)<1e6))continue;
            float4 arrow=DrawMotionArrow(pixel,root,backward,spacing,lambda,lookRange,aa,zeroMargin,scale);
            if(arrow.a>best.a)best=arrow;
        }
        float alpha=best.a+grid*(1.0-best.a);
        float3 premult=best.rgb*best.a+float3(0.58,0.60,0.64)*grid*(1.0-best.a);
        output=float4(alpha>1e-6?premult/alpha:0.0,alpha*whiteNits);
        if(hasPrediction!=0)output=float4(lerp(DebugColor.SampleLevel(Linear,uv,0).rgb,output.rgb,output.a),1.0);
    }
    OutFlow[id.xy]=DebugLegend(id.xy,uint(radius),output);
}
)HLSL"
