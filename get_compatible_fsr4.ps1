# Install the GPU-appropriate precompiled FSR 4 shader override from the package's pinned manifest.
#
# This script never edits OptiScaler.ini, never enables frame generation, never changes a game executable,
# and never replaces an unfamiliar DLL. It downloads only the four pinned ZIPs from the upstream release,
# verifies the archive and the single DLL it extracts by SHA-256, keeps a .orig backup, and writes a small
# marker so -Restore can put the backup back.
#
# Upstream (this fork of the FSR 4 override): https://github.com/zgauthier2000/fsr4-shader-performance-overrides
# The upstream DLLs are modified AMD DLLs, are not Authenticode-signed, and are for single-player games only.
# SHA-256 against the pinned manifest is the only integrity check here - do not disable antivirus to use them.
param(
    # Defaults to the folder containing this script, which is the extracted package root.
    [string]$Destination = $PSScriptRoot,

    [ValidateSet('exact', 'lossy', 'lite', 'lite-lossy')]
    [string]$Variant = 'exact',

    [ValidateSet('auto', 'rx7000', 'rx6000', 'igpu-rdna3', 'igpu-rdna2', 'rdna4')]
    [string]$GpuFamily = 'auto',

    [string]$ArchivePath,
    [switch]$Restore,

    # Skips the interactive Y/Enter confirmation for unattended installs. The risks are still printed.
    [switch]$AcceptThirdPartyDll
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$manifestPath = Join-Path $PSScriptRoot 'redist/fsr4/manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Missing pinned manifest: $manifestPath. Use a package that ships redist/fsr4/manifest.json."
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json

function Get-Sha256([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Assert-Sha256([string]$Path, [string]$Expected, [string]$What) {
    $actual = Get-Sha256 $Path
    $wanted = $Expected.ToUpperInvariant()
    if ($actual -ne $wanted) {
        throw "$What SHA-256 mismatch for $Path`nexpected: $wanted`nactual:   $actual`nNothing was installed. Keep this file for inspection and use a fresh download."
    }
}

function Assert-PlainFile([string]$Path, [string]$Role) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer) { throw "$Role is a directory, expected a file: $Path" }
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Role is a symlink or junction; refusing to touch it: $Path"
    }
}

function Get-Archive([string]$Family) {
    $property = $manifest.archives.PSObject.Properties[$Family]
    if (-not $property) { throw "The manifest has no archive for GPU family '$Family'." }
    return $property.Value
}

function Get-Variant([string]$Name) {
    $property = $manifest.variants.PSObject.Properties[$Name]
    if (-not $property) { throw "The manifest has no variant '$Name'." }
    return $property.Value
}

function Get-VariantHash($Archive, [string]$Name) {
    $property = $Archive.variants.PSObject.Properties[$Name]
    if (-not $property) { throw "Archive $($Archive.file) has no hash for variant '$Name'." }
    return $property.Value
}

function Write-Risks {
    Write-Host ''
    Write-Host 'FSR 4 shader performance override - IMPORTANT' -ForegroundColor Yellow
    Write-Host "Upstream project: $($manifest.upstream.projectUrl)"
    Write-Host "Pinned release:   $($manifest.upstream.releaseUrl)"
    Write-Host "Licence/notice:   $($manifest.upstream.licenseUrl)"
    Write-Host ''
    Write-Host '* SINGLE-PLAYER GAMES ONLY. Never use in online games or games with anti-cheat:' -ForegroundColor Yellow
    Write-Host '  these are modified game binaries and anti-cheat can treat them as tampering.' -ForegroundColor Yellow
    Write-Host '* NOT FOR RX 9000 / RDNA4. The upstream builds do not support those cards.' -ForegroundColor Yellow
    Write-Host '* These DLLs are NOT Authenticode-signed. Integrity is checked only against' -ForegroundColor Yellow
    Write-Host "  the SHA-256 hashes pinned in $manifestPath." -ForegroundColor Yellow
    Write-Host '*-lossy changes the picture; read the upstream WARNING.txt and measure before/after.' -ForegroundColor Yellow
    Write-Host '* A game or OptiScaler update may restore the original DLL. Re-run this script after updates.' -ForegroundColor Yellow
    Write-Host '* OptiScaler.ini is not changed. If Fsr4Model or Fsr4GpuSupport was forced, set both back to auto yourself.' -ForegroundColor Yellow
    Write-Host ''
}

