"""Translate the current native kernels mechanically into ReShade FX compute intrinsics.
No original kernels are reimplemented or simplified; layouts/bindings and API syntax change.
"""
from pathlib import Path
import re,json,hashlib,argparse
parser=argparse.ArgumentParser()
parser.add_argument('--native-root',type=Path,required=True,help='OptiScaler/motion directory containing canonical kernels')
parser.add_argument('--feeder-shader',type=Path,required=True)
parser.add_argument('--out',type=Path,required=True)
args=parser.parse_args();NATIVE=args.native_root;OUT=args.out
OUT.mkdir(parents=True,exist_ok=True)
def strings(file):return '\n'.join(re.findall(r'R"HLSL\((.*?)\)HLSL"',file.read_text(encoding='utf-8-sig'),re.S))
source=strings(NATIVE/'OpticalFlow_Hlsl.h')
scene=strings(NATIVE/'SceneCut_Hlsl.h')
def func(s,name):
    m=re.search(r'\b(?:void|float\d?|bool|int)\s+'+name+r'\s*\(',s)
    start=m.start();brace=s.index('{',m.end());depth=1;i=brace+1
    while depth:
        if s[i]=='{':depth+=1
        elif s[i]=='}':depth-=1
        i+=1
    return s[start:i]
def splitargs(s):
    out=[];begin=0;n=0
    for i,c in enumerate(s):
        if c in '([':n+=1
        elif c in ')]':n-=1
        elif c==',' and n==0:out.append(s[begin:i].strip());begin=i+1
    out.append(s[begin:].strip());return out
def atomic(s):
    for name,new in [('InterlockedAdd','atomicAdd'),('InterlockedExchange','atomicExchange')]:
        while name+'(' in s:
            a=s.index(name+'(');b=a+len(name)+1;n=1;i=b
            while n:
                if s[i]=='(':n+=1
                elif s[i]==')':n-=1
                i+=1
            args=splitargs(s[b:i-1]);dst=args[0]
            if dst.startswith('State['):dst='stState, int2('+dst[6:-1]+')'
            call=new+'('+dst+', '+args[1]+')'
            if len(args)==3:call=args[2]+' = '+call
            s=s[:a]+call+s[i:]
    return s
def translate(s,reads,writes):
    s=re.sub(r'\bsample\b','nativeSampleValue',s) # 'sample' is reserved in FX.
    # FX lowers round() to GLSL.std.450 Round on Vulkan, whose halfway
    # rule differs from HLSL's round-to-nearest-even. Preserve native candidates.
    s=re.sub(r'\bround\s*\(', 'F5RoundEven(', s)
    s=re.sub(r'\[numthreads\([^\]]+\)\]\s*','',s)
    s=s.replace('GroupMemoryBarrierWithGroupSync()','barrier()').replace('DeviceMemoryBarrier()','memoryBarrier()')
    s=atomic(s)
    for original,(sampler,kind) in reads.items():
        s=s.replace(original+'.Load(', 'Read_'+original+'(')
        s=s.replace(original+'.SampleLevel(Linear,', 'Sample_'+original+'(')
    for original,storage in writes.items():
        s=re.sub(r'\b'+original+r'\[([^\]]+)\]\s*=\s*(.*?);',lambda m:'tex2Dstore('+storage+', int2('+m[1]+'), '+m[2]+');',s,flags=re.S)
        s=re.sub(r'\b'+original+r'\[([^\]]+)\]',lambda m:'tex2Dfetch('+storage+', int2('+m[1]+'))',s)
    for label in ['Candidate','Refinement','Events']:
        s=s.replace('tex2Dstore(stTrace'+label+',','WriteTrace'+label+'(')
    # Uniform/source values are read with functions; unlike HLSL constant-buffer aliases,
    # resource handles are bound by ReShade using the samplers/storage declarations below.
    helpers=''
    for original,(sampler,kind) in reads.items():
        access='tex2Dfetch('+sampler+', at.xy)'
        sample='tex2Dlod('+sampler+', float4(uv, 0.0, mip))'
        if kind=='float':access+='.x';sample+='.x'
        if kind=='uint':access='uint('+access+'.x)';sample='uint('+sample+'.x)'
        helpers+=kind+' Read_'+original+'(int3 at){return '+access+';}\n'
        if kind!='uint':helpers+=kind+' Sample_'+original+'(float2 uv,float mip){return '+sample+';}\n'
    return helpers+s

settings={
 'motionFlags':'(F5LOW_ALGORITHM_FLAGS|(F5LOW_STAGE_TRACE && MV_Trace?64u:0u))',
 'photoTolerance':'1e-6','evidenceNoise':'(1.0/255.0)','reservedMotion':'0.0', 
 'radius':'clamp(MV_SearchRadius,0,4)','hasPrediction':'1u','lambda':'MV_SearchPenalty','scale':'1.0',
 'hasHistory':'(MV_UseHistory && HistoryReady()?1u:0u)','knee':'max(MV_ConfidenceKnee,1e-7)','coarseCells':'uint(clamp(MV_CoarseCells,1,9))',
 'depthMatching':'(MV_UseDepth && MV_DepthAvailable?1u:0u)',
 'depthSize':'uint2(BUFFER_WIDTH,BUFFER_HEIGHT)','reversed':'RESHADE_DEPTH_INPUT_IS_REVERSED',
 'hasGlobal':'(MV_GlobalCandidate && GlobalReady()?1u:0u)','inverseRefinement':'F5LOW_INVERSE_REFINEMENT',
 'sceneCutEnabled':'(MV_SceneCut?1u:0u)','whiteNits':'max(MV_WhiteNits,1.0)','zeroMargin':'MV_ZeroMargin',
 'zeroReach':'min(MV_ZeroReach,64u)','stillFrames':'min(MV_StillFrames,255u)','hasAge':'(AgeReady()?1u:0u)',
 'stillEpsilon':'MV_StillEpsilon','stillMargin':'MV_StillMargin','lookWeights':'(MV_LookWeights?1u:0u)',
 'lookRange':'max(MV_LookRange,1e-4)','lookDistance':'max(MV_LookDistance,0.5)'}
