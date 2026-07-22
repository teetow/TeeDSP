<#
.SYNOPSIS
    Standing watcher for the AirPods endpoint dead-cycle/recovery pattern
    caught live 2026-07-22.

.DESCRIPTION
    That occurrence's signature: the render endpoint mints a fresh GUID (or
    the same GUID retries), goes NOTPRESENT, and dies -- UNPLUGGED within
    under 100ms -- repeating every ~35s until either a fresh mint or an
    in-place retry finally reaches ACTIVE. Get-AirPodsEndpointHistory.ps1
    already reports mints after the fact; this watches live and specifically
    flags the *dead* cycles (NOTPRESENT -> UNPLUGGED faster than a real
    device could plausibly settle) versus a clean connect, and reports how
    many dead cycles preceded a recovery.

    Polls Microsoft-Windows-Audio/Operational event 65 every 2s (a targeted
    log query -- cheap) rather than Get-PnpDevice (a full device
    enumeration -- observed to be too slow to poll tightly, see WORKLOG).
    Also snapshots which Bluetooth radio currently reports Status=OK at the
    start of each sequence and on recovery, deliberately not hardcoded to
    one adapter -- this is meant to keep working across a dongle swap so a
    +B+ comparison (does a different radio show the same dead-cycle rate?)
    is possible from the same tool.

    Non-elevated; read-only. Ctrl+C to stop.

.PARAMETER DeviceNameMatch
    Regex against the event's DeviceName. Default 'AirPods'.

.PARAMETER LogPath
    Where to append the log, in addition to console output.
    Default: $env:TEMP\teedsp-airpods-watch.log

.PARAMETER DeadCycleThresholdMs
    NOTPRESENT -> UNPLUGGED faster than this counts as a dead cycle rather
    than a plausible real disconnect. The occurrence this was built from
    measured 69-84ms; default 500ms leaves headroom either way.

.PARAMETER DurationMinutes
    Stop automatically after this many minutes. Default 0 = run until Ctrl+C.

.EXAMPLE
    .\Watch-AirPodsEndpointHealth.ps1
    .\Watch-AirPodsEndpointHealth.ps1 -LogPath D:\logs\airpods.log -DurationMinutes 240
#>
[CmdletBinding()]
param(
    [string]$DeviceNameMatch = 'AirPods',
    [string]$LogPath = (Join-Path $env:TEMP 'teedsp-airpods-watch.log'),
    [int]$DeadCycleThresholdMs = 500,
    [int]$DurationMinutes = 0
)

$ErrorActionPreference = 'Stop'
$states = @{ 1 = 'ACTIVE'; 2 = 'DISABLED'; 4 = 'NOTPRESENT'; 8 = 'UNPLUGGED' }

function Write-Log([string]$msg) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'), $msg
    $line | Tee-Object -FilePath $LogPath -Append
}

# Deliberately not hardcoded to one adapter -- a dongle swap is part of the
# current experiment. A name-exclusion filter is too fragile here (it
# actually picked a paired game controller in testing) -- the radio itself
# is the physical device sitting directly on the USB bus (InstanceId
# "USB\VID_..."), while every paired peripheral, transport, and enumerator
# node lives under BTH\/BTHENUM\/BTHLE\ instead. That distinction holds
# regardless of which dongle is plugged in or what it's named.
function Get-ActiveBluetoothRadio {
    $radio = Get-PnpDevice -Class Bluetooth -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -match '^USB\\' -and $_.Status -eq 'OK' } |
        Select-Object -First 1
    if ($radio) { return $radio.FriendlyName }
    return '(none found as OK)'
}

Write-Log "=== Watch started. DeviceNameMatch='$DeviceNameMatch' DeadCycleThresholdMs=$DeadCycleThresholdMs ==="
Write-Log "Active Bluetooth radio right now: $(Get-ActiveBluetoothRadio)"

$lastCheck = Get-Date
$notPresentTime = @{}     # guid -> time of its most recent NOTPRESENT
$sequenceStart = $null    # non-null while a dead-cycle sequence is in progress
$deadCycles = 0

$deadline = if ($DurationMinutes -gt 0) { (Get-Date).AddMinutes($DurationMinutes) } else { [datetime]::MaxValue }

while ((Get-Date) -lt $deadline) {
    $events = Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-Audio/Operational'; Id = 65 } `
                            -MaxEvents 50 -ErrorAction SilentlyContinue |
              Where-Object { $_.TimeCreated -gt $lastCheck } |
              Sort-Object TimeCreated

    foreach ($ev in $events) {
        $lastCheck = $ev.TimeCreated
        [xml]$x = $ev.ToXml()
        $d = @{}
        foreach ($n in $x.Event.EventData.Data) { $d[$n.Name] = $n.'#text' }
        if ($d.DeviceName -notmatch $DeviceNameMatch) { continue }   # skips the paired UNKNOWN dupes too

        $guid = $d.DeviceId
        $guidShort = if ($guid -match '\{([0-9a-f-]{8})') { $Matches[1] } else { $guid }
        $stateName = $states[[int]$d.NewState]
        $now = $ev.TimeCreated

        Write-Log ("  {0}  {1}" -f $guidShort, $stateName)

        switch ($stateName) {
            'NOTPRESENT' {
                if (-not $sequenceStart) {
                    $sequenceStart = $now
                    $deadCycles = 0
                    Write-Log "=== Sequence start. Active radio: $(Get-ActiveBluetoothRadio) ==="
                }
                $notPresentTime[$guid] = $now
            }
            'UNPLUGGED' {
                if ($notPresentTime.ContainsKey($guid)) {
                    $gapMs = ($now - $notPresentTime[$guid]).TotalMilliseconds
                    if ($gapMs -ge 0 -and $gapMs -lt $DeadCycleThresholdMs) {
                        $deadCycles++
                        Write-Log ">>> DEAD CYCLE #$deadCycles ($guidShort, NOTPRESENT->UNPLUGGED in $([math]::Round($gapMs))ms)"
                    }
                }
            }
            'ACTIVE' {
                if ($sequenceStart) {
                    $totalS = [math]::Round(($now - $sequenceStart).TotalSeconds, 1)
                    Write-Log ">>> RECOVERED after $deadCycles dead cycle(s), ${totalS}s. Active radio: $(Get-ActiveBluetoothRadio) <<<"
                    $sequenceStart = $null
                } else {
                    Write-Log ">>> Went ACTIVE with no dead cycles (clean connect) <<<"
                }
                $notPresentTime.Clear()
            }
        }
    }

    Start-Sleep -Seconds 2
}

Write-Log "=== Watch stopped (duration limit reached) ==="
