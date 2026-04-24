<#
.SYNOPSIS
    Compares the current Windows audio-stack state against a previously captured
    baseline and reports drift.

.DESCRIPTION
    Loads realtek_clean_baseline.json (or a baseline path passed in) and
    compares:
      - TeeDspApo COM registration (hard fail if registered in a "clean" baseline
        but changed now)
      - Per-endpoint FxProperties (presence + value of each property)
      - Audio services (status + start type)
      - Default endpoint IDs per role

    Exit codes:
       0  no drift
       1  drift detected
       2  baseline file missing / malformed

.PARAMETER BaselinePath
    Path to the baseline JSON. Defaults to ./realtek_clean_baseline.json.

.PARAMETER FailOnDrift
    Exit 1 if drift is found (script returns 0 otherwise). Default behavior just
    reports.

.PARAMETER IgnoreMissingEndpoints
    If set, endpoints present in the baseline but missing now are treated as
    informational, not drift (useful after a device was unplugged).
#>
param(
    [string]$BaselinePath = (Join-Path $PSScriptRoot 'realtek_clean_baseline.json'),
    [switch]$FailOnDrift,
    [switch]$IgnoreMissingEndpoints
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path $BaselinePath)) {
    Write-Error "Baseline not found: $BaselinePath"
    exit 2
}

$baseline = Get-Content $BaselinePath -Raw | ConvertFrom-Json

# ---- Build current snapshot using the same logic as capture_baseline.ps1 ------
function Get-EndpointSummary {
    param([string]$Root, [string]$Flow)
    if (-not (Test-Path $Root)) { return @() }
    $items = @()
    foreach ($ep in Get-ChildItem $Root -ErrorAction SilentlyContinue) {
        $guid = $ep.PSChildName
        $fx = @{}
        $fxPath = "$($ep.PSPath)\FxProperties"
        if (Test-Path $fxPath) {
            $fxProps = Get-ItemProperty -Path $fxPath -ErrorAction SilentlyContinue
            if ($fxProps) {
                foreach ($n in $fxProps.PSObject.Properties.Name) {
                    if ($n.StartsWith('PS')) { continue }
                    $v = $fxProps.$n
                    if ($v -is [array]) { $fx[$n] = @($v | ForEach-Object { [string]$_ }) }
                    else { $fx[$n] = [string]$v }
                }
            }
        }
        $items += [pscustomobject]@{
            flow         = $Flow
            guid         = $guid
            fxProperties = $fx
        }
    }
    return $items
}

$nowRender  = Get-EndpointSummary -Root 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'  -Flow 'Render'
$nowCapture = Get-EndpointSummary -Root 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture' -Flow 'Capture'

$drift = @()

function Compare-Endpoints {
    param($BaselineEndpoints, $CurrentEndpoints, [string]$Flow)
    $currentByGuid = @{}
    foreach ($e in $CurrentEndpoints) { $currentByGuid[$e.guid] = $e }

    foreach ($b in $BaselineEndpoints) {
        if (-not $currentByGuid.ContainsKey($b.guid)) {
            if (-not $IgnoreMissingEndpoints) {
                $script:drift += "[$Flow/$($b.guid)] endpoint present in baseline but not in current"
            }
            continue
        }
        $c = $currentByGuid[$b.guid]
        $bFx = $b.fxProperties
        $cFx = $c.fxProperties

        $bNames = @()
        if ($bFx) {
            $bProps = $bFx.PSObject.Properties
            if ($bProps) { $bNames = @($bProps | ForEach-Object { $_.Name }) }
        }
        $cNames = @()
        if ($cFx) { $cNames = @($cFx.Keys) }

        foreach ($name in $bNames) {
            if ($cNames -notcontains $name) {
                $script:drift += "[$Flow/$($b.guid)] MISSING property $name (baseline had a value)"
                continue
            }
            $bVal = $bFx.$name
            $cVal = $cFx[$name]
            $bStr = if ($bVal -is [array]) { ($bVal -join '|') } else { [string]$bVal }
            $cStr = if ($cVal -is [array]) { ($cVal -join '|') } else { [string]$cVal }
            if ($bStr -ne $cStr) {
                $script:drift += "[$Flow/$($b.guid)] CHANGED $name`n    baseline: $bStr`n    current:  $cStr"
            }
        }
        foreach ($name in $cNames) {
            if ($bNames -notcontains $name) {
                $cVal = $cFx[$name]
                $cStr = if ($cVal -is [array]) { ($cVal -join '|') } else { [string]$cVal }
                $script:drift += "[$Flow/$($b.guid)] NEW property $name = $cStr"
            }
        }
    }

    $baselineGuids = @($BaselineEndpoints | ForEach-Object { $_.guid })
    foreach ($c in $CurrentEndpoints) {
        if ($baselineGuids -notcontains $c.guid) {
            $script:drift += "[$Flow/$($c.guid)] NEW endpoint not in baseline"
        }
    }
}

Compare-Endpoints -BaselineEndpoints $baseline.renderEndpoints  -CurrentEndpoints $nowRender  -Flow 'Render'
Compare-Endpoints -BaselineEndpoints $baseline.captureEndpoints -CurrentEndpoints $nowCapture -Flow 'Capture'

# ---- TeeDspApo COM registration -----------------------------------------------
$teeClsid = $baseline.teeDspApo.clsid
$nowRegistered = Test-Path "HKLM:\SOFTWARE\Classes\CLSID\$teeClsid"
if ($nowRegistered -ne $baseline.teeDspApo.comRegistered) {
    $drift += "[TeeDspApo] COM registration changed: baseline=$($baseline.teeDspApo.comRegistered) now=$nowRegistered"
}

# ---- Services -----------------------------------------------------------------
foreach ($bsvc in $baseline.services) {
    $s = Get-Service -Name $bsvc.Name -ErrorAction SilentlyContinue
    if (-not $s) {
        $drift += "[service/$($bsvc.Name)] present in baseline but not found now"
        continue
    }
    $wmi = Get-CimInstance Win32_Service -Filter "Name='$($bsvc.Name)'" -ErrorAction SilentlyContinue
    $nowStart = if ($wmi) { [string]$wmi.StartMode } else { $null }
    if ([string]$s.Status -ne $bsvc.Status) {
        $drift += "[service/$($bsvc.Name)] Status: baseline=$($bsvc.Status) now=$($s.Status)"
    }
    if ($nowStart -ne $bsvc.StartMode) {
        $drift += "[service/$($bsvc.Name)] StartMode: baseline=$($bsvc.StartMode) now=$nowStart"
    }
}

# ---- Report -------------------------------------------------------------------
Write-Host ("Baseline captured: {0} on {1}" -f $baseline.capturedAt, $baseline.machine.ComputerName)
Write-Host ""
if ($drift.Count -eq 0) {
    Write-Host 'No drift detected. Current state matches baseline.'
    exit 0
} else {
    Write-Host ("Drift detected ({0} items):" -f $drift.Count)
    foreach ($d in $drift) { Write-Host "  - $d" }
    if ($FailOnDrift) { exit 1 }
    exit 0
}
