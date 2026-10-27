# Rebuild optional direct PQ encoders. The original Finished Picture blob is unchanged.
param([switch]$Check)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dir = Join-Path $root 'OptiScaler/shaders/dlssnr/precompile'
$temp = Join-Path ([IO.Path]::GetTempPath()) ('nr-pq-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
$jobs = @(
    @{ Name = 'pq_direct'; Source = 'pq_direct'; Profile = 'cs_6_0'; Entry = 'CSMain' },
    @{ Name = 'pq_vs'; Source = 'pq_raster'; Profile = 'vs_6_0'; Entry = 'VSMain' },
    @{ Name = 'pq_ps'; Source = 'pq_raster'; Profile = 'ps_6_0'; Entry = 'PSMain' }
)
try {
    foreach ($job in $jobs) {
        $base = "dlssnr_finished_$($job.Name)"
        $stem = "${base}_Shader"
        & (Join-Path $root 'OptiScaler/shaders/shader_tools/dxc.exe') -T $job.Profile -E $job.Entry -O3 -Qstrip_debug -Qstrip_reflect `
            (Join-Path $dir "dlssnr_finished_$($job.Source).hlsl") -Fo (Join-Path $temp "$stem.cso") `
            -Fh (Join-Path $temp "$stem.h") -Vn "${base}_cso"
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
    }
} finally {
    # Only explicitly named outputs are removed; never recurse through a computed path.
    foreach ($job in $jobs) {
        $stem = "dlssnr_finished_$($job.Name)_Shader"
        foreach ($ext in @('cso', 'h')) {
            $file = Join-Path $temp "$stem.$ext"
            if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force }
        }
    }
    Remove-Item -LiteralPath $temp -Force
}
