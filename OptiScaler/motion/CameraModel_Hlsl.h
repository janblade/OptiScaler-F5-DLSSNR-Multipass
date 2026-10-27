#pragma once
// One contemporaneous finest-flow model, never a shared per-pixel matching window.
// Fitting is an independently tested motion prior, separate from sample reuse.
#define F5LOW_CAMERA_MODEL_HLSL                                                                                        \
    R"HLSL(
groupshared float4 gCameraSamples[128]; // observed full-resolution XY, texture weight, within-cell motion variance
groupshared float2 gCameraPositions[128];
groupshared float4 gCameraA[128];
groupshared float4 gCameraB[128];
groupshared float4 gCameraC[128];
groupshared float gCameraPower[128];
groupshared float4 gCameraRows[3];
groupshared float2 gCameraMedian;

float2 CameraEvaluate(float2 position) {
    float3 v=float3(1.0,position);
    return float2(dot(gCameraRows[0].xyz,v),dot(gCameraRows[1].xyz,v));
}

void CameraReduce(uint t) {
    GroupMemoryBarrierWithGroupSync();
    [unroll] for(uint stride=64u;stride>0u;stride>>=1u) {
        if(t<stride) {
            gCameraA[t]+=gCameraA[t+stride];
            gCameraB[t]+=gCameraB[t+stride];
            gCameraC[t]+=gCameraC[t+stride];
            gCameraPower[t]+=gCameraPower[t+stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

void CameraSolve() {
    float4 a=gCameraA[0],b=gCameraB[0],c=gCameraC[0];
    float k00=a.w*b.y-b.x*b.x;
    float k01=a.z*b.x-a.y*b.y;
    float k02=a.y*b.x-a.z*a.w;
    float k11=a.x*b.y-a.z*a.z;
    float k12=a.y*a.z-a.x*b.x;
    float k22=a.x*a.w-a.y*a.y;
    float determinant=a.x*k00+a.y*k01+a.z*k02;
    float weight=max(a.x,1e-12);
    float xx=a.w/weight-(a.y/weight)*(a.y/weight);
    float xy=b.x/weight-(a.y/weight)*(a.z/weight);
    float yy=b.y/weight-(a.z/weight)*(a.z/weight);
    float coverage=max(0.5*(xx+yy-sqrt((xx-yy)*(xx-yy)+4.0*xy*xy)),0.0);
    bool valid=a.x>0.1 && determinant>1e-5*a.x*a.x*a.x && coverage>0.005 && (xx+yy-coverage>0.10);
    if(valid) {
        float3 bx=float3(b.z,b.w,c.x),by=c.yzw;
        float3 r0=float3(k00,k01,k02),r1=float3(k01,k11,k12),r2=float3(k02,k12,k22);
        float3 fitX=float3(dot(r0,bx),dot(r1,bx),dot(r2,bx))/determinant;
        float3 fitY=float3(dot(r0,by),dot(r1,by),dot(r2,by))/determinant;
        if(coverage<0.05) {
            // A narrow strip cannot independently constrain both cross-axis
            // slopes. Couple rotation and zoom through a similarity model.
            float aspect=float(aux.y)/float(aux.x);
            float denominator=a.w+aspect*aspect*b.y-(a.y*a.y+aspect*aspect*a.z*a.z)/weight;
            float zoom=(b.w+aspect*c.w-(a.y*b.z+aspect*a.z*c.y)/weight)/max(denominator,1e-12);
            float rotation=(c.z-aspect*c.x-(a.y*c.y-aspect*a.z*b.z)/weight)/max(denominator,1e-12);
            float2 mean=float2(a.y,aspect*a.z)/weight;
            float2 meanMotion=float2(b.z,c.y)/weight;
            fitX=float3(meanMotion.x-zoom*mean.x+rotation*mean.y,zoom,-rotation*aspect);
            fitY=float3(meanMotion.y-rotation*mean.x-zoom*mean.y,rotation,zoom*aspect);
        }
        float2 average=float2(bx.x,by.x)/weight;
        float explained=(dot(fitX,bx)+dot(fitY,by))/weight;
        float noise=max(gCameraPower[0]/weight-explained,0.0);
        float gain=max(explained-dot(average,average),0.0);
        // A continuously regularized affine model: spatial slope needs evidence
        // beyond residual noise. A pan cannot become a noisy rotation by overfit.
        float floorGain=0.0025+0.05*noise;
        float geometric=saturate((gain-floorGain)/max(gain+floorGain,1e-12));
        fitX.yz*=geometric;fitY.yz*=geometric;
        float2 centre=float2(a.y,a.z)/weight;
        fitX.x=average.x-dot(fitX.yz,centre);
        fitY.x=average.y-dot(fitY.yz,centre);
        gCameraRows[0]=float4(fitX,1.0);
        gCameraRows[1]=float4(fitY,coverage);
    } else {
        gCameraRows[0].w=0.0;
    }
}

[numthreads(128,1,1)]
void CameraModel(uint t : SV_GroupIndex) {
    float4 selected=0.0;
    float2 position=0.0;
    float weight=0.0;
    float2 sumMotion=0.0,sumPosition=0.0;
    float sumSquares=0.0;
    // Fixed stratified source coverage, with continuous weighted contributions.
    // No maximum-confidence winner can flip the cell to a different observation.
    // These are model-fit moments, never a replacement pixel's matching decision.
    [loop] for(uint s=0u;s<64u;++s) {
        float2 uv=float2((t%16u+(s%8u+0.5)/8.0)/16.0,(t/16u+(s/8u+0.5)/8.0)/8.0);
        int2 at=min(int2(uv*float2(aux)),int2(aux)-1);
        float4 f=FlowIn.Load(int3(at,0));
        if(any(!isfinite(f.xyz)))continue; // one bad sample must not void the whole frame's fit
        float w=f.z;
        // Texture is evidence of a correspondence, not permission for a small
        // highly textured foreground to own the camera fit's spatial votes.
        w=min(w,0.05);
        w*=min(f.z/0.05,1.0);
        if(depthMatching!=0u) {
            float d=SceneDepth.Load(int3(min(int2(uv*float2(depthSize)),int2(depthSize)-1),0));
            if(reversed!=0u ? d<=1e-6 : d>=0.999999)w=0.0; // the sky, as Global has it
            // Prefer the farther observed surface for the camera hypothesis.
            // This only weights model inference; no pixel's matching weights change.
            w/=max(reversed!=0u?d:1.0-d,1e-6);
        }
        float2 motion=f.xy*scale;
        float2 here=2.0*(float2(at)+0.5)/float2(aux)-1.0;
        sumMotion+=w*motion;
        sumPosition+=w*here;
        sumSquares+=w*dot(motion,motion);
        weight+=w;
    }
    if(weight>0.0) {
        float2 average=sumMotion/weight;
        selected=float4(average,weight/64.0,max(sumSquares/weight-dot(average,average),0.0));
        position=sumPosition/weight;
    }
    // Bound each spatial cell's influence after its own depth-weighted moments.
    // Reciprocal hardware depth can otherwise let an incoherent far sky dominate
    // every well-observed surface. Within-cell motion variance is uncertainty,
    // not extra camera evidence. Retain that same variance in final RMS below.
    // This only changes the fitted prior; per-pixel matching/LK samples are intact.
    selected.z=selected.z/(1.0+selected.z/0.05);
    selected.z/=1.0+selected.w/0.25;
    gCameraSamples[t]=selected;
    gCameraPositions[t]=position;
    if(t==0u)gCameraMedian=0.0;
    GroupMemoryBarrierWithGroupSync();
    float2 below=0.0;float total=0.0;
    [loop] for(uint k=0u;k<128u;++k) {
        float3 other=gCameraSamples[k].xyz;
        total+=other.z;
        below.x+=(other.x<selected.x || (other.x==selected.x && k<t))?other.z:0.0;
        below.y+=(other.y<selected.y || (other.y==selected.y && k<t))?other.z:0.0;
    }
    float middle=0.5*total;
    if(below.x<middle && middle<=below.x+selected.z)gCameraMedian.x=selected.x;
    if(below.y<middle && middle<=below.y+selected.z)gCameraMedian.y=selected.y;
    GroupMemoryBarrierWithGroupSync();
    if(t==0u) {
        gCameraRows[0]=float4(gCameraMedian.x,0.0,0.0,1.0);
        gCameraRows[1]=float4(gCameraMedian.y,0.0,0.0,0.0);
        gCameraRows[2]=0.0;
    }
    GroupMemoryBarrierWithGroupSync();
    float threshold=1.5+0.05*length(gCameraMedian);
    [loop] for(uint iteration=0u;iteration<3u;++iteration) {
        float residual=length(selected.xy-CameraEvaluate(position));
        float robust=rcp(1.0+(residual/threshold)*(residual/threshold));
        float w=selected.z*robust;
        float x=position.x,y=position.y;
        gCameraA[t]=w*float4(1.0,x,y,x*x);
        gCameraB[t]=w*float4(x*y,y*y,selected.x,x*selected.x);
        gCameraC[t]=w*float4(y*selected.x,selected.y,x*selected.y,y*selected.y);
        gCameraPower[t]=w*(dot(selected.xy,selected.xy)+selected.w);
        CameraReduce(t);
        if(t==0u)CameraSolve();
        GroupMemoryBarrierWithGroupSync();
    }
    float residual=length(selected.xy-CameraEvaluate(position));
    bool inlier=residual<threshold && selected.z>1e-4;
    float w=inlier?selected.z:0.0;
    // Cell variance remains part of model uncertainty; averaging cannot turn a
    // noisy collection of observations into falsely perfect confidence.
    gCameraA[t]=float4(w,selected.z,w*(residual*residual+selected.w),inlier?1.0:0.0);
    gCameraB[t]=w*float4(position.x,position.y,position.x*position.x,position.x*position.y);
    gCameraC[t]=float4(w*position.y*position.y,0.0,0.0,0.0);
    gCameraPower[t]=0.0;
    CameraReduce(t);
    if(t==0u) {
        float4 a=gCameraA[0],b=gCameraB[0],c=gCameraC[0];
        float sum=max(a.x,1e-12);
        float xx=b.z/sum-(b.x/sum)*(b.x/sum);
        float xy=b.w/sum-(b.x/sum)*(b.y/sum);
        float yy=c.x/sum-(b.y/sum)*(b.y/sum);
        float coverage=max(0.5*(xx+yy-sqrt((xx-yy)*(xx-yy)+4.0*xy*xy)),0.0);
        float rms=sqrt(max(a.z/sum,0.0));
        float agreement=a.x/max(a.y,1e-12);
        bool finite=all((asuint(float4(gCameraRows[0].xyz,rms))&0x7f800000u)!=0x7f800000u) &&
                    all((asuint(float4(gCameraRows[1].xyz,agreement))&0x7f800000u)!=0x7f800000u);
        bool valid=gCameraRows[0].w>0.5 && finite && a.w>=24.0 && coverage>0.005 && (xx+yy-coverage>0.10) && agreement>=0.70 && rms<threshold;
        gCameraRows[0].w=valid?1.0:0.0;
        gCameraRows[1].w=valid?saturate(agreement*(1.0-0.5*rms/threshold)):0.0;
        gCameraRows[2]=float4(rms,agreement,coverage,a.w);
        OutFlow[uint2(0,0)]=gCameraRows[0];
        OutFlow[uint2(1,0)]=gCameraRows[1];
        OutFlow[uint2(2,0)]=gCameraRows[2];
        OutFlow[uint2(3,0)]=hasGlobal!=0u?GlobalFlow.Load(int3(0,0,0)):float4(0.0,0.0,0.0,0.0);
    }
}
)HLSL"
