<#
.SYNOPSIS
    One-click elevated restart of the Windows audio engine (Audiosrv).

.DESCRIPTION
    Recovery for the audiodg-relaunched-PROTECTED failure (sudden total audio
    loss, APO gone) and the discriminating test for the AirPods reconnect
    wedge: if this alone restores audio, the bug is engine/APO-side; if only a
    Bluetooth restart helps, it's BT-transport-side.

    Self-elevates if launched without admin rights. Appends a one-line record
    to %LOCALAPPDATA%\TeeDsp\audiosrv-restarts.log so incidents can be
    correlated with the endpoint-churn timeline later.
#>
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    exit
}

$dgBefore = Get-Process audiodg -ErrorAction SilentlyContinue
$wasProtected = $false
if ($dgBefore) {
    try { $null = $dgBefore.Handle } catch { $wasProtected = $true }
}
Write-Host ("audiodg before restart: " + $(if (-not $dgBefore) { 'not running' }
    elseif ($wasProtected) { 'running PROTECTED (the known failure state)' }
    else { 'running unprotected' }))

Write-Host 'Restarting Audiosrv...' -ForegroundColor Cyan
Restart-Service Audiosrv -Force
Start-Sleep -Seconds 2

$svc = Get-Service Audiosrv
Write-Host "Audiosrv is now: $($svc.Status)" -ForegroundColor $(
    if ($svc.Status -eq 'Running') { 'Green' } else { 'Red' })

$logDir = Join-Path $env:LOCALAPPDATA 'TeeDsp'
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir | Out-Null }
"{0:yyyy-MM-dd HH:mm:ss}  audiodgBefore={1}  audiosrvAfter={2}" -f (Get-Date),
    $(if (-not $dgBefore) { 'absent' } elseif ($wasProtected) { 'PROTECTED' } else { 'ok' }),
    $svc.Status | Add-Content (Join-Path $logDir 'audiosrv-restarts.log')

Write-Host ''
Write-Host 'If audio works now, the problem was engine-side (TeeDSP-adjacent).'
Write-Host 'If it is still silent, restart Bluetooth - that points at the BT transport.'
Write-Host ''
Read-Host 'Press Enter to close'
