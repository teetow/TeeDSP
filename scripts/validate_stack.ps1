<#
.SYNOPSIS
    Runs unattended validation gates for TeeDSP with optional install/uninstall checks.

.DESCRIPTION
    Default behavior is non-invasive:
    1) configure
    2) build preset
    3) optionally attempt TeeDspApo target build

    Optional install/uninstall actions are available but can trigger UAC prompts.

.PARAMETER ConfigurePreset
    CMake configure preset name.

.PARAMETER BuildPreset
    CMake build preset name.

.PARAMETER TryApoTarget
    Attempts to build TeeDspApo target explicitly.

.PARAMETER FailOnMissingApo
    Fails when TeeDspApo target build fails or is unavailable.

.PARAMETER RunInstall
    Executes scripts/install_apo.ps1 after build.

.PARAMETER RunUninstall
    Executes scripts/uninstall_apo.ps1 after install/build.

.PARAMETER RunContractChecks
    Executes scripts/verify_apo_contract.ps1 after build/install stages.

.PARAMETER RequireEndpointBinding
    Requires endpoint MFX binding check to pass during contract checks.

.PARAMETER ProbeSharedMapping
    Probes Global\\TeeDspApo_v1 or Local\\TeeDspApo_v1 during contract checks.

.PARAMETER RequireSharedMapping
    Requires shared mapping probe to pass during contract checks.

.PARAMETER RequireSlot14Primary
    Requires TeeDSP CLSID to be first in endpoint slot 14 during contract checks.
#>
param(
    [string]$ConfigurePreset = 'vs2022',
    [string]$BuildPreset = 'vs2022-debug',
    [switch]$TryApoTarget,
    [switch]$FailOnMissingApo,
    [switch]$RunInstall,
    [switch]$RunUninstall,
    [switch]$RunContractChecks,
    [switch]$RequireEndpointBinding,
    [switch]$ProbeSharedMapping,
    [switch]$RequireSharedMapping,
    [switch]$RequireSlot14Primary
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Path $PSScriptRoot -Parent

function Invoke-Step {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,
        [Parameter(Mandatory = $true)]
        [scriptblock]$Action
    )

    Write-Host ""
    Write-Host "=== $Name ==="
    & $Action
    if ($LASTEXITCODE -ne 0) {
        throw "$Name failed with exit code $LASTEXITCODE"
    }
    Write-Host "$Name passed."
}

function Resolve-ApoDllPath {
    $repo = Split-Path -Path $PSScriptRoot -Parent
    $candidates = @(
        (Join-Path $repo 'dist\TeeDspApo.dll'),
        (Join-Path $repo 'out\build\vs2022\Debug\TeeDspApo.dll'),
        (Join-Path $repo 'out\build\vs2022\Release\TeeDspApo.dll')
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    $globMatches = Get-ChildItem -Path (Join-Path $repo 'out\build') -Filter 'TeeDspApo.dll' -Recurse -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending
    if ($globMatches -and $globMatches.Count -gt 0) {
        return $globMatches[0].FullName
    }

    return $null
}

$summary = [ordered]@{
    Configure = 'not-run'
    BuildPreset = 'not-run'
    ApoTarget = 'not-run'
    Install = 'not-run'
    Uninstall = 'not-run'
    ContractChecks = 'not-run'
}

Push-Location $repoRoot
try {
    $apoDllPath = $null

    Invoke-Step -Name 'Configure preset' -Action {
        cmake --preset $ConfigurePreset
    }
    $summary.Configure = 'passed'

    Invoke-Step -Name 'Build preset' -Action {
        cmake --build --preset $BuildPreset --parallel
    }
    $summary.BuildPreset = 'passed'

    if ($TryApoTarget) {
        Write-Host ""
        Write-Host '=== Build TeeDspApo target ==='
        cmake --build --preset $BuildPreset --target TeeDspApo --parallel
        if ($LASTEXITCODE -eq 0) {
            $summary.ApoTarget = 'passed'
            Write-Host 'Build TeeDspApo target passed.'
        } else {
            $summary.ApoTarget = 'failed'
            $msg = 'Build TeeDspApo target failed or target unavailable (likely missing audioenginebaseapo.lib).'
            if ($FailOnMissingApo) {
                throw $msg
            }
            Write-Warning $msg
        }
    }

    $apoDllPath = Resolve-ApoDllPath

    if ($RunInstall) {
        if (-not $apoDllPath) {
            throw 'RunInstall requested but TeeDspApo.dll was not found in dist/ or out/build. Build TeeDspApo first.'
        }
        Invoke-Step -Name 'Install APO' -Action {
            & (Join-Path $PSScriptRoot 'install_apo.ps1') -DllPath $apoDllPath
        }
        $summary.Install = 'passed'
    }

    if ($RunContractChecks) {
        Invoke-Step -Name 'Contract checks' -Action {
            $args = @()
            if ($RequireEndpointBinding) { $args += '-RequireEndpointBinding' }
            if ($ProbeSharedMapping) { $args += '-ProbeSharedMapping' }
            if ($RequireSharedMapping) { $args += '-RequireSharedMapping' }
            if ($RequireSlot14Primary) { $args += '-RequireSlot14Primary' }
            & (Join-Path $PSScriptRoot 'verify_apo_contract.ps1') @args
        }
        $summary.ContractChecks = 'passed'
    }

    if ($RunUninstall) {
        Invoke-Step -Name 'Uninstall APO' -Action {
            if ($apoDllPath) {
                & (Join-Path $PSScriptRoot 'uninstall_apo.ps1') -DllPath $apoDllPath
            } else {
                & (Join-Path $PSScriptRoot 'uninstall_apo.ps1')
            }
        }
        $summary.Uninstall = 'passed'
    }

    Write-Host ""
    Write-Host 'Validation summary:'
    $summary.GetEnumerator() | ForEach-Object {
        Write-Host (" - {0}: {1}" -f $_.Key, $_.Value)
    }
}
catch {
    Write-Error $_
    Write-Host ""
    Write-Host 'Validation summary (partial):'
    $summary.GetEnumerator() | ForEach-Object {
        Write-Host (" - {0}: {1}" -f $_.Key, $_.Value)
    }
    exit 1
}
finally {
    Pop-Location
}
