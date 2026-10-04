<#
.SYNOPSIS
    Sets up DLSS-NR in a game that has no DLSS of its own, starting from this build: our release zip, our
    nvngx_dlssnr.dll and our settings, with DLSS5-Feeder doing the part only it can do.

.DESCRIPTION
    A game with no DLSS, FSR or XeSS never gives OptiScaler the depth and motion DLSS-NR needs. DLSS5-Feeder
    (a ReShade add-on, MIT licence, jlrouzies-fr/DLSS5-Feeder) works them out and hands OptiScaler a normal DLSS
    request; OptiScaler then upscales and runs the neural pass. Feeder also covers 32-bit games, Direct3D 9 to 12,
    OpenGL and Vulkan, through a 64-bit helper process. See docs/NON-DLSS-GAMES.md.

    Feeder ships a one-command installer. This script is the same thing seen from our side: you start from OUR zip,
    and it
      1. finds our release zip (or builds one from the folder this script sits in),
      2. checks it is complete and finds your nvngx_dlssnr.dll,
      3. fetches Feeder's installer, pinned to the exact version this was written against and checked against its
         SHA256 before anything runs it,
      4. runs it with OUR zip as the OptiScaler it installs (it installs ReShade, Feeder, a motion provider and the
         NGX runtimes itself, and puts OptiScaler in as winmm.dll or version.dll, in host64\ for a 32-bit game),
      5. checks our side afterwards: the ini keys and the model.
    Run it again with -CheckOnly after the first launch to read the logs.

    Nothing is changed until you have seen the plan and said yes (-Plan stops after showing it).

    Experimental: Feeder officially supports two other DLSS-NR forks. This build presents the same fingerprints
    and has not been run through the installer end to end.

.PARAMETER GameExe
    The game's real executable (or its folder).

.PARAMETER OurZip
    Our release zip (OptiScaler-DLSSNR-F5-<version>.zip). Default: one beside this script or in its parent folder,
    else a zip built from the release folder this script sits in.

.PARAMETER ModelDll
    Your nvngx_dlssnr.dll. Default: one beside this script or in the game folder. With none, Feeder's installer
    downloads its own, which only runs on RTX 50 cards.

.PARAMETER FeederInstaller
    A copy of Install-DLSS5Feeder.ps1 you have already read, used instead of the download. Its hash is shown; a
    version other than the pinned one is only used after you confirm (or with -AllowUnpinnedInstaller).

.PARAMETER Api
    Override render-API detection: D3D, Vulkan, OpenGL, D3D9 or D3D8. Default: Auto.

.PARAMETER Downloads
    Cache for downloads (passed to Feeder's installer and used for its own file).

.PARAMETER Plan
    Show what would happen, change nothing, download nothing.

.PARAMETER CheckOnly
    Do not install; read the logs of an installed game and say whether the neural pass ran.

.PARAMETER Yes
    Skip OUR confirmation and pass -Yes to Feeder's installer (which then answers yes to everything, including its
    Windows Defender exclusion prompt). For unattended use only.

.PARAMETER NoVerify, NoPause
    Passed to Feeder's installer.

.EXAMPLE
    .\Install-NonDlssGame.ps1 "G:\Games\Dusk\Dusk.exe" -ModelDll D:\models\nvngx_dlssnr.dll -Plan

.EXAMPLE
    .\Install-NonDlssGame.ps1 "D:\Games\Fable Anniversary\Binaries\Win32\Fable Anniversary.exe" -CheckOnly

.NOTES
    Windows PowerShell 5.1 compatible. If Windows refuses to run it:
      powershell -ExecutionPolicy Bypass -File .\Install-NonDlssGame.ps1 "<path to game.exe>"
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string] $GameExe,

    [string] $OurZip,
    [string] $ModelDll,
    [string] $FeederInstaller,

    [ValidateSet('Auto', 'D3D', 'Vulkan', 'OpenGL', 'D3D9', 'D3D8')]
    [string] $Api = 'Auto',

    [string] $Downloads,

    [switch] $Plan,
    [switch] $CheckOnly,
    [switch] $AllowUnpinnedInstaller,
    [switch] $Yes,
    [switch] $NoVerify,
    [switch] $NoPause
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# ---------------------------------------------------------------------------------------------------------
# The Feeder installer this was written against: commit 516f86c (1.17.0, main, 2026-09-28), read before it was
# pinned. A newer upstream (1.18.0-beta.1) exists and has not been read. Bump all three together, after reading it.
# ---------------------------------------------------------------------------------------------------------
$Feeder = @{
    Commit = '516f86cfe4162318e21167be2dd54fa10eeed6f2'
    Sha256 = '3081c147777f6199a4d868a17b557b3fa2bc6a6fc0923f137a6d26f96a35ce8d'
    Url    = 'https://raw.githubusercontent.com/jlrouzies-fr/DLSS5-Feeder/516f86cfe4162318e21167be2dd54fa10eeed6f2/tools/Install-DLSS5Feeder.ps1'
}

