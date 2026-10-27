# Rebuild only the optional direct PQ encoder. The original Finished Picture blob is unchanged.
param([switch]$Check)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dir = Join-Path $root 'OptiScaler/shaders/dlssnr/precompile'
$temp = Join-Path ([IO.Path]::GetTempPath()) ('nr-pq-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
$stem = 'dlssnr_finished_pq_direct_Shader'
try {
    & (Join-Path $root 'OptiScaler/shaders/shader_tools/dxc.exe') -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect `
        (Join-Path $dir 'dlssnr_finished_pq_direct.hlsl') -Fo (Join-Path $temp "$stem.cso") `
        -Fh (Join-Path $temp "$stem.h") -Vn dlssnr_finished_pq_direct_cso
    if ($LASTEXITCODE -ne 0) { throw 'PQ encoder compilation failed.' }
    $header = Join-Path $temp "$stem.h"
    [string[]]$lines = [IO.File]::ReadAllLines($header) | ForEach-Object { $_.TrimEnd() }
    [IO.File]::WriteAllLines($header, $lines, [Text.UTF8Encoding]::new($false))
    foreach ($ext in @('cso', 'h')) {
        $built = Join-Path $temp "$stem.$ext"
        $target = Join-Path $dir "$stem.$ext"
        if ($Check) {
            if (!(Test-Path $target) -or (Get-FileHash $built).Hash -ne (Get-FileHash $target).Hash) {
                throw "Stale PQ encoder: $target"
            }
        } else { Copy-Item -LiteralPath $built -Destination $target }
    }
} finally {
    # Only the two explicitly named outputs are removed; never recurse through a computed path.
    foreach ($ext in @('cso', 'h')) {
        $file = Join-Path $temp "$stem.$ext"
        if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force }
    }
    Remove-Item -LiteralPath $temp -Force
}