parts=[]
# Scratch is private to a single compute workgroup/dispatch. Separate entry
# points never share contents. Alias only arrays whose entry points cannot run
# together in one invocation; preserve every index, value, barrier and operation.
scratch_layout={
 'gCurrentTile':('float','F5ScratchScalar',0,256),
 'gMatchDepth':('float','F5ScratchScalar',256,256),
 'gZeroPrevious':('float','F5ScratchScalar',512,256),
 'gSurface':('float','F5ScratchScalar',0,576),
 'gGuideLuma':('float','F5ScratchScalar',576,576),
 'gCameraPower':('float','F5ScratchScalar',0,128),
 'gCounted':('float','F5ScratchScalar',0,256),
 'gSmooth':('float','F5ScratchScalar',256,256),
 'gBefore':('float','F5ScratchScalar',512,256),
 'gSum':('float','F5ScratchScalar',768,256),
 'gTile':('float4','F5ScratchVector',0,576),
 'gCameraSamples':('float4','F5ScratchVector',0,128),
 'gCameraA':('float4','F5ScratchVector',128,128),
 'gCameraB':('float4','F5ScratchVector',256,128),
 'gCameraC':('float4','F5ScratchVector',384,128),
 'gCameraRows':('float4','F5ScratchVector',512,3),
}
def reuse_scratch(code):
    for variable,(kind,bank,offset,count) in scratch_layout.items():
        declaration=r'groupshared\s+'+kind+r'\s+'+variable+r'\[\s*'+str(count)+r'\s*\]\s*;'
        if not re.search(declaration,code):continue
        code=re.sub(declaration,'',code)
        code=re.sub(r'\b'+variable+r'\[([^\]]+)\]',lambda m:bank+'['+str(offset)+'+('+m[1]+')]',code)
    if 'groupshared int2 gCurrentBase;' in code:
        code=code.replace('groupshared int2 gCurrentBase;','')
        code=re.sub(r'\bgCurrentBase\b','F5ScratchBase',code)
    return code

def namespace(name,code,reads,writes,defs):
    code=reuse_scratch(code)
    merged=settings.copy();merged.update(defs)
    # Native dimensions come from a constant buffer. Preserve that arithmetic
    # contract instead of letting FX fold a reciprocal for odd image sizes.
    # Consumer kernels retain the original FX resource-dimension lowering;
    # estimator reciprocal evaluation needs runtime dimensions for native parity.
    if name not in ['F5Proxy','F5GuideDepth','F5Uncover','F5Guide']:
        for k,v in list(merged.items()):
            v=v.replace('uint2(BUFFER_WIDTH,BUFFER_HEIGHT)','NativeBufferDimensions()')
            v=re.sub(r'uint2\(SIZE([0-5])\)',lambda m:'NativeDimensions('+str(2<<int(m[1]))+'u)',v)
            merged[k]=v
    block='namespace '+name+' {\n'
    for k,v in merged.items():block+='#define '+k+' ('+v+')\n'
    block+=translate(code,reads,writes)+'\n'
    for k in merged:block+='#undef '+k+'\n'
    parts.append(block+'}\n')

# Appended to the standalone, shared-kernel generator's resource translation helpers.
match='\n'.join(func(source,n) for n in ['Sample','Distance','MatchDistance','SameSurface','Cost','ZeroCost','LargestChange','LookWeight','FractionalCost','RefinementIntegerCost','VectorEvidence','Match'])
if re.search(r'\bvoid\s+PrepareCurrent\s*\(',source):
    # Keep native cooperative reads as a mechanical transplant as well. Each
    # level gets its own namespace; unused entry-point resources are stripped.
    begin=source.index('// Cooperative source-image reads only.')
    end=source.index('// The previous frame at a fractional pixel position, filtered.',begin)
    match=source[begin:end]+'\n'+match

for level in range(6):
    coarser=min(level+1,5)
    reads={'CurLuma':('sC'+str(level),'float'),'PrevLuma':('sP'+str(level),'float'),
           'Prediction':('sF'+str(coarser),'float4'),'History':('sHF'+str(level),'float4'),
           'SceneDepth':('sDepth','float'),'GlobalFlow':('sGlobal','float4'),'CutFlag':('sCut','uint'),'LastAge':('sLastAge','uint')}
    namespace('F5Level'+str(level),match,reads,{'OutFlow':'stF'+str(level),'OutAge':'stAge','TraceCandidate':'stTraceCandidate','TraceRefinement':'stTraceRefinement','TraceEvents':'stTraceEvents'},
        {'motionFlags':settings['motionFlags'].replace('(F5LOW_STAGE_TRACE && MV_Trace?64u:0u)','(F5LOW_STAGE_TRACE && MV_Trace && MV_TraceLevel=='+str(level)+'?64u:0u)'),
         'size':'uint2(SIZE'+str(level)+')','aux':'uint2(SIZE'+str(coarser)+')',
         'hasPrediction':'0u' if level==5 else '1u','radius':'clamp(MV_CoarseRadius,0,8)' if level==5 else 'clamp(MV_SearchRadius,0,4)',
         'scale':'(1.0 / '+str(2<<level)+'.0)','stillFrames':'min(MV_StillFrames,255u)' if level==0 else '0u',
         'lookWeights':'(MV_LookWeights?1u:0u)' if level==0 else '0u','zeroMargin':'MV_ZeroMargin' if level==0 else '0.0'})
namespace('F5Median','\n'.join(func(source,n) for n in ['Sort','Median9','Median']),{'FlowIn':('sF0','float4')},
          {'OutFlow':'stMedian'},{'size':'uint2(SIZE0)','aux':'uint2(SIZE0)','scale':'2.0'})
smooth='groupshared float4 gTile[576];\ngroupshared float gSurface[576];\ngroupshared float gGuideLuma[576];\n'+'\n'.join(func(source,n) for n in ['Distance','SameSurface','LookWeight'])+'\n'+strings(NATIVE/'CameraFusion_Hlsl.h')+'\n'+func(source,'SmoothRegion')+'\n'+func(source,'Smooth')
namespace('F5Smooth',smooth,{'FlowIn':('sMedian','float4'),'GuideLuma':('sC0','float'),'SceneDepth':('sDepth','float'),
          'CameraPrevious':('sP0','float'),'History':('sHF0','float4'),'GlobalFlow':('sCameraModel','float4'),'LastAge':('sAge','uint')},
          {'OutFlow':'stSmooth'},{'size':'uint2(SIZE0)','radius':'clamp(MV_SmoothRadius,0,4)',
          'hasGlobal':'(MV_GlobalCandidate && HistoryReady()?2u:0u)',
          'hasAge':'(MV_StillFrames>0 && HistoryReady()?1u:0u)'})
