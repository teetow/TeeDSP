<#
.SYNOPSIS
    Event-log-based history of AirPods A2DP endpoint churn (mints, reuses, artifacts).

.DESCRIPTION
    Replaces registry endpoint counting as the churn metric. Registry counts turned
    out to measure the wrong thing twice over: key timestamps are last-write (not
    creation), and tombstone cleanup / driver reinstalls silently distort the count.

    This script reconstructs the endpoint lifecycle from Microsoft-Windows-Audio/
    Operational event 65 (MMDevAPI device state change), which records DeviceName,
    endpoint GUID, and new state for every transition — including endpoints that
    have since been deleted from the registry.

    Definitions (approximations, documented 2026-07-07 re-litigation):
    - MINT: first-ever appearance of an endpoint GUID that reaches UNPLUGGED or
      ACTIVE within 60s (a real new endpoint). GUIDs that only ever assert
      NOTPRESENT are tombstone re-asserts (e.g. mass sweeps on radio/driver
      re-enumeration), not mints. GUIDs already live at the start of log coverage
      are "pre-existing", not mints.
    - INSTALL ARTIFACT: a mint within 1h after a TeeDSP driver-package operation
      in setupapi.dev.log (install/delete re-enumerates the device and predictably
      mints a fresh endpoint — not organic churn).
    - RECONNECT: an ACTIVE event whose GUID had no ACTIVE event in the prior
      5 minutes (filters same-instant flap recoveries). A reconnect on an
      already-known GUID is a REUSE; the reuse ratio is reuses/(reuses+mints).

    The mint signature observed in practice: old GUID UNPLUGGED -> NOTPRESENT,
    new GUID appears ~3s later and goes ACTIVE — a PnP interface teardown+recreate
    at connect time (BT-stack level).

    Non-elevated; read-only.

.PARAMETER DeviceNameMatch
    Regex matched against the event's DeviceName. Default 'AirPods'.

.PARAMETER Since
    Only consider events at/after this time. Default: full log coverage.
#>
[CmdletBinding()]
param(
    [string]$DeviceNameMatch = 'AirPods',
    [datetime]$Since = [datetime]::MinValue
)
$ErrorActionPreference = 'Stop'

# --- 1. Pull endpoint state-change events -----------------------------------
$states = @{ 1 = 'ACTIVE'; 2 = 'DISABLED'; 4 = 'NOTPRESENT'; 8 = 'UNPLUGGED' }
$raw = Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-Audio/Operational'; Id = 65 } -MaxEvents 100000
$events = foreach ($e in $raw) {
    if ($e.TimeCreated -lt $Since) { continue }
    [xml]$x = $e.ToXml()
    $d = @{}
    foreach ($n in $x.Event.EventData.Data) { $d[$n.Name] = $n.'#text' }
    if ($d.DeviceName -notmatch $DeviceNameMatch -or $d.flow -ne '0') { continue }
    if ($d.DeviceId -notmatch '\{([0-9a-f-]{36})\}$') { continue }
    [PSCustomObject]@{
        Time  = $e.TimeCreated
        Guid  = $Matches[1]
        State = $states[[int]$d.NewState]
    }
}
$events = @($events | Sort-Object Time)
if (-not $events) { "No matching events (DeviceNameMatch='$DeviceNameMatch')."; return }
$logStart = $events[0].Time

# --- 2. TeeDSP driver operations from setupapi.dev.log (for artifact flags) --
$driverOps = @()
$setupLog = "$env:SystemRoot\INF\setupapi.dev.log"
if (Test-Path $setupLog) {
    $sectionTime = $null; $sectionHasTee = $false
    foreach ($line in [System.IO.File]::ReadLines($setupLog)) {
        if ($line -match '^>>>\s+Section start (\d{4})/(\d{2})/(\d{2}) (\d{2}:\d{2}:\d{2})') {
            if ($sectionTime -and $sectionHasTee) { $driverOps += $sectionTime }
            $sectionTime = [datetime]"$($Matches[1])-$($Matches[2])-$($Matches[3]) $($Matches[4])"
            $sectionHasTee = $false
        }
        elseif ($line -match 'teedsp') { $sectionHasTee = $true }
    }
    if ($sectionTime -and $sectionHasTee) { $driverOps += $sectionTime }
}