function Say([string]$Status, [string]$Text, [string]$Detail = '')
{
    $colour = switch ($Status) { 'Ok' { 'Green' } 'Warn' { 'Yellow' } 'Fail' { 'Red' } 'Info' { 'Gray' } default { 'White' } }
    Write-Host ('  [{0,-4}] {1}' -f $Status, $Text) -ForegroundColor $colour
    if ($Detail) { Write-Host ('         ' + $Detail) -ForegroundColor DarkGray }
}

function Stop-Here([string]$Text, [string]$Detail = '')
{
    Say 'Fail' $Text $Detail
    if (-not $NoPause -and [Environment]::UserInteractive -and -not $Plan) { Read-Host 'Press Enter to exit' | Out-Null }
    exit 1
}

function Get-Sha256([string]$Path) { return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant() }

# 32 or 64, from the PE header's Machine field; $null when it cannot be read.
function Get-PeBits([string]$Path)
{
    try
    {
        $fs = [IO.File]::OpenRead($Path)
        try
        {
            $b = New-Object byte[] 4096
            $n = $fs.Read($b, 0, $b.Length)
            if ($n -lt 0x40 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { return $null }
            $pe = [BitConverter]::ToInt32($b, 0x3C)
            if ($pe -lt 0 -or $pe + 6 -gt $n) { return $null }
            $machine = [BitConverter]::ToUInt16($b, $pe + 4)
            if ($machine -eq 0x8664 -or $machine -eq 0xAA64) { return 64 }
            if ($machine -eq 0x014C) { return 32 }
            return $null
        }
        finally { $fs.Dispose() }
    }
    catch { return $null }
}

function Resolve-GameExe([string]$Path)
{
    if (-not $Path) { Stop-Here 'No game given.' 'Pass the game''s .exe (or its folder) as the first argument.' }
    $full = (Resolve-Path -LiteralPath $Path -ErrorAction SilentlyContinue)
    if (-not $full) { Stop-Here ('Not found: ' + $Path) }
    $full = $full.ProviderPath
    if (Test-Path -LiteralPath $full -PathType Container)
    {
        $skip = '(?i)unins|setup|launcher|crash|redist|vc_redist|dxsetup|helper|updater'
        $exe = Get-ChildItem -LiteralPath $full -Filter *.exe -File | Where-Object { $_.Name -notmatch $skip } |
               Sort-Object Length -Descending | Select-Object -First 1
        if (-not $exe) { Stop-Here ('No game .exe found in ' + $full) }
        return $exe.FullName
    }
    return $full
}

# ---- The game ------------------------------------------------------------------------------------------------

Write-Host ''
Write-Host 'DLSS-NR for a game with no DLSS (our build, through DLSS5-Feeder)' -ForegroundColor White
Write-Host ''

$exe = Resolve-GameExe $GameExe
$gameDir = Split-Path -Parent $exe
$bits = Get-PeBits $exe
if ($null -eq $bits) { Say 'Warn' 'Could not read the exe''s architecture; assuming 64-bit.'; $bits = 64 }
$consumerDir = if ($bits -eq 32) { Join-Path $gameDir 'host64' } else { $gameDir }

Say 'Info' ('Game: ' + $exe)
Say 'Info' ('Architecture: ' + $bits + '-bit' + $(if ($bits -eq 32) { '  (OptiScaler and the model go in host64\, run by a 64-bit helper)' } else { '' }))

# ---- CheckOnly: read the logs ---------------------------------------------------------------------------------

if ($CheckOnly)
{
    $feedLog = if ($bits -eq 32) { Join-Path $consumerDir 'dlss5-feed-host.log' } else { Join-Path $gameDir 'dlss5-feed.log' }
    $optiLog = Join-Path $consumerDir 'OptiScaler.log'
    $problems = 0

    if (-not (Test-Path -LiteralPath $feedLog))
    {
        Say 'Fail' ('No Feeder log yet: ' + $feedLog) 'Start the game once and play for a minute, then run this again.'
        $problems++
    }
    else
    {
        $text = Get-Content -LiteralPath $feedLog -Raw
        if ($text -match 'routed through OptiScaler DLSS-NR') { Say 'Ok' 'Feeder''s NGX calls reach OptiScaler.' }
        else { Say 'Fail' 'Feeder''s calls did not reach OptiScaler.' 'OptiScaler is not loaded in the process: check its DLL name (winmm.dll or version.dll) and that no other neural add-on is beside it.'; $problems++ }

        if ($text -match 'neural model \(feature 18\) loaded') { Say 'Ok' 'The neural model (feature 18) loaded.' }
        elseif ($text -match 'neural model \(feature 18\) NOT loaded') { Say 'Fail' 'The neural model never loaded.' 'Needs [DlssNr] Enabled=true and nvngx_dlssnr.dll beside OptiScaler; OptiScaler.log says why.'; $problems++ }
        else { Say 'Warn' 'No verdict on the neural model yet.' 'It is written after the first evaluates; play a little longer.' }
    }

    if (Test-Path -LiteralPath $optiLog)
    {
        $o = Get-Content -LiteralPath $optiLog -Tail 4000 | Out-String
        if ($o -match '(?i)DLSS-NR') { Say 'Ok' 'OptiScaler.log mentions DLSS-NR.' } else { Say 'Warn' 'OptiScaler.log has no DLSS-NR line.' ($optiLog) }
    }
    else { Say 'Warn' ('No OptiScaler.log: ' + $optiLog) 'OptiScaler never loaded, or logging is off.' }

    exit $(if ($problems -gt 0) { 1 } else { 0 })
}

# ---- Our zip ---------------------------------------------------------------------------------------------------

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$builtZip = $null

if (-not $OurZip)
{
    $found = @($here, (Split-Path -Parent $here)) | Where-Object { $_ } | ForEach-Object {
        Get-ChildItem -LiteralPath $_ -Filter 'OptiScaler-DLSSNR-F5-*.zip' -File -ErrorAction SilentlyContinue } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($found) { $OurZip = $found.FullName }
}

if (-not $OurZip)
{
    # Run from an extracted release: build the zip Feeder's installer wants from the folder.
    $root = $here
    if (-not (Test-Path -LiteralPath (Join-Path $root 'OptiScaler.dll'))) { $root = Split-Path -Parent $here }
    if (Test-Path -LiteralPath (Join-Path $root 'OptiScaler.dll'))
    {
        $cache = if ($Downloads) { $Downloads } else { Join-Path $env:LOCALAPPDATA 'OptiScaler-NR\build' }
        $builtZip = Join-Path $cache 'OptiScaler-DLSSNR-F5-local.zip'
        $OurZip = $builtZip
    }
}

if (-not $OurZip) { Stop-Here 'Our release zip was not found.' 'Pass it with -OurZip, or run this from beside it or from the extracted release folder.' }

if (-not $builtZip)
{
    if (-not (Test-Path -LiteralPath $OurZip)) { Stop-Here ('Zip not found: ' + $OurZip) }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $z = [IO.Compression.ZipFile]::OpenRead($OurZip)
    try
    {
        $names = @($z.Entries | ForEach-Object { $_.FullName -replace '\\', '/' })
        $need = @('OptiScaler.dll', 'OptiScaler.ini', 'nvngx.dll_dlssnr.dll')
        $missing = @($need | Where-Object { $names -notcontains $_ })
        if ($missing.Count -gt 0) { Stop-Here ('That zip is not a complete release: missing ' + ($missing -join ', ') + '.') }
        if (-not ($names | Where-Object { $_ -like 'OptiScaler/*' })) { Say 'Warn' 'The zip has no OptiScaler\ runtime folder.' }
    }
    finally { $z.Dispose() }
    Say 'Ok' ('Our zip: ' + $OurZip) ('SHA256 ' + (Get-Sha256 $OurZip))
}
else { Say 'Info' ('Our zip: built from ' + $root + ' when the install runs') ($builtZip) }

# ---- The model -------------------------------------------------------------------------------------------------

if (-not $ModelDll)
{
    foreach ($d in @($here, $gameDir, $consumerDir))
    {
        $p = Join-Path $d 'nvngx_dlssnr.dll'
        if (Test-Path -LiteralPath $p) { $ModelDll = $p; break }
    }
}

if ($ModelDll)
{
    if (-not (Test-Path -LiteralPath $ModelDll)) { Stop-Here ('Model not found: ' + $ModelDll) }
    $mi = Get-Item -LiteralPath $ModelDll
    Say 'Ok' ('Model: ' + $ModelDll) ('{0:N0} MB' -f ($mi.Length / 1MB))
    if ($mi.Length -lt 50MB) { Say 'Warn' 'That file is small for a DLSS-NR model.' 'Make sure it is nvngx_dlssnr.dll and not a forwarder.' }
}
else
{
    Say 'Warn' 'No nvngx_dlssnr.dll found.' 'Feeder''s installer will download its own, which only runs on RTX 50 cards. On RTX 20, 30 or 40 pass your own with -ModelDll.'
}

# ---- Feeder's installer ----------------------------------------------------------------------------------------

$cacheDir = if ($Downloads) { $Downloads } else { Join-Path $env:LOCALAPPDATA 'OptiScaler-NR\feeder' }
$installerPath = $null
$installerNote = ''

if ($FeederInstaller)
{
    if (-not (Test-Path -LiteralPath $FeederInstaller)) { Stop-Here ('Installer not found: ' + $FeederInstaller) }
    $installerPath = (Resolve-Path -LiteralPath $FeederInstaller).ProviderPath
    $h = Get-Sha256 $installerPath
    if ($h -eq $Feeder.Sha256) { $installerNote = 'matches the pinned version' }
    else
    {
        $installerNote = 'NOT the pinned version (' + $h + ')'
        if (-not $AllowUnpinnedInstaller -and -not $Plan)
        {
            Say 'Warn' 'That installer is not the version this script was written against.' ('Pinned ' + $Feeder.Sha256 + '; yours ' + $h + '. It may behave differently with our build.')
            if ($Yes -or -not [Environment]::UserInteractive) { Stop-Here 'Refusing an unpinned installer without -AllowUnpinnedInstaller.' }
            if ((Read-Host '  Use it anyway? (y/N)') -notmatch '^(y|yes)$') { Stop-Here 'Stopped.' }
        }
    }
}
else
{
    $installerPath = Join-Path (Join-Path $cacheDir $Feeder.Commit.Substring(0, 12)) 'Install-DLSS5Feeder.ps1'
    $installerNote = 'downloaded from the pinned commit ' + $Feeder.Commit.Substring(0, 12) + ' and hash-checked'
}

# ---- The plan --------------------------------------------------------------------------------------------------

Write-Host ''
Write-Host 'Plan' -ForegroundColor White
Say 'Info' ('Feeder installer: ' + $installerPath) $installerNote
Say 'Info' ('OptiScaler goes into: ' + $consumerDir + '  (as winmm.dll or version.dll, chosen from the exe''s imports)')
Say 'Info' 'Feeder''s installer also fetches ReShade, DLSS5-Feeder, a motion-vector provider and the NGX runtimes, and for D3D8/D3D9 games dgVoodoo2. Its own README lists them.'
Say 'Info' 'It sets [DlssNr] Enabled=true, Dx12Upscaler=dlss and ScanExposure=false in OptiScaler.ini, and leaves any value you set by hand.'
Say 'Info' 'Existing ReShade and OptiScaler settings in the folder are kept; Feeder backs up what it merges.'

if ($Plan) { Write-Host ''; Say 'Ok' 'Plan only: nothing was downloaded or changed.'; exit 0 }

if (-not $Yes)
{
    if (-not [Environment]::UserInteractive) { Stop-Here 'Not interactive: pass -Yes to go ahead.' }
    if ((Read-Host '  Go ahead? (y/N)') -notmatch '^(y|yes)$') { Say 'Info' 'Stopped; nothing was changed.'; exit 0 }
}

# ---- Fetch and verify -----------------------------------------------------------------------------------------

if (-not $FeederInstaller)
{
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $installerPath) | Out-Null
    $needFetch = -not (Test-Path -LiteralPath $installerPath) -or ((Get-Sha256 $installerPath) -ne $Feeder.Sha256)
    if ($needFetch)
    {
        try
        {
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
            Invoke-WebRequest -UseBasicParsing -Uri $Feeder.Url -OutFile $installerPath
        }
        catch { Stop-Here 'Could not download Feeder''s installer.' ($_.Exception.Message + '  Download it yourself from ' + $Feeder.Url + ' and pass it with -FeederInstaller.') }
    }
    $got = Get-Sha256 $installerPath
    if ($got -ne $Feeder.Sha256)
    {
        Remove-Item -LiteralPath $installerPath -Force -ErrorAction SilentlyContinue
        Stop-Here 'The downloaded installer does not match the pinned hash, so it was deleted and not run.' ('Expected ' + $Feeder.Sha256 + ', got ' + $got + '.')
    }
    Say 'Ok' 'Feeder''s installer downloaded and matches the pinned hash.'
}