function Get-FamilyFromAdapter($Adapter, $Rules) {
    $deviceId = ''
    if ($Adapter.PNPDeviceID -match 'DEV_([0-9A-Fa-f]{4})') { $deviceId = $Matches[1].ToUpperInvariant() }
    $name = [string]$Adapter.Name

    foreach ($family in @('rdna4', 'rx7000', 'rx6000', 'igpu-rdna3', 'igpu-rdna2')) {
        $ids = $Rules.deviceIds.PSObject.Properties[$family]
        if ($ids -and $ids.Value -and $ids.Value -contains $deviceId) { return $family }
    }
    foreach ($family in @('rdna4', 'rx7000', 'rx6000', 'igpu-rdna3', 'igpu-rdna2')) {
        $patterns = $Rules.namePatterns.PSObject.Properties[$family]
        if (-not $patterns -or -not $patterns.Value) { continue }
        foreach ($pattern in $patterns.Value) {
            if ($name -match $pattern) { return $family }
        }
    }
    return $null
}

function Resolve-GpuFamily([string]$Override) {
    if ($Override -eq 'rdna4') {
        throw 'RX 9000 / RDNA4 is not supported by these upstream DLLs. Nothing was installed.'
    }
    if ($Override -ne 'auto') {
        return [pscustomobject]@{ Family = $Override; AdapterName = 'forced by -GpuFamily'; Source = '-GpuFamily' }
    }

    $adapters = @()
    try { $adapters = @(Get-CimInstance Win32_VideoController -ErrorAction Stop) }
    catch {
        if (Get-Command Get-WmiObject -ErrorAction SilentlyContinue) {
            $adapters = @(Get-WmiObject Win32_VideoController -ErrorAction Stop)
        }
    }
    if ($adapters.Count -eq 0) {
        throw 'No GPU was reported by Windows. Pass -GpuFamily rx7000|rx6000|igpu-rdna3|igpu-rdna2.'
    }

    $candidates = @()
    foreach ($adapter in $adapters) {
        $name = [string]$adapter.Name
        if ($name -match 'Microsoft Basic Render|Remote Desktop|Virtual') { continue }
        $family = Get-FamilyFromAdapter $adapter $manifest.gpuRules
        if ($family) {
            $candidates += [pscustomobject]@{ Adapter = $adapter; Family = $family; Name = $name }
        }
    }

    # A desktop card wins when a laptop/APU also reports an adapter. The upstream archives are per GPU class.
    foreach ($group in @(@('rx7000', 'rx6000'), @('igpu-rdna3', 'igpu-rdna2'))) {
        $matches = @($candidates | Where-Object { $group -contains $_.Family })
        if ($matches.Count -gt 0) {
            if ($matches.Count -gt 1) {
                Write-Warning "Several adapters matched: $(($matches | ForEach-Object { $_.Name }) -join '; '). Using $($matches[0].Name). Pass -GpuFamily to choose another."
            }
            return [pscustomobject]@{ Family = $matches[0].Family; AdapterName = $matches[0].Name; Source = 'detected' }
        }
    }

    $rdna4 = @($candidates | Where-Object { $_.Family -eq 'rdna4' })
    if ($rdna4.Count -gt 0) {
        throw "Detected $($rdna4[0].Name): RX 9000 / RDNA4 is not supported by these upstream DLLs. Nothing was installed."
    }

    $unknownAmd = @($adapters | Where-Object {
            ([string]$_.PNPDeviceID -match 'VEN_1002') -or ([string]$_.Name -match 'AMD|Radeon')
        })
    if ($unknownAmd.Count -gt 0) {
        throw "Could not classify AMD adapter '$($unknownAmd[0].Name)' ($($unknownAmd[0].PNPDeviceID)). Pass -GpuFamily rx7000|rx6000|igpu-rdna3|igpu-rdna2."
    }

    Write-Warning 'No AMD adapter was detected. Using the upstream igpu-rdna3 fallback (GeForce, Intel Arc and others). Pass -GpuFamily if that is wrong.'
    return [pscustomobject]@{ Family = 'igpu-rdna3'; AdapterName = 'no AMD adapter'; Source = 'upstream fallback' }
}