# --- 3. Walk the timeline: mints, reconnects, reuses -------------------------
$firstSeen = @{}; $lastActive = @{}; $lastState = @{}
$mints = New-Object System.Collections.Generic.List[object]
$reuses = 0; $reconnects = 0

for ($i = 0; $i -lt $events.Count; $i++) {
    $ev = $events[$i]
    $isNewGuid = -not $firstSeen.ContainsKey($ev.Guid)
    if ($isNewGuid) {
        $firstSeen[$ev.Guid] = $ev.Time
        $preExisting = ($ev.Time - $logStart).TotalMinutes -lt 5
        # real endpoint (not a tombstone re-assert): reaches UNPLUGGED/ACTIVE within 60s
        $becomesReal = $false
        for ($j = $i; $j -lt $events.Count -and ($events[$j].Time - $ev.Time).TotalSeconds -le 60; $j++) {
            if ($events[$j].Guid -eq $ev.Guid -and $events[$j].State -in 'UNPLUGGED', 'ACTIVE') { $becomesReal = $true; break }
        }
        if (-not $preExisting -and $becomesReal) {
            # predecessor: most recent other GUID that went NOTPRESENT within the prior 60s
            $pred = $null
            for ($j = $i - 1; $j -ge 0 -and ($ev.Time - $events[$j].Time).TotalSeconds -le 60; $j--) {
                if ($events[$j].Guid -ne $ev.Guid -and $events[$j].State -eq 'NOTPRESENT') { $pred = $events[$j].Guid; break }
            }
            $nearOp = $driverOps | Where-Object { $ev.Time -ge $_ -and ($ev.Time - $_).TotalHours -le 1 } | Select-Object -First 1
            $mints.Add([PSCustomObject]@{
                Time        = $ev.Time
                NewEndpoint = $ev.Guid.Substring(0, 8)
                Replaces    = if ($pred) { $pred.Substring(0, 8) } else { '?' }
                Artifact    = if ($nearOp) { 'INSTALL ({0:HH:mm})' -f $nearOp } else { '' }
            })
        }
    }
    if ($ev.State -eq 'ACTIVE') {
        $gap = if ($lastActive.ContainsKey($ev.Guid)) { ($ev.Time - $lastActive[$ev.Guid]).TotalMinutes } else { [double]::MaxValue }
        if ($gap -gt 5) {
            $reconnects++
            if (-not $isNewGuid -and ($ev.Time - $firstSeen[$ev.Guid]).TotalSeconds -gt 60) { $reuses++ }
        }
        $lastActive[$ev.Guid] = $ev.Time
    }
    $lastState[$ev.Guid] = $ev
}

# --- 4. Report ---------------------------------------------------------------
"Log coverage : {0:yyyy-MM-dd HH:mm} -> {1:yyyy-MM-dd HH:mm}  ({2} events, {3} endpoints)" -f $logStart, $events[-1].Time, $events.Count, $firstSeen.Count
"TeeDSP driver ops in setupapi.dev.log: $($driverOps.Count)"
''
'Mints (new endpoint GUID appearing):'
$mints | Format-Table @{n='Time';e={'{0:MM-dd HH:mm:ss}' -f $_.Time}}, NewEndpoint, Replaces, Artifact -AutoSize
$organic = @($mints | Where-Object { -not $_.Artifact })
$days = [math]::Max(($events[-1].Time - $logStart).TotalDays, 0.01)
'Summary:'
"  mints total    : $($mints.Count)  (organic $($organic.Count), install artifacts $($mints.Count - $organic.Count))"
"  organic rate   : {0:N2}/day over {1:N1} days" -f ($organic.Count / $days), $days
"  reconnects     : $reconnects  (GUID reused: $reuses -> reuse ratio {0:P0})" -f ($(if ($reconnects) { $reuses / $reconnects } else { 0 }))
''
'Current endpoints (last known state):'
$lastState.Values | Sort-Object Time | ForEach-Object { '  {0:MM-dd HH:mm:ss}  {1}  {2}' -f $_.Time, $_.Guid.Substring(0,8), $_.State }