if ($builtZip)
{
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $builtZip) | Out-Null
    if (Test-Path -LiteralPath $builtZip) { Remove-Item -LiteralPath $builtZip -Force }
    Say 'Info' 'Building the zip from the release folder (about 125 MB; takes a moment)...'
    $items = Get-ChildItem -LiteralPath $root | Where-Object { $_.Name -notmatch '^Install-NonDlssGame\.ps1$' }
    Compress-Archive -Path ($items | ForEach-Object { $_.FullName }) -DestinationPath $builtZip -CompressionLevel Fastest
}

# ---- Run it ----------------------------------------------------------------------------------------------------

# The Dagherbou profile is the one for a build that ships nvngx.dll_dlssnr.dll: Feeder reads that off the zip itself,
# and it is what makes it set ScanExposure=false. It is a label for Feeder's own handling, not a claim about our code.
$installerArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $installerPath, '-GameExe', $exe,
          '-Consumer', 'OptiScaler', '-OptiScalerFork', 'Dagherbou', '-OptiScalerZip', $OurZip)
if ($ModelDll) { $installerArgs += @('-DlssNrDll', $ModelDll) }
if ($Api -ne 'Auto') { $installerArgs += @('-Api', $Api) }
if ($Downloads) { $installerArgs += @('-Downloads', $Downloads) }
if ($Yes) { $installerArgs += '-Yes' }
if ($NoVerify) { $installerArgs += '-NoVerify' }
$installerArgs += '-NoPause'

