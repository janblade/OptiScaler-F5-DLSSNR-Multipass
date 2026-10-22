"""Reproducible, non-production experiments against the shipped 0fa378f4 host.

Optional (tools/test_superior_port.ps1 -GpuCandidates). 0fa378f4 is a commit of the Superior port's own history; it
exists only in a clone with the PR #230 head (git fetch origin pull/230/head). Without it this prints why and exits 3,
which the runner reports as a skipped mode.
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
host = root / 'OptiScaler/shaders/dlssnr/precompile'
out = root / 'artifacts/validation/four-candidates'
if subprocess.run(['git', 'cat-file', '-e', '0fa378f4^{commit}'], cwd=root,
                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0:
    print('The four-candidate experiments need the Superior port commit 0fa378f4, which is not in this clone '
          '(git fetch origin pull/230/head); skipped')
    raise SystemExit(3)
out.mkdir(parents=True, exist_ok=True)
dxc = root / 'OptiScaler/shaders/shader_tools/dxc.exe'
def shipped(path):
    return subprocess.check_output(['git', 'show', '0fa378f4:' + path], cwd=root).decode('utf-8')

source = shipped('OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl')
estimator = shipped('OptiScaler/shaders/dlssnr/superior/HostResolve.hlsli')
source = source.replace('#include "../superior/HostResolve.hlsli"', '#include "estimator.hlsli"')
(out / 'estimator.hlsli').write_text(estimator)

def compile_shader(text, name, defines=()):
    path = out / (name + '.hlsl')
    path.write_text(text)
    subprocess.run([str(dxc), '-T', 'cs_6_0', '-E', 'CSMain', '-O3', '-Qstrip_debug', '-Qstrip_reflect',
                    '-I', str(host), '-I', str(host / '../superior'),
                    *[a for d in defines for a in ('-D', d)], str(path),
                    '-Fo', str(out / (name + '_Shader.cso'))], check=True)

lds = '''
groupshared float3 sProxy[144];
groupshared float3 sResidual[144];
static int2 sLo, sSize;
static bool sUse;
void PrepareSuperiorTile(uint2 group, uint lane)
{
    uint w,h; gSource.GetDimensions(w,h);
    float2 a=(float2(group*8u)+0.5)/float2(gWidth,gHeight);
    float2 b=(float2(min(group*8u+7u,uint2(gWidth,gHeight)-1u))+0.5)/float2(gWidth,gHeight);
#if defined(DLSSNR_SUPERIOR_HOST_PACKED)
    a=SuperiorPackedUv(a); b=SuperiorPackedUv(b);
#endif
    int2 first=int2(floor(a*float2(w,h)));
    int2 last=int2(floor(b*float2(w,h)));
    sLo=clamp(min(first,last)-1,0,int2(w,h)-1);
    int2 hi=clamp(max(first,last)+1,0,int2(w,h)-1);
    sSize=hi-sLo+1;
    sUse=gCompareMode==0 && all(sSize<=12);
    if(sUse)
    {
        for(uint i=lane;i<(uint)(sSize.x*sSize.y);i+=64)
        {
            int2 p=sLo+int2(i%sSize.x,i/sSize.x);
            sProxy[i]=gSource.Load(int3(p,0)).rgb;
            sResidual[i]=gModel.Load(int3(p,0)).rgb;
        }
    }
    GroupMemoryBarrierWithGroupSync();
}
'''
# Keep the exact estimator's sourcePos rounding, tap order, weights and accumulation.
lds_estimator = estimator.replace('float3 SuperiorShapeEdit', lds + '\nfloat3 SuperiorShapeEdit')
lds_estimator = lds_estimator.replace('const float3 proxy = gSource.Load(int3(p, 0)).rgb;',
    'const uint index=(p.y-sLo.y)*sSize.x+(p.x-sLo.x);\n            const float3 proxy = sUse ? sProxy[index] : gSource.Load(int3(p, 0)).rgb;')
lds_estimator = lds_estimator.replace('const float3 residual = gModel.Load(int3(p, 0)).rgb;',
    'const float3 residual = sUse ? sResidual[index] : gModel.Load(int3(p, 0)).rgb;')
(out / 'lds.hlsli').write_text(lds_estimator)
cached = estimator.replace('float2 SuperiorPackedUv(float2 uv)', 'static uint2 sPixel;\nfloat2 SuperiorPackedUv(float2 uv)')
cached = cached.replace('    // Fix evaluation order', '''    if(gCompareMode==0u)
        return float2(gMotion.Load(int3(sPixel.x,1,0)).x,gMotion.Load(int3(sPixel.y,2,0)).x);
    // Fix evaluation order''')
(out / 'cached.hlsli').write_text(cached)
for packed in (False, True):
    for curve in (-1, 0, 2, 4, 6):
        suffix = ('packed_' if packed else '') + ('sdr' if curve < 0 else 'hdr'+str(curve))
        defines = ['DLSSNR_SUPERIOR_HOST_RESOLVE=1']
        if packed: defines += ['DLSSNR_SUPERIOR_HOST_PACKED=1']
        if curve < 0: defines += ['DLSSNR_SUPERIOR_HOST_SDR=1']
        compile_shader(source, 'base_'+suffix, defines)
        shipped_name = 'host_resolve_' + ('packed_' if packed else '') + ('sdr' if curve < 0 else 'hdr') + '_Shader.cso'
        blob = subprocess.check_output(['git', 'show', '0fa378f4:OptiScaler/shaders/dlssnr/superior/precompile/' + shipped_name], cwd=root)
        assert (out / ('base_' + suffix + '_Shader.cso')).read_bytes() == blob, 'Baseline compiler/bytecode changed'
        fixed = '\n#define gPassthrough '+('1u' if curve<0 else '0u')+'\n'
        fixed += '#define gReversibleMode '+str(max(curve,0))+'u\n'
        fixed += '#define gDebugView 0u\n#define gCompareMode 0u\n#define gApplyModel 1u\n'
        specialized = source.replace('// Bringing an impossible colour', fixed+'\n// Bringing an impossible colour')
        compile_shader(specialized, 'fixed_'+suffix, defines)
        if packed:
            table_fixed = specialized.replace('"estimator.hlsli"','"cached.hlsli"').replace(
                'const uint lane = groupThreadId.y * 8u + groupThreadId.x;',
                'const uint lane = groupThreadId.y * 8u + groupThreadId.x;\n    sPixel=id.xy;')
            compile_shader(table_fixed, 'cached_fixed_'+suffix, defines)
        if curve in (-1,0):
            tile = source.replace('"estimator.hlsli"','"lds.hlsli"').replace(
                'const uint lane = groupThreadId.y * 8u + groupThreadId.x;',
                'const uint lane = groupThreadId.y * 8u + groupThreadId.x;\n    PrepareSuperiorTile(groupId.xy,lane);')
            compile_shader(tile, 'lds_'+suffix, defines)
            if packed:
                table = source.replace('"estimator.hlsli"','"cached.hlsli"').replace(
                    'const uint lane = groupThreadId.y * 8u + groupThreadId.x;',
                    'const uint lane = groupThreadId.y * 8u + groupThreadId.x;\n    sPixel=id.xy;')
                compile_shader(table, 'cached_'+suffix, defines)

# Same FP32 shader evaluation as the consumer: include both UV divisions and the precise native product.
builder = source[:source.index('// Bringing an impossible colour')]
builder += '''
Texture2D<float4> gMotion : register(t3);
RWTexture2D<float4> gTarget : register(u0);
'''+estimator[estimator.index('// Immutable warp'):estimator.index('float3 SuperiorShapeEdit')].replace('#endif','')
builder += '''
[numthreads(8,8,1)]
void CSMain(uint3 id:SV_DispatchThreadID)
{
    if(id.y==0u && id.x<11u) gTarget[id.xy]=gMotion.Load(int3(id.xy,0));
    if(id.y==1u && id.x<gWidth)
        gTarget[id.xy]=SuperiorPackedUv(float2((float(id.x)+0.5)/float(gWidth),0)).xxxx;
    if(id.y==2u && id.x<gHeight)
        gTarget[id.xy]=SuperiorPackedUv(float2(0,(float(id.x)+0.5)/float(gHeight))).yyyy;
}
'''
compile_shader(builder, 'cache_build')

# Fusion experiment has a separate root signature with three UAVs; other APIs and legacy PSOs are untouched.
spatial = (host / 'dlssnr_spatial.hlsl').read_text()
spatial = spatial.replace('#ifdef SPATIAL_GUIDES', '#if defined(SPATIAL_GUIDES) || defined(SPATIAL_FUSED)')
spatial = spatial.replace('#include "dlssnr_spatial_warp.hlsli"',
    'RWTexture2D<float4> gColour : register(u2);\n#include "dlssnr_spatial_warp.hlsli"')
spatial = spatial.replace('    float2 nativeUv = nativePixel / gNativeWorkSize.xy;\n    int2 depthLo',
    '    gColour[id.xy] = SamplePackedColour(nativePixel,pixel);\n    float2 nativeUv = nativePixel / gNativeWorkSize.xy;\n    int2 depthLo')
compile_shader(spatial, 'fused_pack', ['SPATIAL_FUSED=1'])
compile_shader(spatial.replace('float2 nativeUv = nativePixel / gNativeWorkSize.xy;',
                              'precise float2 nativeUv = nativePixel / gNativeWorkSize.xy;'),
               'fused_uv', ['SPATIAL_FUSED=1'])
compile_shader(spatial.replace('float2 nativePixel = PwUnpack(pixel);',
                              'precise float2 nativePixel = PwUnpack(pixel);').replace(
                              'float2 nativeUv = nativePixel / gNativeWorkSize.xy;',
                              'precise float2 nativeUv = nativePixel / gNativeWorkSize.xy;'),
               'fused_precise', ['SPATIAL_FUSED=1'])
compile_shader(spatial.replace('    gColour[id.xy] = SamplePackedColour(nativePixel,pixel);', '').replace(
                              '    gTarget1[id.xy] = motionPacked;',
                              '    gTarget1[id.xy] = motionPacked;\n    gColour[id.xy] = SamplePackedColour(nativePixel,pixel);'),
               'fused_order', ['SPATIAL_FUSED=1'])
print('Four candidate families compiled')