namespace('F5Camera',strings(NATIVE/'CameraModel_Hlsl.h'),
          {'FlowIn':('sMedian','float4'),'SceneDepth':('sDepth','float'),'GlobalFlow':('sGlobal','float4')},
          {'OutFlow':'stCameraModel'}, {'size':'uint2(4,1)','aux':'uint2(SIZE0)','scale':'1.0',
          'hasGlobal':'(MV_GlobalCandidate && GlobalReady()?1u:0u)'})
namespace('F5Global','groupshared float3 gSample[128];\ngroupshared float2 gMiddle;\n'+func(source,'Global'),
          {'FlowIn':('sSmooth','float4'),'SceneDepth':('sDepth','float')},{'OutFlow':'stGlobal'},
          {'size':'uint2(1,1)','aux':'uint2(SIZE0)'})
namespace('F5Luma',strings(NATIVE/'Luma_Hlsl.h')+'\n'+func(source,'Luma'),{'Color':('sBack','float4')},{'OutLuma':'stC0'},
          {'size':'uint2(SIZE0)','aux':'uint2(BUFFER_WIDTH,BUFFER_HEIGHT)'})
for level in range(1,6):
    namespace('F5Down'+str(level),func(source,'Down'),{'CurLuma':('sC'+str(level-1),'float')},{'OutLuma':'stC'+str(level)},
              {'size':'uint2(SIZE'+str(level)+')','aux':'uint2(SIZE'+str(level-1)+')'})
sc=scene[scene.index('static const float kStops'):]
sc=sc.replace('void SceneHist(uint3 group : SV_GroupID, uint t : SV_GroupIndex)','void SceneHist(uint3 group, uint t)')
sc=sc.replace('void SceneDiverge(uint3 group : SV_GroupID, uint i : SV_GroupIndex)','void SceneDiverge(uint3 group, uint i)')
namespace('F5Cut',sc,{'Luma':('sC0','float')},{'State':'stState','Cut':'stCut','Distrust':'stCutDistrust'},
          {'size':'uint2(SIZE0)','hasPrevious':'(SceneHistoryReady()?1u:0u)','threshold':'MV_CutThreshold'})

# These are the native consumer's existing motion reconstruction kernels, not a
# second implementation of bilinear/surface interpolation.
trust=strings(NATIVE/'TrustMask_Hlsl.h')
trustdefs={'size':'uint2(SIZE0)','depthSize':'uint2(BUFFER_WIDTH,BUFFER_HEIGHT)',
           'depthCount':'(MV_DepthAvailable?1u:0u)','reversed':'RESHADE_DEPTH_INPUT_IS_REVERSED',
           'fullPerFlow':'(float(BUFFER_WIDTH)/float((BUFFER_WIDTH+1)/2))',
           'revealTolerance':'MV_RevealTolerance','uncoverReach':'MV_UncoverReach',
           'guideSurface':'(MV_GuideSurface && MV_DepthAvailable?1u:0u)'}
proxy=func(trust,'DepthProxy').replace('SceneDepths[k].Load(','SceneDepth.Load(')
namespace('F5Proxy',proxy,{'SceneDepth':('sDepth','float')},{'OutFloat':'stProxy'},trustdefs)
guideDepth=func(trust,'GuideDepth').replace('SceneDepths[k].Load(','SceneDepth.Load(')
namespace('F5GuideDepth',guideDepth,{'SceneDepth':('sDepth','float')},{'OutFloat':'stGuideDepth'},dict(trustdefs,size='uint2(BUFFER_WIDTH,BUFFER_HEIGHT)'))
uncover='static const float kSky=5e19;\n'+func(trust,'HiddenBefore')+'\n'+func(trust,'Uncover')
uncover=uncover.replace('DepthBefore.GatherRed(Linear, uv)','tex2DgatherR(sPreviousProxyGather, uv)')
uncover=uncover.replace('[unroll]','[loop]') # FXC cannot force-expand FX's lowered nested continue loops.
namespace('F5Uncover',uncover,{'Flow':('sSmooth','float4'),'DepthNow':('sProxy','float'),'DepthBefore':('sPreviousProxy','float')},
          {'OutFlow':'stUncovered'},trustdefs)
guide=func(trust,'GuideMotion').replace('Flow.GetDimensions(flowWidth, flowHeight);','flowWidth=(BUFFER_WIDTH+1)/2;flowHeight=(BUFFER_HEIGHT+1)/2;')
namespace('F5Guide',guide,{'Flow':('sUncovered','float4'),'GuideRawDepth':('sGuideDepth','float'),'GuideProxy':('sProxy','float')},
          {'OutFlow':'stPixelMotion'},dict(trustdefs,size='uint2(BUFFER_WIDTH,BUFFER_HEIGHT)'))
# Port the native per-pixel trust decision, including decaying memory and the
# current GPU scene-cut flag. The statistical counter remains native-only.
namespace('F5Trust','static const float kSky=5e19;\n'+func(trust,'TrustPixel'),
          {'Flow':('sSmooth','float4'),'FlowBefore':('sPreviousSmooth','float4'),
           'DepthNow':('sProxy','float'),'DepthBefore':('sPreviousProxy','float'),
           'LumaNow':('sC0','float'),'LumaBefore':('sP0','float'),
           'MaskBefore':('sPreviousTrust','float'),'CutFlag':('sCut','uint')},{},
          dict(trustdefs,fullPerFlow='(float(NativeBufferDimensions().x)/float(NativeDimensions(2u).x))',hasHistory='(TrustHistoryReady()?1u:0u)',depthBoth='(MV_DepthAvailable && DepthHistoryReady()?1u:0u)',
               depthTolerance='max(MV_TrustDepthTolerance,1e-4)',flowTolerance='max(MV_TrustFlowTolerance,1e-4)',
               lumaTolerance='max(MV_TrustLumaTolerance,1e-4)',decay='saturate(MV_TrustDecay)',debugView='0u'))