Write-Host ''
Write-Host 'Running Feeder''s installer with our zip...' -ForegroundColor White
& powershell.exe @installerArgs
$code = $LASTEXITCODE

Write-Host ''
Write-Host 'Our side' -ForegroundColor White

$opti = $null
foreach ($n in @('winmm.dll', 'version.dll', 'dbghelp.dll', 'winhttp.dll', 'wininet.dll'))
{
    $p = Join-Path $consumerDir $n
    if ((Test-Path -LiteralPath $p) -and (Select-String -LiteralPath $p -Pattern 'OptiScaler.ini' -SimpleMatch -Quiet)) { $opti = $p; break }
}

if ($opti) { Say 'Ok' ('OptiScaler is in place as ' + (Split-Path -Leaf $opti)) $opti }
else { Say 'Fail' 'OptiScaler is not in the folder it should be.' ($consumerDir + ' has none of winmm.dll, version.dll, dbghelp.dll, winhttp.dll, wininet.dll as OptiScaler.') }

$ini = Join-Path $consumerDir 'OptiScaler.ini'
if (Test-Path -LiteralPath $ini)
{
    $t = Get-Content -LiteralPath $ini -Raw
    function Key([string]$Section, [string]$Name)
    {
        $m = [regex]::Match($t, '(?ms)^\[' + [regex]::Escape($Section) + '\](.*?)(?=^\[|\z)')
        if (-not $m.Success) { return $null }
        $k = [regex]::Match($m.Groups[1].Value, '(?m)^' + [regex]::Escape($Name) + '\s*=\s*(\S+)')
        if ($k.Success) { return $k.Groups[1].Value } else { return $null }
    }
    $enabled = Key 'DlssNr' 'Enabled'
    if ($enabled -ieq 'true') { Say 'Ok' '[DlssNr] Enabled=true' } else { Say 'Fail' ('[DlssNr] Enabled is ' + $enabled + '.') 'The neural pass is off until it is true.' }
    $fp = Key 'DlssNr' 'FinishedPicture'
    if ($fp -ieq 'true') { Say 'Warn' '[DlssNr] FinishedPicture=true.' 'It moves the pass to the game''s Present; on the helper path that is a window the game never sees. Set it to false.' }
    $up = Key 'Upscalers' 'Dx12Upscaler'
    if ($up -and $up -ine 'dlss') { Say 'Warn' ('[Upscalers] Dx12Upscaler=' + $up + ' (Feeder''s docs use dlss).') }
}
else { Say 'Warn' 'No OptiScaler.ini in the folder.' }

$modelHere = @($consumerDir, $gameDir) | ForEach-Object { Join-Path $_ 'nvngx_dlssnr.dll' } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if ($modelHere) { Say 'Ok' ('Model present: ' + $modelHere) } else { Say 'Fail' 'nvngx_dlssnr.dll is not in the folder.' 'DLSS-NR cannot run without it.' }

Write-Host ''
Say 'Info' 'Next: start the game, play a minute, then run this script again with -CheckOnly.'
Say 'Info' 'OptiScaler''s menu opens with Insert' ($(if ($bits -eq 32) { 'For a 32-bit game it lives in the helper: ReShade overlay > Add-ons > DLSS 5 Feed > "Show the DLSS 5 panel in-game", then Insert.' } else { '' }))

if (-not $NoPause -and [Environment]::UserInteractive) { Read-Host 'Press Enter to exit' | Out-Null }
exit $code
