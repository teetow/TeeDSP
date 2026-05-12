#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Uninstall the TeeDSP APO and unbind it from a render endpoint.

.PARAMETER EndpointId
    Endpoint to unbind. If omitted, current default render endpoint is used.

.PARAMETER All
    Unbind from every active render endpoint, not just one.
#>

[CmdletBinding()]
param(
    [string]$EndpointId,
    [switch]$All
)

$ErrorActionPreference = 'Stop'
$ApoClsid = '{B7E1A0C0-7E5D-4D8B-9E2A-1C4F8D3A2B11}'

# Reuse interop shim from Install.ps1 — keep them in sync.
. "$PSScriptRoot\_Interop.ps1"

# --- Step 1: clear endpoint properties --------------------------------------
$endpoints = @()
if ($All) {
    $endpoints = [TeeDsp.Apo.Helpers]::ListRender() | ForEach-Object { $_[0] }
} elseif ($EndpointId) {
    $endpoints = @($EndpointId)
} else {
    $endpoints = @([TeeDsp.Apo.Helpers]::DefaultRenderId())
}

foreach ($ep in $endpoints) {
    Write-Host "[1/3] Clearing FX CLSID on endpoint: $ep"
    try {
        [TeeDsp.Apo.Helpers]::ClearClsidProp($ep, [TeeDsp.Apo.PK]::ModeEffectCLSID)
    } catch {
        Write-Warning "  failed: $($_.Exception.Message)"
    }
}

# --- Step 2: unregister COM --------------------------------------------------
$systemDll = Join-Path $env:SystemRoot 'System32\TeeDspApo.dll'
if (Test-Path $systemDll) {
    Write-Host "[2/3] Unregistering COM (regsvr32 /u)"
    Start-Process -FilePath "$env:SystemRoot\System32\regsvr32.exe" `
        -ArgumentList "/s", "/u", "`"$systemDll`"" -Wait | Out-Null
}

# --- Step 3: restart audio service so audiodg releases the DLL --------------
Write-Host "[3/3] Restarting Windows Audio service"
Restart-Service -Name 'Audiosrv' -Force

# --- Step 4: delete the DLL --------------------------------------------------
if (Test-Path $systemDll) {
    Write-Host "[4/4] Removing $systemDll"
    Remove-Item -Force $systemDll -ErrorAction SilentlyContinue
    if (Test-Path $systemDll) {
        Write-Warning "DLL still locked; reboot may be needed for full cleanup."
    }
}

Write-Host ""
Write-Host "Uninstalled."