rounding='''// Preserve HLSL halfway rounding on the SPIR-V backend as well.
float F5RoundEven(float v) {
#if __RENDERER__ == 0x20000
    float b=floor(v),f=v-b;
    return b+((f>0.5 || (f==0.5 && (int(b)&1)!=0))?1.0:0.0);
#else
    return round(v);
#endif
}
float2 F5RoundEven(float2 v){return float2(F5RoundEven(v.x),F5RoundEven(v.y));}
float3 F5RoundEven(float3 v){return float3(F5RoundEven(v.x),F5RoundEven(v.y),F5RoundEven(v.z));}
float4 F5RoundEven(float4 v){return float4(F5RoundEven(v.x),F5RoundEven(v.y),F5RoundEven(v.z),F5RoundEven(v.w));}
'''
core='// Generated mechanically from the active native F5Low and TrustMask kernels.\n'+rounding+'groupshared float4 F5ScratchVector[576];\ngroupshared float F5ScratchScalar[1152];\ngroupshared int2 F5ScratchBase;\n'+'\n'.join(parts)
(OUT/'F5LowNativeCore.fxh').write_text(core,encoding='utf-8')

header='''// Identical source-image sample reuse is built in. Displaced LK reads remain per pixel.
// Complete F5Low 0.1.31 incremental pipeline and native consumer motion guide.
// Generated from the active OptiScaler sources; no separate motion algorithm.
#include "ReShade.fxh"
#if __RESHADE_PERFORMANCE_MODE__
#error "F5Low native parity requires ReShade Performance Mode OFF (parameter specialization changes matching arithmetic)."
#endif
#ifndef F5LOW_STAGE_TRACE
#define F5LOW_STAGE_TRACE 0
#endif
// The corrected estimator is the only production path; no baseline/experiment switch.
#define F5LOW_ALGORITHM_FLAGS 7u
#define F5LOW_INVERSE_REFINEMENT (MV_InverseRefinement?1u:0u)
// Provider 5 consumes the native pixel guide directly.
#ifndef LUMA_MODE
#define LUMA_MODE 1
#endif
uniform uint MV_FrameCount < source="framecount"; >;
uniform bool MV_DepthAvailable < source="bufready_depth"; >;
// A custom source is runtime data even in performance mode and is excluded from
// presets. ReShade initializes unknown sources to zero: zero means auto size.
uniform uint2 MV_NativeSizeOverride <source="f5low_native_dimensions";ui_hidden=true;> = 0;
uint2 NativeBufferDimensions(){return all(MV_NativeSizeOverride>0u)?MV_NativeSizeOverride:uint2(BUFFER_WIDTH,BUFFER_HEIGHT);}
uint2 NativeDimensions(uint divisor){return (NativeBufferDimensions()+divisor-1u)/divisor;}
uniform bool MV_UseDepth < ui_label="Use selected depth for matching"; > = true;
uniform bool MV_SuppressSky <ui_label="Stop sending sky motion vectors";ui_tooltip="Requires correct depth. Zeroes sky/far-depth output below the cutoff and rejects its temporal history. Far geometry and some smoke may share that depth. Off leaves the complete field active.";> = false;
uniform float MV_SkyDepthCutoff <ui_label="Sky / far-depth cutoff";ui_type="slider";ui_min=0.0;ui_max=0.001;ui_step=0.000001;ui_tooltip="Raw depth distance from the far plane, after normal/reversed conversion. 0 excludes only clear depth. Increase for finite-depth skyboxes such as MGR; this is depth classification, not semantic sky detection.";> = 0.0001;
uniform bool MV_Trace <ui_category="F5Low debug";ui_hidden=(F5LOW_STAGE_TRACE==0);ui_label="Record candidate / LK trace (adds cost)";> = false;
uniform int MV_TraceLevel <ui_hidden=(F5LOW_STAGE_TRACE==0);ui_category="F5Low debug";ui_type="slider";ui_min=0;ui_max=5;ui_label="Trace pyramid level (0 = finest)";> = 0;
uniform bool MV_UseHistory = true;
uniform bool MV_GlobalCandidate = true;
uniform bool MV_InverseRefinement <> = false;
uniform bool MV_SceneCut = true;
uniform int MV_SearchRadius = 1;
uniform int MV_CoarseRadius = 4;
uniform int MV_CoarseCells = 9;
uniform int MV_SmoothRadius <ui_type="slider";ui_min=0;ui_max=4;> = 4;
uniform float MV_SearchPenalty = 0.03;
uniform float MV_ConfidenceKnee = 0.004;
uniform float MV_WhiteNits = 203.0;
uniform float MV_ZeroMargin = 0.0;
uniform uint MV_ZeroReach = 0;
uniform uint MV_StillFrames = 8;
uniform float MV_StillEpsilon = 0.004;
uniform float MV_StillMargin = 0.02;
uniform bool MV_LookWeights = true;
uniform float MV_LookRange = 0.06;
uniform float MV_LookDistance = 8.0;
uniform float MV_CutThreshold = 0.45;
uniform bool MV_GuideSurface <ui_label="Reconstruct motion by depth surface (native GuideMotion)";> = true;
uniform bool MV_UncoverBackground <ui_label="Use native disocclusion motion reconstruction";> = true;
uniform float MV_UncoverReach = 24.0;
uniform float MV_RevealTolerance = 0.25;
uniform float MV_TrustDepthTolerance <ui_category="History trust";> = 0.10;
uniform float MV_TrustFlowTolerance <ui_category="History trust";> = 1.4;
uniform float MV_TrustLumaTolerance <ui_category="History trust";> = 0.25;
uniform float MV_TrustDecay <ui_category="History trust";> = 0.5;
uniform int MV_DebugView <ui_type="combo";ui_category="F5Low debug";ui_label="View";ui_items="Motion arrows\0Normals / depth\0Final optical flow\0Confidence / rejection\0Confidence channels (texture / fit / history)\0History rejection\0Still age\0";> = 0;
uniform float MV_DebugSpacing = 32.0;
uniform float MV_DebugGain = 2.5;
uniform float MV_DebugThickness = 1.25;
uniform float MV_DebugMinimum = 0.4;
uniform float MV_DebugRange = 16.0;
uniform float MV_DebugOpacity = 1.0;
uniform float MV_DebugGrid = 0.18;
uniform bool MV_DebugShowBackground = true;
uniform bool MV_DebugDepthLinearize <ui_label="Linearized depth preview";ui_category="F5Low debug";> = true;
uniform float MV_DebugDepthMultiplier <ui_label="Depth preview multiplier";ui_category="F5Low debug";> = RESHADE_DEPTH_MULTIPLIER;
uniform float MV_DebugDepthFarPlane <ui_label="Depth linearization far plane";ui_category="F5Low debug";> = RESHADE_DEPTH_LINEARIZATION_FAR_PLANE;
'''
resources=[]
def texture(name,w,h,fmt,kind='float4'):
    suffix=name[3:]
    resources.append('texture2D '+name+' <pooled=false;>{Width='+w+';Height='+h+';Format='+fmt+';};\n'
        'sampler2D<'+kind+'> s'+suffix+' {Texture='+name+';AddressU=Clamp;AddressV=Clamp;MinFilter='+('Point' if kind=='uint' else 'Linear')+';MagFilter='+('Point' if kind=='uint' else 'Linear')+';MipFilter=Point;};\n'
        'storage2D<'+kind+'> st'+suffix+' {Texture='+name+';};\n')
