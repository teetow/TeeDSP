<#
.SYNOPSIS
    Enables or disables the AAC codec in Windows' inbox Bluetooth A2DP source,
    trading codec quality for a fixed codec and (hopefully) a stable sample rate.

.DESCRIPTION
    Why this exists (2026-08-02): AirPods playback intermittently dies in a state
    where the endpoint is ACTIVE and the APO is bound, but the endpoint's cached
    PKEY_AudioEngine_DeviceFormat disagrees with the rate the A2DP link is
    actually running. Nothing in Windows reconciles the two while the audio
    engine is running, so every shared-mode stream open fails with
    AUDCLNT_E_UNSUPPORTED_FORMAT and all apps refuse to play. See
    src/host/EndpointHealth.h for the detector and repair that heal it after the
    fact.

    This script attacks the upstream cause instead. Windows' BthA2dp.sys supports
    only SBC and AAC, and AirPods run AAC at 44.1 kHz while SBC on this machine
    has been observed at 48 kHz. Removing AAC removes codec switching as a source
    of rate variation, so the disagreement should stop arising at all.

    HONEST LIMITS -- this is an experiment, not a known fix:
      * SBC supports both 44.1 and 48 kHz, so forcing SBC does NOT guarantee a
        single rate. It only removes one candidate cause.
      * Whether codec switching is what moves the rate was never measured. The
        codec was inferred *from* the rate, which makes it circular as evidence.
        Measuring the active codec directly would break that circularity.
      * Microsoft suggests this for the "device recognised as 48 kHz instead of
        44.1 kHz" bug but has not acknowledged the bug or published a cause.

    Chosen deliberately at the user's direction: stability at any cost, and the
    codec artifacts are not audible enough to care about. Do not "restore
    quality" by re-enabling AAC without re-reading that trade.

    VERIFYING THE EXPERIMENT -- do not rely on "no failures for a few days".
    These incidents are sporadic, so quiet days prove little on their own. Use
    the far more sensitive signal instead: %TEMP%\teedsp_endpoint_health.log
    records a "cached format disagrees" note even when playback still works. If
    the mismatch never appears there, the disagreement genuinely stopped
    arising. Falsifiable prediction: with AAC off, every newly minted AirPods
    endpoint should cache the SAME rate. A new one at a different rate kills the
    hypothesis. Baseline before this change was 23 endpoints at 48000 Hz and 11
    at 44100 Hz.

.PARAMETER Disable
    Force SBC by setting BluetoothAacEnable = 0.

.PARAMETER Enable
    Restore Windows' default behaviour by removing the value (AAC is on by
    default, so this deletes rather than setting 1).

.NOTES
    Self-elevates. Takes effect on the next A2DP connection -- put the headphones
    back in the case and take them out again. Microsoft's guidance says a reboot
    may be required; if the negotiated rate does not change after a reconnect,
    reboot before concluding anything.
#>
[CmdletBinding(DefaultParameterSetName = 'Status')]
param(
    [Parameter(ParameterSetName = 'Disable')][switch]$Disable,
    [Parameter(ParameterSetName = 'Enable')][switch]$Enable
)

$ErrorActionPreference = 'Stop'
$key  = 'HKLM:\SYSTEM\CurrentControlSet\Services\BthA2dp\Parameters'
$name = 'BluetoothAacEnable'

function Show-State {
    $k = Get-Item $key -ErrorAction SilentlyContinue
    $v = if ($k -and ($k.GetValueNames() -contains $name)) { $k.GetValue($name) } else { $null }
    if ($null -eq $v) {
        Write-Host "AAC: ENABLED (value absent - Windows default)" -ForegroundColor Yellow
    } elseif ($v -eq 0) {
        Write-Host "AAC: DISABLED (BluetoothAacEnable = 0) -> SBC only" -ForegroundColor Green
    } else {
        Write-Host "AAC: ENABLED (BluetoothAacEnable = $v)" -ForegroundColor Yellow
    }
}

if ($PSCmdlet.ParameterSetName -eq 'Status') {
    Show-State
    'Use -Disable to force SBC, -Enable to restore the Windows default.'
    return
}

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arg = if ($Disable) { '-Disable' } else { '-Enable' }
    Start-Process powershell.exe -Verb RunAs -Wait -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", $arg)
    Show-State
    return
}

if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }

if ($Disable) {
    New-ItemProperty -Path $key -Name $name -Value 0 -PropertyType DWord -Force | Out-Null
    Write-Host 'AAC disabled. Reconnect the headphones (case, then out) for it to apply.'
} else {
    Remove-ItemProperty -Path $key -Name $name -ErrorAction SilentlyContinue
    Write-Host 'Restored Windows default (AAC enabled). Reconnect the headphones to apply.'
}
Show-State
