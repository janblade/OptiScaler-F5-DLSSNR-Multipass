param(
    [string]$ReferenceShaderDirectory,
    [switch]$GpuQuality,
    [switch]$GpuTiming,
    [switch]$GpuHost,
    [switch]$GpuSpatial,
    [switch]$GpuSpatialTiming,
    [switch]$GpuNativeTemporalTiming,
    [switch]$GpuPackedTiming,
    [switch]$GpuHostOptTiming,
    [switch]$GpuCandidates,
    [switch]$GpuCandidateTiming,
    [switch]$GpuNativeCenter,
    [switch]$GpuNativeCenterTiming
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
Push-Location $repo
try {
    python tests/test_nr_superior_scaling.py
    if ($LASTEXITCODE -ne 0) { throw 'Shader identity/ABI test failed' }
    $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'MSVC x64 tools are required' }
    $vc = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
    $out = 'artifacts/validation'
    New-Item -ItemType Directory -Force $out | Out-Null
    $cpu = @('superior_policy', 'spatial_policy', 'interpass_policy', 'nr_guides_smoke', 'nr_surface_formats_smoke')
    $quality = @('superior_carrier', 'superior_prepare_quality', 'superior_input_quality',
                 'superior_exposure_quality', 'superior_softknee_quality', 'superior_tall_low_quality',
                 'interpass_low_cache_quality')
    $names = @($cpu)
    if ($GpuQuality) { $names += $quality }
    if ($GpuTiming) { $names += 'superior_tall_low_timing' }
    if ($GpuSpatial) { $names += @('nr_spatial_dx12_gpu', 'spatial_raw_low_quality', 'packed_prepare_quality') }
    if ($GpuSpatialTiming -or $GpuNativeTemporalTiming) {
        python tools/prepare_superior_host_tests.py --timing-baseline
        if ($LASTEXITCODE -ne 0) { throw 'Spatial timing baseline missing (fetch full history)' }
        $names += 'spatial_timing'
    }
    if ($GpuPackedTiming) { $names += 'packed_stage_timing' }
    if ($GpuHostOptTiming) {
        python tools/prepare_superior_host_tests.py --host-opt-baseline
        if ($LASTEXITCODE -ne 0) { throw 'Host baseline missing (fetch full history)' }
        $names += 'host_opt_timing'
    }
    if ($GpuHost) {
        python tools/prepare_superior_host_tests.py
        if ($LASTEXITCODE -ne 0) { throw 'Host shader reference compilation failed' }
        $names += @('superior_host_quality', 'packed_host_quality')
    }
    if ($GpuNativeCenter -or $GpuNativeCenterTiming) {
        python tools/prepare_native_center_tests.py
        if ($LASTEXITCODE -ne 0) { throw 'Native centre reference/probe compilation failed' }
        $names += 'native_center_mask'
        if ($GpuNativeCenterTiming) { $names += 'native_center_timing' }
    }
    if ($GpuCandidates -or $GpuCandidateTiming) {
        python tools/prepare_superior_candidates.py
        if ($LASTEXITCODE -ne 0) { throw 'Candidate shader compilation failed' }
        $names += @('four_candidates_quality', 'fused_pack_experiment')
        if ($GpuCandidateTiming) { $names += 'four_candidates_timing' }
    }
    foreach ($name in $names) {
        $src = if ($name -like 'nr_*_smoke' -or $name -eq 'nr_spatial_dx12_gpu') { "tests/$name.cpp" } else { "tests/nr_gpu/$name.cpp" }
        cmd /c "`"$vc`" && cl /nologo /std:c++17 /EHsc /O2 `"$src`" /Fe:$out/$name.exe /Fo:$out/$name.obj /link d3d12.lib dxgi.lib"
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $name" }
    }
    foreach ($name in $cpu) {
        & "$out/$name.exe"
        if ($LASTEXITCODE -ne 0) { throw "Test failed: $name" }
    }
    if ($GpuCandidates -or $GpuCandidateTiming) {
        & "$out/four_candidates_quality.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Candidate bit-exact comparison failed' }
        if ($GpuCandidateTiming) {
            foreach ($family in @('fixed', 'cached_fixed', 'lds', 'final')) {
                & "$out/four_candidates_timing.exe" $family
                if ($LASTEXITCODE -ne 0) { throw "Candidate timing/4K quality failed: $family" }
            }
            & "$out/fused_pack_experiment.exe" timing
        } else {
            & "$out/fused_pack_experiment.exe"
        }
        # Fusion is an intentionally retained rejected experiment, never a production path.
        if ($LASTEXITCODE -ne 2) { throw 'Fused experiment result changed: review the recorded rejection' }
        $global:LASTEXITCODE = 0
    }
    if ($GpuSpatial) {
        & "$out/nr_spatial_dx12_gpu.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Spatial GPU validation failed' }
        & "$out/spatial_raw_low_quality.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Spatial raw low GPU comparison failed' }
        & "$out/packed_prepare_quality.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Packed carrier/guide comparison failed' }
    }
    if ($GpuNativeCenter -or $GpuNativeCenterTiming) {
        & "$out/native_center_mask.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Native centre identity/transition mask failed' }
    }
    if ($GpuHost) {
        & "$out/superior_host_quality.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Host composition comparison failed' }
        & "$out/packed_host_quality.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Packed host composition comparison failed' }
    }
    if ($GpuNativeCenterTiming) {
        & "$out/native_center_timing.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Native centre GPU timing failed' }
    }
    if ($GpuSpatialTiming) {
        & "$out/spatial_timing.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Spatial GPU timing failed' }
    }
    if ($GpuNativeTemporalTiming) {
        & "$out/spatial_timing.exe" --native-temporal
        if ($LASTEXITCODE -ne 0) { throw 'Native temporal stage timing failed' }
    }
    if ($GpuPackedTiming) {
        & "$out/packed_stage_timing.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Packed route GPU timing failed' }
    }
    if ($GpuHostOptTiming) {
        & "$out/host_opt_timing.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Host optimization GPU timing failed' }
    }
    $candidate = 'OptiScaler/shaders/dlssnr/superior/precompile'
    if ($GpuQuality -or $GpuTiming) {
        if (!$ReferenceShaderDirectory -or !(Test-Path -LiteralPath $ReferenceShaderDirectory -PathType Container)) {
            throw 'Supply -ReferenceShaderDirectory pointing to the original v59 precompile directory'
        }
    }
    if ($GpuQuality) {
        foreach ($name in $quality) {
            if ($name -eq 'superior_carrier') { & "$out/$name.exe" $candidate }
            else { & "$out/$name.exe" $ReferenceShaderDirectory $candidate }
            if ($LASTEXITCODE -ne 0) { throw "GPU comparison failed: $name" }
        }
    }
    if ($GpuTiming) {
        & "$out/superior_tall_low_timing.exe" $ReferenceShaderDirectory $candidate "$out/port-v59-timing.csv" normal full
        if ($LASTEXITCODE -ne 0) { throw 'GPU timing failed' }
    }
}
finally { Pop-Location }