for level in range(6):
    div=2<<level;w='((BUFFER_WIDTH+'+str(div-1)+')/'+str(div)+')';h='((BUFFER_HEIGHT+'+str(div-1)+')/'+str(div)+')'
    header+='#define SIZE'+str(level)+' '+w+','+h+'\n'
    for prefix in ['C','P']:texture('MV_'+prefix+str(level),w,h,'R32F','float')
    for prefix in ['F','HF']:texture('MV_'+prefix+str(level),w,h,'RGBA16F')
halfW='((BUFFER_WIDTH+1)/2)';halfH='((BUFFER_HEIGHT+1)/2)'
resources.append('#if F5LOW_STAGE_TRACE\n')
for name in ['TraceCandidate','TraceRefinement','TraceEvents']:texture('MV_'+name,halfW,halfH,'RGBA32F')
resources.append('#endif\n')
for name in ['Median','Smooth','Uncovered','PreviousSmooth']:texture('MV_'+name,halfW,halfH,'RGBA16F')
for name in ['Proxy','PreviousProxy']:texture('MV_'+name,halfW,halfH,'R32F','float')
for name in ['Trust','PreviousTrust']:texture('MV_'+name,halfW,halfH,'R8','float')
for name in ['Age','LastAge']:texture('MV_'+name,halfW,halfH,'R32U','uint')
texture('MV_Global','1','1','RGBA16F')
texture('MV_CameraModel','4','1','RGBA16F')
texture('MV_Depth','BUFFER_WIDTH','BUFFER_HEIGHT','R32F','float')
texture('MV_GuideDepth','BUFFER_WIDTH','BUFFER_HEIGHT','R32F','float')
texture('MV_State','256','19','R32U','uint');texture('MV_Cut','2','1','R32U','uint')
texture('MV_CutDistrust','1','1','R32F','float');texture('MV_HistoryStamp','1','1','RGBA32F')
texture('MV_Ready','1','1','R32U','uint')
header+='\n'.join(resources)
for label in ['Candidate','Refinement','Events']:
    header+='#if F5LOW_STAGE_TRACE\n'
    header+='void WriteTrace'+label+'(int2 p,float4 value){tex2Dstore(stTrace'+label+',p,value); }\n'
    header+='float4 ReadTrace'+label+'(int2 p){return tex2Dfetch(sTrace'+label+',p); }\n'
    header+='#else\nvoid WriteTrace'+label+'(int2 p,float4 value){}\n'
    header+='float4 ReadTrace'+label+'(int2 p){return 0.0;}\n#endif\n'