# ---- Manifest sanity: a broken manifest must fail before touching the game folder ------------------------------------
foreach ($archive in $manifest.archives.PSObject.Properties) {
    if ($archive.Value.sha256 -notmatch '^[0-9A-Fa-f]{64}$') { throw "Bad archive SHA-256 in manifest: $($archive.Name)" }
    foreach ($hash in $archive.Value.variants.PSObject.Properties) {
        if ($hash.Value -notmatch '^[0-9A-Fa-f]{64}$') { throw "Bad DLL SHA-256 in manifest: $($archive.Name)/$($hash.Name)" }
    }
}
foreach ($variantName in $manifest.variants.PSObject.Properties.Name) {
    if (-not (Get-Variant $variantName).entry) { throw "Missing ZIP entry for variant $variantName" }
}

$allowed = @{}
foreach ($original in $manifest.originals) { $allowed[$original.sha256.ToUpperInvariant()] = $original.description }
foreach ($archive in $manifest.archives.PSObject.Properties) {
    foreach ($hash in $archive.Value.variants.PSObject.Properties) {
        $allowed[$hash.Value.ToUpperInvariant()] = "$($archive.Value.file) / $($hash.Name)"
    }
}

# ---- Destination and marker ------------------------------------------------------------------------------------------
$destinationPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Destination)
if (-not (Test-Path -LiteralPath $destinationPath -PathType Container)) {
    throw "Destination is not a folder: $destinationPath"
}
$hostPath = Join-Path $destinationPath $manifest.target.hostFileName
if (-not (Test-Path -LiteralPath $hostPath -PathType Leaf)) {
    throw "Destination must be the game folder containing $($manifest.target.hostFileName): $destinationPath"
}
$relativeDir = $manifest.target.relativeDirectory -replace '/', [IO.Path]::DirectorySeparatorChar
$targetDir = Join-Path $destinationPath $relativeDir
if (-not (Test-Path -LiteralPath $targetDir -PathType Container)) {
    throw "Missing $relativeDir folder under $destinationPath. Extract the whole package first."
}
$targetPath = Join-Path $targetDir $manifest.target.fileName
if (-not (Test-Path -LiteralPath $targetPath -PathType Leaf)) {
    throw "Missing $($manifest.target.fileName) in $targetDir. Extract the whole package first."
}
Assert-PlainFile $targetPath 'The current upscaler DLL'

$backupPath = $targetPath + $manifest.target.backupSuffix
$markerPath = Join-Path $targetDir $manifest.target.markerName
$marker = $null
if (Test-Path -LiteralPath $markerPath) {
    Assert-PlainFile $markerPath 'The override marker'
    try { $marker = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json }
    catch { throw "The override marker $markerPath is not valid JSON. Resolve it manually before running this script." }
    if (-not $marker -or $marker.tool -ne 'get_compatible_fsr4.ps1') {
        throw "Unrecognised marker at $markerPath. Nothing was installed."
    }
}
if (Test-Path -LiteralPath $backupPath) { Assert-PlainFile $backupPath 'The backup DLL' }

