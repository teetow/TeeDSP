#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Install the TeeDSP APO and bind it to a render endpoint.

.DESCRIPTION
    Stage 1 installer. Performs:
      1. Copies TeeDspApo.dll into %SystemRoot%\System32 (audiodg loads from there)
      2. regsvr32 → DllRegisterServer writes the CLSID + APO registry entries
      3. Sets PKEY_FX_ModeEffectClsid on the chosen render endpoint
      4. Restarts the Windows Audio service so audiodg picks up the change

    Run from an elevated PowerShell. To revert, run Uninstall.ps1 with the
    same -EndpointId.

.PARAMETER DllPath
    Path to the built TeeDspApo.dll. Default: ../../build/apo/Release/TeeDspApo.dll
    relative to this script.

.PARAMETER EndpointId
    Endpoint device ID. If omitted, the current default render endpoint is used.
    Use -List to print available endpoints.

.PARAMETER List
    List render endpoints (id + friendly name) and exit.

.EXAMPLE
    .\Install.ps1 -List
    .\Install.ps1                              # bind to default render endpoint
    .\Install.ps1 -EndpointId "{0.0.0.00000000}.{abcdef...}"
#>

[CmdletBinding()]
param(
    [string]$DllPath,
    [string]$EndpointId,
    [switch]$List,
    # Dev-only: set DisableProtectedAudioDG=1 so audiodg loads the unsigned DLL.
    # Security downgrade — use only for bring-up, then sign properly and clear it.
    [switch]$DevBypassSigning
)

$ErrorActionPreference = 'Stop'

throw @'
Install.ps1 is retired. A loose System32 DLL and direct endpoint-registry
editing do not meet the supported APO deployment model. Build and stage the
componentized driver package with apo/driver/New-TeeDspApoDriverPackage.ps1,
then install its signed package through the Windows driver stack.
'@

# Must match Guids.h / DllMain.cpp.
$ApoClsid = '{B7E1A0C0-7E5D-4D8B-9E2A-1C4F8D3A2B11}'

. "$PSScriptRoot\_Interop.ps1"

# --- List mode ---------------------------------------------------------------
if ($List) {
    Write-Host "Render endpoints:"
    foreach ($pair in [TeeDsp.Apo.Helpers]::ListRender()) {
        Write-Host ("  {0}" -f $pair[1])
        Write-Host ("    {0}" -f $pair[0])
    }
    return
}

# --- Resolve DLL path --------------------------------------------------------
if (-not $DllPath) {
    $here = Split-Path -Parent $MyInvocation.MyCommand.Path
    $candidates = @(
        Join-Path $here '..\..\out\build\vs2022-local\apo\Release\TeeDspApo.dll'
        Join-Path $here '..\..\out\build\vs2022\apo\Release\TeeDspApo.dll'
        Join-Path $here '..\..\build\apo\Release\TeeDspApo.dll'
        Join-Path $here '..\..\build\apo\Debug\TeeDspApo.dll'
    )
    $DllPath = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $DllPath) {
        throw "Couldn't find TeeDspApo.dll. Build first, or pass -DllPath explicitly."
    }
}
$DllPath = (Resolve-Path $DllPath).Path
Write-Host "Using DLL: $DllPath"

# --- Step 1: copy to System32 -----------------------------------------------
$systemDll = Join-Path $env:SystemRoot 'System32\TeeDspApo.dll'
Write-Host "[1/4] Copying DLL to $systemDll"
Copy-Item -Force -Path $DllPath -Destination $systemDll

# --- Step 2: register COM via DllRegisterServer ------------------------------
Write-Host "[2/4] Registering COM (regsvr32)"
$rs = Start-Process -FilePath "$env:SystemRoot\System32\regsvr32.exe" `
    -ArgumentList "/s", "`"$systemDll`"" -Wait -PassThru
if ($rs.ExitCode -ne 0) { throw "regsvr32 failed with exit code $($rs.ExitCode)" }

# --- Step 3: bind to endpoint ------------------------------------------------
if (-not $EndpointId) {
    $EndpointId = [TeeDsp.Apo.Helpers]::DefaultRenderId()
    Write-Host "[3/4] No -EndpointId given; using default render endpoint:"
    Write-Host "       $EndpointId"
} else {
    Write-Host "[3/4] Binding to endpoint: $EndpointId"
}
[TeeDsp.Apo.Helpers]::SetClsidProp(
    $EndpointId, [TeeDsp.Apo.PK]::ModeEffectCLSID, $ApoClsid)

# --- Step 4: (dev) let audiodg load the unsigned DLL -------------------------
if ($DevBypassSigning) {
    Write-Host "[4/5] Dev bypass: DisableProtectedAudioDG=1 (audiodg will load unsigned DLLs)"
    Write-Warning "Security downgrade — clear this and sign the DLL before any real use."
    New-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio' `
        -Name 'DisableProtectedAudioDG' -Value 1 -PropertyType DWord -Force | Out-Null
} else {
    Write-Host "[4/5] Skipping signing bypass (-DevBypassSigning not set)."
    Write-Host "       The DLL must be signed for audiodg to load it."
}

# --- Step 5: restart audio service ------------------------------------------
Write-Host "[5/5] Restarting Windows Audio service"
Restart-Service -Name 'Audiosrv' -Force

Write-Host ""
Write-Host "Done. Play audio to verify pass-through."
Write-Host "If no sound, check Event Viewer > Applications and Services Logs"
Write-Host "  > Microsoft > Windows > Audio. Most APO load failures show up there."