header+='''namespace F5Low {
texture2D HistoryDistrust <pooled=false;>{Width=BUFFER_WIDTH;Height=BUFFER_HEIGHT;Format=R8;};
texture2D FrameState <pooled=false;>{Width=1;Height=1;Format=RGBA32F;};
texture2D PixelMotionVectors <pooled=false;>{Width=BUFFER_WIDTH;Height=BUFFER_HEIGHT;Format=RGBA16F;};
texture2D Confidence <pooled=false;>{Width=BUFFER_WIDTH;Height=BUFFER_HEIGHT;Format=R16F;};
}
sampler2D<float4> sPixelMotion{Texture=F5Low::PixelMotionVectors;AddressU=Clamp;AddressV=Clamp;MinFilter=Point;MagFilter=Point;MipFilter=Point;};
storage2D<float4> stPixelMotion{Texture=F5Low::PixelMotionVectors;};
storage2D<float> stPublishedTrust{Texture=F5Low::HistoryDistrust;};
storage2D<float> stPublishedConfidence{Texture=F5Low::Confidence;};
storage2D<float4> stPublishedFrameState{Texture=F5Low::FrameState;};
sampler2D<float> sPublishedTrust{Texture=F5Low::HistoryDistrust;AddressU=Clamp;AddressV=Clamp;MinFilter=Linear;MagFilter=Linear;MipFilter=Point;};
sampler2D<float4> sDebugGuide{Texture=F5Low::PixelMotionVectors;AddressU=Clamp;AddressV=Clamp;MinFilter=Linear;MagFilter=Linear;MipFilter=Point;};
sampler sBack{Texture=ReShade::BackBufferTex;AddressU=Clamp;AddressV=Clamp;};
sampler2D<float4> sPreviousProxyGather{Texture=MV_PreviousProxy;AddressU=Clamp;AddressV=Clamp;MinFilter=Linear;MagFilter=Linear;MipFilter=Point;};
bool HistoryReady(){return tex2Dfetch(sReady,int2(0,0)).x!=0u;}
bool SceneHistoryReady(){return (tex2Dfetch(sReady,int2(0,0)).x&32u)!=0u;}
bool TrustHistoryReady(){return (tex2Dfetch(sReady,int2(0,0)).x&16u)!=0u;}
bool GlobalReady(){return (tex2Dfetch(sReady,int2(0,0)).x&2u)!=0u;}
bool AgeReady(){return (tex2Dfetch(sReady,int2(0,0)).x&4u)!=0u;}
bool DepthHistoryReady(){return (tex2Dfetch(sReady,int2(0,0)).x&8u)!=0u;}
'''
header+='sampler sDLSS5_RawDepth{Texture=ReShade::DepthBufferTex;AddressU=Clamp;AddressV=Clamp;MinFilter=Point;MagFilter=Point;MipFilter=Point;};\n'
header+=func(args.feeder_shader.read_text(encoding='utf-8-sig'),'RawDepth')
header+='''\n#include "F5LowNativeCore.fxh"
void CS_FrameReady(uint3 id:SV_DispatchThreadID){float4 s=tex2Dfetch(sHistoryStamp,int2(0,0));bool valid=MV_FrameCount>=2 && s.w>0.5 && abs(s.x-float((MV_FrameCount-1)%65536))<0.25 && (uint(s.w)>>8)==((MV_FrameCount-1u)>>16) && s.y==BUFFER_WIDTH && s.z==BUFFER_HEIGHT;tex2Dstore(stReady,int2(0,0),valid?(uint(s.w)&255u):0u);}
void CS_Clear(uint3 id:SV_DispatchThreadID){if(!MV_SceneCut)return;if(id.x<256 && id.y<19 && (id.y<9 || id.y==18 || !HistoryReady()))tex2Dstore(stState,int2(id.xy),0u);}
void CS_Depth(uint3 id:SV_DispatchThreadID){
 if(id.x>=BUFFER_WIDTH || id.y>=BUFFER_HEIGHT || !MV_DepthAvailable)return;
 float raw=RawDepth((float2(id.xy)+0.5)/float2(BUFFER_WIDTH,BUFFER_HEIGHT));
 tex2Dstore(stDepth,int2(id.xy),raw);
 float guide=RESHADE_DEPTH_INPUT_IS_REVERSED?max(0.0,raw):1.0-min(1.0,raw);
 tex2Dstore(stGuideDepth,int2(id.xy),MV_DepthAvailable?guide:0.0);
}
void CS_Hist(uint3 group:SV_GroupID,uint3 local:SV_GroupThreadID){if(MV_SceneCut)F5Cut::SceneHist(group,local.x);}
void CS_Diverge(uint3 group:SV_GroupID,uint3 local:SV_GroupThreadID){if(!MV_SceneCut){if(local.x==0){tex2Dstore(stCut,int2(0,0),0u);tex2Dstore(stCut,int2(1,0),0u);tex2Dstore(stCutDistrust,int2(0,0),0.0);}return;}F5Cut::SceneDiverge(group,local.x);}
void CS_Luma(uint3 id:SV_DispatchThreadID){F5Luma::Luma(id);}
void CS_Global(uint3 id:SV_DispatchThreadID){if(!MV_GlobalCandidate || !HistoryReady()){if(id.x==0)tex2Dstore(stGlobal,int2(0,0),float4(0,0,0,0));return;}F5Global::Global(id);}
void CS_Median(uint3 id:SV_DispatchThreadID){F5Median::Median(id);}
void CS_CameraModel(uint3 id:SV_DispatchThreadID){if(!MV_GlobalCandidate || !HistoryReady()){if(id.x<4u)tex2Dstore(stCameraModel,int2(id.x,0),0.0);return;}F5Camera::CameraModel(id.x);}

void CS_Smooth(uint3 id:SV_DispatchThreadID,uint3 group:SV_GroupID,uint3 local:SV_GroupThreadID){F5Smooth::Smooth(id,group,local);}
void CS_Proxy(uint3 id:SV_DispatchThreadID){if(!MV_DepthAvailable){if(all(id.xy<uint2(SIZE0)))tex2Dstore(stProxy,int2(id.xy),1e20);return;}F5Proxy::DepthProxy(id);}
void CS_GuideDepth(uint3 id:SV_DispatchThreadID){F5GuideDepth::GuideDepth(id);}
void CS_Uncover(uint3 id:SV_DispatchThreadID){if(!MV_UncoverBackground || !MV_DepthAvailable || !DepthHistoryReady()){if(all(id.xy<uint2(SIZE0)))tex2Dstore(stUncovered,int2(id.xy),tex2Dfetch(sSmooth,int2(id.xy)));return;}F5Uncover::Uncover(id);}
void CS_Guide(uint3 id:SV_DispatchThreadID){
 if(id.x>=BUFFER_WIDTH || id.y>=BUFFER_HEIGHT)return;
 float2 uv=(float2(id.xy)+0.5)/NativeBufferDimensions();
 bool excluded=MV_SuppressSky && MV_DepthAvailable && tex2Dfetch(sGuideDepth,int2(id.xy)).x<=max(MV_SkyDepthCutoff,0.0);
 if(excluded)tex2Dstore(stPixelMotion,int2(id.xy),0.0);else F5Guide::GuideMotion(id);
 tex2Dstore(stPublishedTrust,int2(id.xy),excluded?1.0:tex2Dlod(sTrust,float4(uv,0,0)).x);
 tex2Dstore(stPublishedConfidence,int2(id.xy),excluded?0.0:tex2Dlod(sSmooth,float4(uv,0,0)).w);
}
void CS_Trust(uint3 id:SV_DispatchThreadID){if(all(id.xy<uint2(SIZE0)))tex2Dstore(stTrust,int2(id.xy),F5Trust::TrustPixel(int2(id.xy)));}
void CS_SaveTrust(uint3 id:SV_DispatchThreadID){if(all(id.xy<uint2(SIZE0))){tex2Dstore(stPreviousTrust,int2(id.xy),tex2Dfetch(sTrust,int2(id.xy)));tex2Dstore(stPreviousSmooth,int2(id.xy),tex2Dfetch(sSmooth,int2(id.xy)));}}
float PS_TrustGuide(float4 p:SV_Position,float2 uv:TEXCOORD):SV_Target{return tex2Dlod(sTrust,float4(uv,0,0)).x;}
void CS_SaveProxy(uint3 id:SV_DispatchThreadID){if(all(id.xy<uint2(SIZE0)))tex2Dstore(stPreviousProxy,int2(id.xy),tex2Dfetch(sProxy,int2(id.xy)));}
void CS_SaveAge(uint3 id:SV_DispatchThreadID){if(all(id.xy<uint2(SIZE0)))tex2Dstore(stLastAge,int2(id.xy),tex2Dfetch(sAge,int2(id.xy)));}
void PS_Stamp(float4 p:SV_Position,float2 uv:TEXCOORD,out float4 history:SV_Target0,out float4 published:SV_Target1){history=published=float4(float(MV_FrameCount%65536),BUFFER_WIDTH,BUFFER_HEIGHT,float((MV_FrameCount>>16)*256u)+1+(HistoryReady()&&MV_GlobalCandidate?2:0)+(HistoryReady()&&MV_StillFrames>0?4:0)+(HistoryReady()&&MV_DepthAvailable?8:0)+(HistoryReady()?16:0)+(MV_SceneCut?32:0));}
'''
for level in range(6):
    header+='void CS_Match'+str(level)+'(uint3 id:SV_DispatchThreadID){if(!HistoryReady()){if(all(id.xy<uint2(SIZE'+str(level)+')))tex2Dstore(stF'+str(level)+',int2(id.xy),float4(0,0,0,0));'
    if level==0:header+='if(all(id.xy<uint2(SIZE0)))tex2Dstore(stAge,int2(id.xy),0u);'
    header+='return;}F5Level'+str(level)+'::Match(id);}\n'
    if level:header+='void CS_Down'+str(level)+'(uint3 id:SV_DispatchThreadID){F5Down'+str(level)+'::Down(id);}\n'
    header+='void CS_Save'+str(level)+'(uint3 id:SV_DispatchThreadID){if(all(id.xy<uint2(SIZE'+str(level)+'))){tex2Dstore(stP'+str(level)+',int2(id.xy),tex2Dfetch(sC'+str(level)+',int2(id.xy)));tex2Dstore(stHF'+str(level)+',int2(id.xy),tex2Dfetch(sF'+str(level)+',int2(id.xy)));}}\n'