$targetHash = Get-Sha256 $targetPath
if (-not $allowed.ContainsKey($targetHash)) {
    throw "Refusing to overwrite an unknown $($manifest.target.fileName) (SHA-256 $targetHash). Back it up and move it aside yourself first."
}
if (Test-Path -LiteralPath $backupPath) {
    $backupHash = Get-Sha256 $backupPath
    if (-not $allowed.ContainsKey($backupHash)) {
        throw "Refusing to use an unknown backup DLL at $backupPath (SHA-256 $backupHash)."
    }
    if ($marker -and $marker.originalSha256 -and $backupHash -ne $marker.originalSha256.ToUpperInvariant()) {
        throw "The backup no longer matches the marker at $markerPath. Restore it manually."
    }
}
if ($marker -and -not (Test-Path -LiteralPath $backupPath)) {
    throw "The marker says an override was installed but its backup is missing: $backupPath. Restore it manually."
}

# ---- Restore ----------------------------------------------------------------------------------------------------------
if ($Restore) {
    if (-not $marker) { throw "No override marker at $markerPath; there is nothing for -Restore to undo." }
    $restoreHash = Get-Sha256 $backupPath
    if (-not $allowed.ContainsKey($restoreHash)) {
        throw "The backup is not a known original DLL (SHA-256 $restoreHash). Refusing to restore."
    }
    $tempRestore = Join-Path $targetDir ("{0}.restore-{1}.tmp" -f $manifest.target.fileName, [Guid]::NewGuid().ToString('N'))
    try {
        [IO.File]::Copy($backupPath, $tempRestore, $false)
        Assert-Sha256 $tempRestore $restoreHash 'Restored DLL'
        Move-Item -LiteralPath $tempRestore -Destination $targetPath -Force
    }
    catch {
        Remove-Item -LiteralPath $tempRestore -ErrorAction SilentlyContinue
        throw
    }
    Assert-Sha256 $targetPath $restoreHash 'Restored DLL'
    Remove-Item -LiteralPath $markerPath -Force
    Write-Host "Restored $targetPath from $backupPath"
    Write-Host "SHA-256: $restoreHash. The backup and marker were kept/removed as expected; no INI was changed."
    return
}

# ---- Warnings and explicit consent -------------------------------------------------------------------------------------
Write-Risks
if (-not $AcceptThirdPartyDll) {
    Write-Host 'Type Y and press Enter to accept these risks and continue.' -ForegroundColor Yellow
    Write-Host 'Anything else - including just Enter - cancels. For unattended use, pass -AcceptThirdPartyDll.' -ForegroundColor Yellow
    Write-Host -NoNewline 'Accept [y/N]: '
    $answer = $null
    try { $answer = [Console]::ReadLine() } catch { $answer = $null }
    if ($null -eq $answer -or $answer.Trim() -notmatch '^(y|yes)$') {
        throw 'Cancelled at the risk confirmation. Nothing was installed.'
    }
}

$gpuInfo = Resolve-GpuFamily $GpuFamily
Write-Host "GPU: $($gpuInfo.AdapterName) -> '$($gpuInfo.Family)' ($($gpuInfo.Source))"

$archive = Get-Archive $gpuInfo.Family
$variantRecord = Get-Variant $Variant
$expectedDllHash = (Get-VariantHash $archive $Variant).ToUpperInvariant()
if ($variantRecord.changesImage) {
    Write-Warning "Variant '$Variant' changes the picture: $($variantRecord.description). The upstream archive's WARNING.txt has the details."
}

if ($marker -and $marker.family -eq $gpuInfo.Family -and $marker.variant -eq $Variant -and
    $marker.dllSha256 -eq $expectedDllHash -and $targetHash -eq $expectedDllHash) {
    Write-Host "Already installed: $($gpuInfo.Family) / $Variant, SHA-256 $expectedDllHash. Nothing to do."
    Write-Host "To undo: powershell -ExecutionPolicy Bypass -File `".\$([IO.Path]::GetFileName($PSCommandPath))`" -Destination `"$destinationPath`" -Restore"
    return
}

