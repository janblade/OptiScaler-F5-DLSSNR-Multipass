"""Compile legacy and independently extracted v59-estimator reference shaders."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
host = root / 'OptiScaler/shaders/dlssnr/precompile'
out = root / 'artifacts/validation/host-shaders'
out.mkdir(parents=True, exist_ok=True)
dxc = root / 'OptiScaler/shaders/shader_tools/dxc.exe'
source = (host / 'dlssnr.hlsl').read_text(encoding='utf-8')
frozen = (host / '../superior/precompile/dlssnr.hlsl').read_text(encoding='utf-8')
begin = frozen.index('float3 P100GuidedResidualAt(')
end = frozen.index('\n\n#if !defined(VK_MODE)', begin)
reference = frozen[begin:end]
reference = reference.replace('P100GuidedResidualAt(', 'SuperiorGuidedEdit(')
reference = reference.replace('GuidedResidualBilinear(uvq)', 'gModel.SampleLevel(gLinear, saturate(uvq), 0).rgb')
reference = reference.replace('gResidualBlendUnused', '0.015').replace('gResidualScale', '3.0')
reference = reference.replace('gTransfer == 9u', 'true')
reference = reference.replace('return InterPassGuideBlend(bilinearResidual, guided, saturate(gResidualConfidenceUnused));',
    'return gGuideWidth != 0u ? gMvScaleX * guided + (gMvScaleY - gMvScaleX) * gSuperiorLow.SampleLevel(gLinear, uvq, 0).rgb : guided;')
reference = 'Texture2D<float4> gSuperiorLow : register(t5);\n' + reference
reference_source = source.replace('#include "../superior/HostResolve.hlsli"', reference)
(out / 'reference.hlsl').write_text(reference_source, encoding='utf-8')

def compile_shader(path, name, defines=()):
    subprocess.run([str(dxc), '-T', 'cs_6_0', '-E', 'CSMain', '-O3', '-Qstrip_debug', '-Qstrip_reflect',
                    '-I', str(host), *[arg for d in defines for arg in ('-D', d)], str(path),
                    '-Fo', str(out / (name + '_Shader.cso'))], check=True)

compile_shader(host / 'dlssnr.hlsl', 'legacy_recompiled')
upstream = subprocess.check_output(['git', 'show', '2a5be38d:OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl'], cwd=root)
(out / 'upstream.hlsl').write_bytes(upstream)
compile_shader(out / 'upstream.hlsl', 'upstream_recompiled')
# The production legacy bytecode is preserved, independently of compilation flags
# used for the new Superior-only PSOs.
original_blob = subprocess.check_output(['git', 'show', '2a5be38d:OptiScaler/shaders/dlssnr/precompile/DlssNr_Shader.cso'], cwd=root)
assert original_blob == (host / 'DlssNr_Shader.cso').read_bytes()
for variant in ('hdr', 'sdr'):
    defines = ['DLSSNR_SUPERIOR_HOST_RESOLVE=1', 'DLSSNR_SUPERIOR_RESOLVE=1', 'DLSSNR_SUPERIOR_NORMAL=1']
    if variant == 'sdr':
        defines += ['DLSSNR_SUPERIOR_SDR=1', 'DLSSNR_SUPERIOR_HOST_SDR=1']
    compile_shader(out / 'reference.hlsl', 'reference_' + variant, defines)
print('Host reference shaders compiled')
