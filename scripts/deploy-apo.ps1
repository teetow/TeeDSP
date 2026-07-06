<#
.SYNOPSIS
    Full TeeDSP DSP deploy: build, bump + stage a signed APO driver package,
    install it into the Driver Store, retire any previously-installed
    TeeDspApoComponent packages, and restart Windows Audio so audiodg picks
    up the change. Also rebuilds/relaunches the UI via publish.ps1.

.DESCRIPTION
    publish.ps1 alone only rebuilds and relaunches the standalone TeeDsp.exe
    UI process — it never touches the actual system APO. Windows loads that
    from a signed driver package registered in the Driver Store (see
    apo/driver/README.md), completely independent of anything in
    out/build or %LOCALAPPDATA%\Programs\TeeDsp. Without this script the
    driver-store copy can silently go stale indefinitely: rebuilding the
    project, and even restarting the audio engine via the UI's "Restart
    audio engine" button, has zero effect on it — Audiosrv restart just
    reloads whatever package is currently registered, stale or not.

    Run this whenever a change under src/dsp/**, apo/**, or src/shared/**
    needs to actually reach the running audio path, not just the UI.
    Requires one UAC elevation (driver install + uninstall + Audiosrv
    restart happen together so you only get prompted once).

    Verify success afterward via the "DSP build" line in the TeeDSP status
    bar — it reads live off the running APO instance, not a file on disk.

.PARAMETER SkipUiPublish
    Skip the publish.ps1 (UI build + relaunch) step and only refresh the
    driver-installed APO. Useful if you already ran publish.ps1 separately.

.PARAMETER CertSubject
    Subject of the dev signing cert to reuse for the package.
    Default: 'CN=TeeDSP Dev Self-Sign' (create once via apo\install\SignDev.ps1).
#>
param(
    [switch]$SkipUiPublish,
    [string]$CertSubject = 'CN=TeeDSP Dev Self-Sign'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path $PSScriptRoot -Parent

function Get-ConfigPreset {
    if (Test-Path (Join-Path $repoRoot 'CMakeUserPresets.json')) { return 'vs2022-local' }
    return 'vs2022'
}

# ---- 1. Build everything (and optionally refresh the UI) -----------------
if (-not $SkipUiPublish) {
    Write-Host "== Building + publishing UI =="
    & (Join-Path $PSScriptRoot 'publish.ps1') -Restart
} else {
    Write-Host "== Building (UI publish skipped) =="
    $configPreset = Get-ConfigPreset
    $buildPreset  = if ($configPreset -eq 'vs2022-local') { 'vs2022-local-release' } else { 'vs2022-release' }
    & cmake --preset $configPreset | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
    & cmake --build --preset $buildPreset --parallel
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }
}

$configPreset = Get-ConfigPreset
$apoDll = Join-Path $repoRoot "out\build\$configPreset\apo\Release\TeeDspApo.dll"
if (-not (Test-Path $apoDll)) { throw "Built APO DLL not found: $apoDll" }

# ---- 2. Bump the component INF's DriverVer --------------------------------
# Windows won't reload a package whose date/version doesn't outrank what's
# already installed, so every deploy needs a strictly-increasing stamp.
# Encoded as 0.1.<day-of-year>.<HHmm> alongside today's date — monotonic
# within a year and trivially inspectable (pnputil driver-version output).
$infPath = Join-Path $repoRoot 'apo\driver\TeeDspApoComponent.inf'
$now = Get-Date
$dateStr = $now.ToString('MM/dd/yyyy')
$versionStr = "0.1.$($now.DayOfYear).$($now.ToString('HHmm'))"
$infText = Get-Content $infPath -Raw
$infText = [regex]::Replace($infText, 'DriverVer\s*=\s*[^\r\n]+', "DriverVer   = $dateStr,$versionStr")
Set-Content -Path $infPath -Value $infText -NoNewline
Write-Host "Bumped TeeDspApoComponent.inf DriverVer -> $dateStr,$versionStr"

# ---- 3. Stage + sign a fresh driver package -------------------------------
$cert = Get-ChildItem Cert:\CurrentUser\My |
    Where-Object { $_.Subject -eq $CertSubject } |
    Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $cert) {
    throw "No signing cert found ('$CertSubject'). Run apo\install\SignDev.ps1 once first."
}

$stageDir = Join-Path $repoRoot "out\driver-package-$($now.ToString('yyyyMMdd-HHmmss'))"
& (Join-Path $repoRoot 'apo\driver\New-TeeDspApoDriverPackage.ps1') `
    -DllPath $apoDll -OutputDir $stageDir -CertThumbprint $cert.Thumbprint

# ---- 4. Elevate once: install the new package, retire old ones, restart --
$componentInf = Join-Path $stageDir 'TeeDspApoComponent.inf'
$log = Join-Path $repoRoot 'out\deploy-apo-log.txt'
Remove-Item $log -ErrorAction SilentlyContinue

# Built as a single elevated script so the user is only prompted once. Only
# the two host-known paths ($componentInf, $log) are substituted by the
# *parent* shell below; everything else (backtick-escaped) evaluates in the
# elevated child, where the actual pnputil enumeration happens.
$elevatedScript = @"
`$log = '$log'
"== Installing new package ==" | Out-File `$log
`$addOutput = pnputil /add-driver '$componentInf' /install
`$addOutput | Out-File `$log -Append
`$justAdded = (`$addOutput | Select-String 'Published Name:\s+(oem\d+\.inf)').Matches |
    Select-Object -Last 1 | ForEach-Object { `$_.Groups[1].Value }

"== Retiring superseded TeeDspApoComponent packages (keeping `$justAdded) ==" | Out-File `$log -Append
`$lines = pnputil /enum-drivers
`$current = `$null
`$stale = @()
foreach (`$line in `$lines) {
    if (`$line -match 'Published Name:\s+(oem\d+\.inf)') { `$current = `$Matches[1] }
    if (`$line -match 'Original Name:\s+teedspapocomponent\.inf' -and `$current -ne `$justAdded) { `$stale += `$current }
}
foreach (`$oem in (`$stale | Sort-Object -Unique)) {
    "Removing superseded `$oem" | Out-File `$log -Append
    pnputil /delete-driver `$oem /uninstall /force *>> `$log
}

"== Restarting Windows Audio ==" | Out-File `$log -Append
Restart-Service Audiosrv -Force *>> `$log
"@

Write-Host ""
Write-Host "A UAC elevation prompt will appear now - approve it to install the"
Write-Host "new driver package, retire the old one(s), and reload Windows Audio."
Start-Process powershell -Verb RunAs `
    -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', $elevatedScript) -Wait

Write-Host ""
Write-Host "--- deploy log ---"
Get-Content $log -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Done. Check the 'DSP build' line in the TeeDSP status bar - it should"
Write-Host "now read today's build time."