# ---- Download and extract only the allowlisted DLL ----------------------------------------------------------------------
$cacheRoot = Join-Path $PSScriptRoot ".dependencies\fsr4\$($manifest.version)"
New-Item -ItemType Directory -Force -Path $cacheRoot | Out-Null
$archivePath = $ArchivePath
if (-not $archivePath) {
    $archivePath = Join-Path $cacheRoot $archive.file
    if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
        Write-Host "Downloading upstream archive: $($archive.url)"
        $downloadPath = Join-Path $cacheRoot ("download-{0}.zip" -f [Guid]::NewGuid().ToString('N'))
        try {
            Invoke-WebRequest -Uri $archive.url -OutFile $downloadPath -UseBasicParsing
            Assert-Sha256 $downloadPath $archive.sha256 'Downloaded archive'
            Move-Item -LiteralPath $downloadPath -Destination $archivePath
        }
        catch {
            Remove-Item -LiteralPath $downloadPath -ErrorAction SilentlyContinue
            throw
        }
    }
}
$archivePath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($archivePath)
if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) { throw "Archive not found: $archivePath" }
Assert-Sha256 $archivePath $archive.sha256 'Upstream archive'

Add-Type -AssemblyName System.IO.Compression.FileSystem
$payloadDir = Join-Path $cacheRoot 'payload'
New-Item -ItemType Directory -Force -Path $payloadDir | Out-Null
$payloadPath = Join-Path $payloadDir ("{0}-{1}.dll" -f $gpuInfo.Family, $Variant)
$needExtract = -not (Test-Path -LiteralPath $payloadPath -PathType Leaf)
if (-not $needExtract) { $needExtract = (Get-Sha256 $payloadPath) -ne $expectedDllHash }
if ($needExtract) {
    $zip = [IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        $entry = $zip.GetEntry($variantRecord.entry)
        if (-not $entry) { throw "The pinned archive does not contain $($variantRecord.entry)" }
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $payloadPath, $true)
    }
    finally { $zip.Dispose() }
}
Assert-Sha256 $payloadPath $expectedDllHash 'Extracted DLL'

# ---- Install with backup and marker -------------------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $backupPath)) {
    [IO.File]::Copy($targetPath, $backupPath, $false)
    Write-Host "Backup created: $backupPath"
}
$backupHash = Get-Sha256 $backupPath

$tempTarget = Join-Path $targetDir ("{0}.install-{1}.tmp" -f $manifest.target.fileName, [Guid]::NewGuid().ToString('N'))
try {
    [IO.File]::Copy($payloadPath, $tempTarget, $false)
    Assert-Sha256 $tempTarget $expectedDllHash 'Staged DLL'
    Move-Item -LiteralPath $tempTarget -Destination $targetPath -Force
}
catch {
    Remove-Item -LiteralPath $tempTarget -ErrorAction SilentlyContinue
    throw
}
Assert-Sha256 $targetPath $expectedDllHash 'Installed DLL'

$markerObject = [ordered]@{
    tool            = 'get_compatible_fsr4.ps1'
    upstreamProject = $manifest.upstream.projectUrl
    upstreamRelease = $manifest.version
    family          = $gpuInfo.Family
    variant         = $Variant
    archive         = $archive.file
    archiveSha256   = $archive.sha256.ToUpperInvariant()
    dllSha256       = $expectedDllHash
    originalSha256  = $backupHash
    installedAtUtc  = [DateTime]::UtcNow.ToString('o')
}
[IO.File]::WriteAllText($markerPath, ($markerObject | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))

Write-Host ''
Write-Host "Installed $($gpuInfo.Family) / $Variant from upstream $($manifest.version)." -ForegroundColor Green
Write-Host "DLL:    $targetPath"
Write-Host "SHA-256: $expectedDllHash"
Write-Host "Backup: $backupPath (SHA-256 $backupHash)"
Write-Host "To undo: powershell -ExecutionPolicy Bypass -File `".\$([IO.Path]::GetFileName($PSCommandPath))`" -Destination `"$destinationPath`" -Restore"
Write-Host ('OptiScaler.ini was not changed. Upstream: ' + $manifest.upstream.projectUrl)