header+='''void CS_SaveFine(uint3 id:SV_DispatchThreadID){
 if(all(id.xy==0u))CS_FrameReady(id);
 CS_Save0(id);CS_Save1(id);CS_Save2(id);
}
void CS_SaveCoarse(uint3 id:SV_DispatchThreadID){CS_Save3(id);CS_Save4(id);CS_Save5(id);}
void CS_SaveFinal(uint3 id:SV_DispatchThreadID){
 CS_SaveAge(id);CS_SaveProxy(id);CS_SaveTrust(id);
 if(all(id.xy==0u)) {
   float4 value=float4(float(MV_FrameCount%65536),BUFFER_WIDTH,BUFFER_HEIGHT,float((MV_FrameCount>>16)*256u)+1+(HistoryReady()&&MV_GlobalCandidate?2:0)+(HistoryReady()&&MV_StillFrames>0?4:0)+(HistoryReady()&&MV_DepthAvailable?8:0)+(HistoryReady()?16:0)+(MV_SceneCut?32:0));
   tex2Dstore(stHistoryStamp,int2(0,0),value);tex2Dstore(stPublishedFrameState,int2(0,0),value);
 }
}
'''
passes=[]

def cs(name,entry,tx,ty,dx,dy):passes.append('pass '+name+'{ComputeShader='+entry+'<'+str(tx)+','+str(ty)+'>;DispatchSizeX='+str(dx)+';DispatchSizeY='+str(dy)+';}')
halfDispatch=('(BUFFER_WIDTH+15)/16','(BUFFER_HEIGHT+15)/16');fullDispatch=('(BUFFER_WIDTH+7)/8','(BUFFER_HEIGHT+7)/8')
cs('SaveFine','CS_SaveFine',8,8,*halfDispatch)
cs('SaveCoarse','CS_SaveCoarse',8,8,'(BUFFER_WIDTH+127)/128','(BUFFER_HEIGHT+127)/128')
cs('Luma','CS_Luma',8,8,*halfDispatch);cs('Depth','CS_Depth',8,8,*fullDispatch)
cs('ClearSceneScratch','CS_Clear',8,8,32,3);cs('SceneHistogram','CS_Hist',256,1,8,9);cs('SceneDivergence','CS_Diverge',256,1,9,1)
for level in range(1,6):
    div=(2<<level)*8;cs('Down'+str(level),'CS_Down'+str(level),8,8,'(BUFFER_WIDTH+'+str(div-1)+')/'+str(div),'(BUFFER_HEIGHT+'+str(div-1)+')/'+str(div))
for level in reversed(range(6)):
    div=(2<<level)*8;cs('Match'+str(level),'CS_Match'+str(level),8,8,'(BUFFER_WIDTH+'+str(div-1)+')/'+str(div),'(BUFFER_HEIGHT+'+str(div-1)+')/'+str(div))
cs('Median','CS_Median',8,8,*halfDispatch);cs('CameraModel','CS_CameraModel',128,1,1,1);cs('Smooth','CS_Smooth',16,16,'(BUFFER_WIDTH+31)/32','(BUFFER_HEIGHT+31)/32');cs('GlobalCandidate','CS_Global',128,1,1,1)
cs('DepthProxy','CS_Proxy',8,8,*halfDispatch)
cs('Trust','CS_Trust',8,8,*halfDispatch)

cs('Uncover','CS_Uncover',8,8,*halfDispatch);cs('GuideMotion','CS_Guide',8,8,*fullDispatch)
cs('SaveFinal','CS_SaveFinal',8,8,*halfDispatch)
header+='technique ExperimentalMotionVectors <ui_label="F5Low complete native motion pipeline";>{\n'+'\n'.join(passes)+'\n}\n'

# Debug uses the exact same display kernel as native, with its output size fixed
# to the half-resolution native preview. Enlarging it does not touch any vectors.
debug=strings(NATIVE/'FlowDebugLegend_Hlsl.h')+'\n'+strings(NATIVE/'MotionArrows_Hlsl.h')+'\n'+strings(NATIVE/'FlowDiagnostics_Hlsl.h')
debug=debug.replace('float4 x=DebugMatch.Load(int3(0,0,0)),y=DebugMatch.Load(int3(1,0,0));','float4 x=tex2Dfetch(sCameraModel,int2(0,0)),y=tex2Dfetch(sCameraModel,int2(1,0));')
debug=debug.replace('[unroll]','[loop]') # Display-only FX lowering cannot forcibly unroll nested continue loops.
# The helper file includes its arrow macro; strings() extracts its own literal once.
debug=debug.replace('float historyConfidence=hasHistory!=0 ? 1.0-DebugGuide.SampleLevel(Linear,uv,0).x : 0.0;',
                    'float historyConfidence=1.0-tex2Dlod(sPublishedTrust,float4(uv,0,0)).x;')
debug=debug.replace('return DebugGuide.SampleLevel(Linear,uv,0).x;', 'return tex2Dlod(sPublishedTrust,float4(uv,0,0)).x;')

