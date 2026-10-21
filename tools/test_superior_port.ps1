param(
    [string]$ReferenceShaderDirectory,
    [switch]$GpuQuality,
    [switch]$GpuTiming
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
    $cpu = @('superior_policy', 'interpass_policy', 'nr_guides_smoke', 'nr_surface_formats_smoke')
    $quality = @('superior_carrier', 'superior_prepare_quality', 'superior_input_quality',
                 'superior_exposure_quality', 'superior_softknee_quality', 'superior_tall_low_quality',
                 'interpass_low_cache_quality')
    $names = @($cpu)
    if ($GpuQuality) { $names += $quality }
    if ($GpuTiming) { $names += 'superior_tall_low_timing' }
    foreach ($name in $names) {
        $src = if ($name -like 'nr_*_smoke') { "tests/$name.cpp" } else { "tests/nr_gpu/$name.cpp" }
        cmd /c "`"$vc`" && cl /nologo /std:c++17 /EHsc /O2 `"$src`" /Fe:$out/$name.exe /Fo:$out/$name.obj /link d3d12.lib dxgi.lib"
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $name" }
    }
    foreach ($name in $cpu) {
        & "$out/$name.exe"
        if ($LASTEXITCODE -ne 0) { throw "Test failed: $name" }
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
