#pragma once
// Per-pixel photometric validation of a geometric prior. No LK sampling positions
// or matching decisions are shared between pixels.
#define F5LOW_CAMERA_FUSION_HLSL R"HLSL(
bool CameraExactStatic(int2 p,float cur[16]) {
    float largest=0.0;
    [unroll] for(int j=0;j<4;++j)
        [unroll] for(int i=0;i<4;++i) {
            int2 q=clamp(p+int2(2*i-3,2*j-3),0,int2(size)-1);
            largest=max(largest,abs(cur[j*4+i]-CameraPrevious.Load(int3(q,0))));
        }
    if(largest>1e-7)return false;
    // Preserve the same even-offset protection as Still Age for small features.
    static const int2 even[5]={int2(0,0),int2(-2,0),int2(2,0),int2(0,-2),int2(0,2)};
    [unroll] for(int k=0;k<5;++k) {
        int2 q=clamp(p+even[k],0,int2(size)-1);
        largest=max(largest,abs(GuideLuma.Load(int3(q,0))-CameraPrevious.Load(int3(q,0))));
    }
    return largest<=1e-7;
}

float CameraPhotoCost(int2 p,float2 motion,float w[16],float cur[16]) {
    float difference[16];float mean=0.0,total=0.0;
    [unroll] for(int j=0;j<4;++j)
        [unroll] for(int i=0;i<4;++i) {
            int k=j*4+i;
            int2 q=clamp(p+int2(2*i-3,2*j-3),0,int2(size)-1);
            float r=CameraPrevious.SampleLevel(Linear,(float2(q)+motion*0.5+0.5)/float2(size),0);
            difference[k]=cur[k]-r;
            mean+=w[k]*difference[k];total+=w[k];
        }
    mean/=total;float cost=0.0;
    [unroll] for(int k=0;k<16;++k)cost+=w[k]*abs(difference[k]-mean);
    return cost*16.0/total;
}

// Regional proposals are a separately validated regularization change, not
// source-sample sharing. Every pixel validates a proposal in its own original
// depth/luminance-weighted photometric window before accepting it.
float2 CameraFuse(int2 p,float2 motion,float structure,float vectorEvidence,bool cached,
                  float2 regional,float conflict,float coherence,inout float support) {
    support=saturate(support);
    // Texture structure alone does not establish the selected correspondence.
    // A noisy but detailed surface must remain eligible for a validated prior.
    float ambiguous=1.0-smoothstep(0.02,0.15,min(structure,vectorEvidence));
    bool regionalReady=cached && conflict>0.0;
    float4 mx=0.0,my=0.0;
    if(hasGlobal==2u){mx=GlobalFlow.Load(int3(0,0,0));my=GlobalFlow.Load(int3(1,0,0));}
    bool cameraReady=mx.w>0.5 && my.w>0.0 && max(ambiguous,conflict)>0.0;
    if(!regionalReady && !cameraReady)return motion;
    float3 position=float3(1.0,2.0*(float2(p)+0.5)/float2(size)-1.0);
    float2 camera=float2(dot(mx.xyz,position),dot(my.xyz,position));
    float cur[16],w[16];
    bool foregroundEdge=false;
    int2 origin=(p>>4)*16-4; // p is a valid nonnegative output pixel
    int2 local=p-origin;
    float zc=depthMatching!=0u ? (cached?gSurface[local.y*24+local.x]:Distance((float2(p)+0.5)/float2(size))):0.0;
    float lc=cached?gTile[local.y*24+local.x].w:GuideLuma.Load(int3(p,0));
    [unroll] for(int j=0;j<4;++j)
        [unroll] for(int i=0;i<4;++i) {
            int k=j*4+i;
            int2 offset=int2(2*i-3,2*j-3);
            int2 q=clamp(p+offset,0,int2(size)-1);
            int2 at=q-origin;
            cur[k]=cached?gTile[at.y*24+at.x].w:GuideLuma.Load(int3(q,0));
            float z=depthMatching!=0u ? (cached?gSurface[at.y*24+at.x]:Distance((float2(q)+0.5)/float2(size))):0.0;
            foregroundEdge=foregroundEdge || (depthMatching!=0u && z>zc*1.10);
            w[k]=depthMatching!=0u?SameSurface(z,zc):1.0;
            if(lookWeights!=0u)w[k]*=LookWeight(cur[k],lc,float2(offset));
        }
    if(foregroundEdge)return motion;
    float2 measured=cached?gTile[local.y*24+local.x].xy:FlowIn.Load(int3(p,0)).xy;
    if(hasAge!=0u && stillFrames!=0u && LastAge.Load(int3(p,0))>=1u &&
       dot(measured,measured)<0.01 && CameraExactStatic(p,cur)) {
        support=1.0;return float2(0.0,0.0);
    }
    float ownCost=CameraPhotoCost(p,motion,w,cur);
    float bestCost=ownCost;
    float regionalAllowance=(16.0/255.0)*(1.0-0.5*saturate(structure));
    bool confirmedRegion=false;
    float acceptedRegion=0.0;
    if(regionalReady) {
        float regionCost=CameraPhotoCost(p,regional,w,cur);
        float compatible=saturate(1.0-max(regionCost-ownCost,0.0)/regionalAllowance);
        float blend=conflict*compatible*compatible*(3.0-2.0*compatible);
        motion=lerp(motion,regional,blend);
        acceptedRegion=blend;
        support=max(support,blend*(0.5+0.45*coherence));
        confirmedRegion=regionCost+0.25*regionalAllowance<ownCost;
        bestCost=min(ownCost,regionCost);
    }
    if(cameraReady) {
        float cameraCost=CameraPhotoCost(p,camera,w,cur);
        float ambiguity=max(ambiguous,conflict);
        float allowance=(48.0/255.0)*ambiguity*my.w;
        // A photometrically confirmed independent regional motion (e.g. smoke)
        // must not be overwritten by a merely plausible stationary camera.
        if(confirmedRegion)allowance=min(allowance,0.5*regionalAllowance);
        // Inside the photometrically indistinguishable band, use the
        // coherent proposal fully. Leaving a fraction of an unsupported vector
        // behind preserves its arbitrary direction even with a good model.
        float compatible=1.0-smoothstep(0.5,1.0,max(cameraCost-bestCost,0.0)/max(allowance,1e-6));
        // Independent low-contrast advection can fit substantially better than
        // the camera despite a small absolute residual. Preserve that evidence.
        float ownPreference=smoothstep(0.25,0.60,(cameraCost-bestCost)/max(cameraCost,1e-4));
        float independentRegion=smoothstep(0.05,0.25,length(regional-camera));
        float regionEvidence=acceptedRegion*smoothstep(0.6,0.9,coherence)*independentRegion;
        float motionCertainty=smoothstep(0.02,0.15,vectorEvidence);
        regionEvidence*=confirmedRegion?1.0:motionCertainty;
        float localEvidence=ownPreference*max(motionCertainty*smoothstep(0.15,0.65,coherence),regionEvidence);
        float cameraStrength=saturate(my.w/0.75);
        float blend=ambiguity*cameraStrength*compatible*compatible*(3.0-2.0*compatible)*(1.0-localEvidence);
        support=max(support,0.85*localEvidence);
        motion=lerp(motion,camera,blend);
        support=max(support,blend*my.w);
    }
    return motion;
}
)HLSL"