referenceRaw=func(args.feeder_shader.read_text(encoding='utf-8-sig'),'RawDepth')
referenceRaw=referenceRaw.replace('float RawDepth(', 'float DebugDepthRaw(').replace('sDLSS5_RawDepth','ReShade::DepthBuffer')
debug=debug.replace(func(debug,'DebugDepthRaw'),referenceRaw)
debug=debug.replace('FlowIn.Load(int3(id.xy,0))','DebugFlowAt(id.xy)')
debug=debug.replace('FlowIn.Load(int3(at,0))','DebugFlowAt(uint2(at))')
debug=debug.replace('DebugColor.Load(int3(at,0))','ReadTraceCandidate(at)')
debug=debug.replace('DebugCurrentLuma.Load(int3(at,0))','ReadTraceRefinement(at)')
debug=debug.replace('float4 events=DebugSceneDepth.Load(int3(at,0));','float4 events=ReadTraceEvents(at);')
reads={'DebugColor':('sBack','float4'),'DebugCurrentLuma':('sC0','float4'),'DebugPreviousLuma':('sP0','float'),
       
       'DebugSceneDepth':('sDepth','float4'),'DebugGuide':('sDebugGuide','float4'),'DebugMatch':('sF0','float4'),'DebugAge':('sAge','uint')}
debug=re.sub(r'Texture2D<[^>]+>\s+\w+\s*:\s*register\(t\d+\);','',debug)
debug='uint F5DebugMode(){static const uint modes[7]={7u,6u,0u,18u,5u,17u,12u};return modes[int(clamp(MV_DebugView,0,6))];}\nfloat4 DebugFlowAt(uint2 at){return tex2Dfetch(sSmooth,int2(at));}\n'+debug
debugdefs={'size':'(F5DebugMode()==6u?NativeBufferDimensions():NativeDimensions(2u))','aux':'NativeBufferDimensions()','radius':'F5DebugMode()','scale':'MV_DebugRange',
           'knee':'MV_DebugSpacing','lambda':'MV_DebugGain','lookRange':'MV_DebugThickness','zeroMargin':'MV_DebugMinimum',
           'hasAge':'(HistoryReady() && MV_StillFrames>0u?1u:0u)','stillFrames':'min(MV_StillFrames,255u)','coarseCells':'uint(MV_TraceLevel)',
           'lookDistance':'MV_DebugGrid','whiteNits':'MV_DebugOpacity','hasPrediction':'(MV_DebugShowBackground?1u:0u)','hasHistory':'1u',
           'hasGlobal':'(MV_GlobalCandidate && HistoryReady()?2u:0u)',
           'inverseRefinement':'(MV_DebugDepthLinearize?1u:0u)','photoTolerance':'MV_DebugDepthMultiplier','evidenceNoise':'MV_DebugDepthFarPlane',
           'depthMatching':'(MV_DepthAvailable?1u:0u)','depthSize':'NativeBufferDimensions()','reversed':'RESHADE_DEPTH_INPUT_IS_REVERSED'}
block='namespace F5Debug {\n'+''.join('#define '+k+' ('+v+')\n' for k,v in debugdefs.items())
block+=translate(debug,reads,{'OutFlow':'stPreview'}).replace('tex2Dstore(stPreview,','WriteDebugPreview(')+'\n'+''.join('#undef '+k+'\n' for k in debugdefs)+'}\n'
texture('MV_Preview',halfW,halfH,'RGBA16F')
texture('MV_DepthPreview','BUFFER_WIDTH','BUFFER_HEIGHT','RGBA8')
# The preview resource was declared after the main resources were emitted.
header+='\n'.join(resources[-2:])
header+='void WriteDebugPreview(int2 p,float4 value){if(MV_DebugView==1)tex2Dstore(stDepthPreview,p,value);else tex2Dstore(stPreview,p,value); }\n'
(OUT/'F5LowDebug.fxh').write_text(block,encoding='utf-8')
header+='''#include "F5LowDebug.fxh"
void CS_Debug(uint3 id:SV_DispatchThreadID){uint2 dims=MV_DebugView==1?NativeBufferDimensions():NativeDimensions(2u);if(any(id.xy>=dims))return;if(!HistoryReady()){WriteDebugPreview(int2(id.xy),float4(0,0,0,0));return;}F5Debug::Visualise(id);}
float4 PS_Debug(float4 p:SV_Position,float2 uv:TEXCOORD):SV_Target{return MV_DebugView==1?tex2Dlod(sDepthPreview,float4(uv,0,0)):tex2Dlod(sPreview,float4(uv,0,0));}
technique F5Low_Debug <ui_label="F5Low debug: arrows, depth, flow, confidence, history and still age";>{
pass Preview{ComputeShader=CS_Debug<8,8>;DispatchSizeX=(BUFFER_WIDTH+7)/8;DispatchSizeY=(BUFFER_HEIGHT+7)/8;}
pass Display{VertexShader=PostProcessVS;PixelShader=PS_Debug;}}
'''
(OUT/'ExperimentalMotionVectors.fx').write_text(header,encoding='utf-8')
(OUT/'native-kernel-map.json').write_text(json.dumps({'nativeHeaderSha256':hashlib.sha256((NATIVE/'OpticalFlow_Hlsl.h').read_bytes()).hexdigest(),
    'consumerHeaderSha256':hashlib.sha256((NATIVE/'TrustMask_Hlsl.h').read_bytes()).hexdigest(),
    'sourceHashes':{name:hashlib.sha256((NATIVE/name).read_bytes()).hexdigest() for name in ['OpticalFlow_Hlsl.h','TrustMask_Hlsl.h','Luma_Hlsl.h','SceneCut_Hlsl.h','FlowDiagnostics_Hlsl.h','MotionArrows_Hlsl.h','FlowDebugLegend_Hlsl.h','CameraModel_Hlsl.h','CameraFusion_Hlsl.h']},
    'nativeMotionKernels':['Luma','Down','Match','Median','CameraModel','Smooth','Global'],
    'nativeConsumerKernels':['TrustPixel','DepthProxy','GuideDepth','HiddenBefore','Uncover','GuideMotion'],
    'nativeDebugKernelsPorted':True,'nativeFullMotionGuidePorted':True,'passes':len(passes),
    'pixelGuideFormat':'RGBA16F; full-resolution pixels; previous minus current',
    'adaptations':['resource bindings and FX intrinsics','separate namespaces per pyramid level','integer ages R32U instead of R8U; same 0..255 values',
                   'history continuity tracked by frame stamp','single selected depth buffer','Smooth loop scheduling attribute for FXC','workgroup scratch allocation reused only between separate compute entry points; per-entry ranges disjoint and sampling unchanged'],
    'validation':'See the matching source hashes and separate GPU readback audit; translation alone is not proof of parity.'},indent=2),encoding='utf-8')
print('Generated full native pipeline and consumer guide:',len(passes),'passes, core',len(core),'bytes')
