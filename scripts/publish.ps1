<#
.SYNOPSIS
    Builds TeeDsp in Release, deploys Qt dependencies, copies the result to
    %LOCALAPPDATA%\Programs\TeeDsp\, and creates a desktop shortcut.

.PARAMETER SkipBuild
    Skip the cmake --build step and use whatever is already in the Release dir.

.PARAMETER InstallDir
    Override the default %LOCALAPPDATA%\Programs\TeeDsp destination.

.NOTES
    Does not require administrator privileges. Safe to re-run; the install
    directory is wiped and refilled each time so stale files don't accumulate.
#>
param(
    [switch]$SkipBuild,
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'Programs\TeeDsp')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot  = Split-Path $PSScriptRoot -Parent
$buildDir  = Join-Path $repoRoot 'out\build\vs2022\Release'
$exeName   = 'TeeDsp.exe'
$exePath   = Join-Path $buildDir $exeName

# ---- Build --------------------------------------------------------------------
# Prefer vs2022-local if CMakeUserPresets.json exists, otherwise use vs2022
$configPreset = 'vs2022'
$buildPreset = 'vs2022-release'
if (Test-Path (Join-Path $repoRoot 'CMakeUserPresets.json')) {
    $configPreset = 'vs2022-local'
    $buildPreset = 'vs2022-local-release'
}

if (-not $SkipBuild) {
    Write-Host "Configuring..."
    & cmake --preset $configPreset | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

    Write-Host "Building Release..."
    & cmake --build --preset $buildPreset --parallel
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }
}

if (-not (Test-Path $exePath)) {
    throw "Build artifact not found: $exePath"
}

# ---- Deploy Qt ----------------------------------------------------------------
# Locate windeployqt — prefer the one next to the Qt used by CMakePresets.json
# or CMakeUserPresets.json (which holds user-specific paths like CMAKE_PREFIX_PATH).
function Get-QtPrefixFromPresets([string]$file) {
    if (-not (Test-Path $file)) { return $null }
    $p = Get-Content $file -Raw | ConvertFrom-Json
    # First, try to find vs2022-local (user override)
    foreach ($preset in $p.configurePresets) {
        if ($preset.name -eq 'vs2022-local') {
            $cv = $preset.cacheVariables
            if ($cv -and (Get-Member -InputObject $cv -Name 'CMAKE_PREFIX_PATH' -MemberType NoteProperty)) {
                return $cv.CMAKE_PREFIX_PATH
            }
        }
    }
    # Fall back to vs2022
    foreach ($preset in $p.configurePresets) {
        if ($preset.name -eq 'vs2022') {
            $cv = $preset.cacheVariables
            if ($cv -and (Get-Member -InputObject $cv -Name 'CMAKE_PREFIX_PATH' -MemberType NoteProperty)) {
                return $cv.CMAKE_PREFIX_PATH
            }
        }
    }
    return $null
}

$qtPrefix = Get-QtPrefixFromPresets (Join-Path $repoRoot 'CMakeUserPresets.json')
if (-not $qtPrefix) {
    $qtPrefix = Get-QtPrefixFromPresets (Join-Path $repoRoot 'CMakePresets.json')
}
if (-not $qtPrefix) { throw "CMAKE_PREFIX_PATH not found in CMakePresets.json or CMakeUserPresets.json" }

$windeployqt = Join-Path $qtPrefix 'bin\windeployqt.exe'
if (-not (Test-Path $windeployqt)) { throw "windeployqt not found at $windeployqt" }

Write-Host "Running windeployqt..."
& $windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw $exePath | Out-Null
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

# ---- Stage to install dir -----------------------------------------------------
$wasRunning = $false
$running = Get-Process -Name TeeDsp -ErrorAction SilentlyContinue
if ($running) {
    $wasRunning = $true
    Write-Host "Stopping running TeeDsp instance(s)..."
    $running | Stop-Process -Force
    Start-Sleep -Milliseconds 500
}
if (Test-Path $InstallDir) {
    Write-Host "Clearing $InstallDir"
    Remove-Item -Path $InstallDir -Recurse -Force
}
New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null

Write-Host "Copying artifacts to $InstallDir"
Copy-Item -Path (Join-Path $buildDir '*') -Destination $InstallDir -Recurse -Force

# Copy theme.qss from source
$themeQss = Join-Path $repoRoot 'src\ui\theme.qss'
if (Test-Path $themeQss) {
    Copy-Item -Path $themeQss -Destination $InstallDir -Force
}

$installedExe = Join-Path $InstallDir $exeName
if (-not (Test-Path $installedExe)) {
    throw "Install failed: $installedExe not present after copy"
}

# ---- Create desktop shortcut --------------------------------------------------
$desktop = [Environment]::GetFolderPath('Desktop')
$shortcutPath = Join-Path $desktop 'TeeDsp.lnk'
$wsh = New-Object -ComObject WScript.Shell
$shortcut = $wsh.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $installedExe
$shortcut.WorkingDirectory = $InstallDir
$shortcut.IconLocation = "$installedExe, 0"
$shortcut.Description = 'TeeDSP — WASAPI DSP host'
$shortcut.Save()

Write-Host ""
Write-Host "Published."
Write-Host "  Install dir:    $InstallDir"
Write-Host "  Executable:     $installedExe"
Write-Host "  Desktop link:   $shortcutPath"

if ($wasRunning) {
    Write-Host "Relaunching TeeDsp..."
    Start-Process -FilePath $installedExe -WorkingDirectory $InstallDir | Out-Null
}
