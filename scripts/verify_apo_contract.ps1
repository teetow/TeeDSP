<#
.SYNOPSIS
    Verifies TeeDSP APO registration and endpoint contract state.

.DESCRIPTION
    Performs non-destructive checks for:
    - CLSID registration
    - AudioEngine APO registration
    - Endpoint FxProperties binding (slot 14 or 6)
    - Audio service health
    - Optional shared-memory mapping probe

.PARAMETER EndpointId
    Endpoint GUID in the MMDevices Render tree (for example {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}).
    If omitted, attempts to read scripts/installed_endpoint.txt.

.PARAMETER ProbeSharedMapping
    Probes Global\TeeDspApo_v1 (and Local\TeeDspApo_v1 fallback).

.PARAMETER RequireSharedMapping
    Fails if shared mapping probe does not find the mapping.

.PARAMETER RequireEndpointBinding
    Fails if endpoint binding cannot be verified.

.PARAMETER RequireSlot14Primary
    Fails unless TeeDSP CLSID is the first item in slot 14 (REG_MULTI_SZ) when slot 14 exists.

.PARAMETER ReportPath
    Optional JSON report output path.
#>
param(
    [string]$EndpointId,
    [switch]$ProbeSharedMapping,
    [switch]$RequireSharedMapping,
    [switch]$RequireEndpointBinding,
    [switch]$RequireSlot14Primary,
    [string]$ReportPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$expectedClsid = '{B3A4C5D6-E7F8-4A1B-9C2D-3E4F5A6B7C8D}'
$endpointFmtid = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}'

$repoRoot = Split-Path -Path $PSScriptRoot -Parent
if (-not $ReportPath) {
    $ReportPath = Join-Path $PSScriptRoot 'verify_apo_contract.last.json'
}

function Get-EndpointFromMarker {
    $marker = Join-Path $PSScriptRoot 'installed_endpoint.txt'
    if (Test-Path $marker) {
        $raw = (Get-Content -Path $marker -ErrorAction SilentlyContinue | Select-Object -First 1)
        if ($raw) {
            return $raw.Trim()
        }
    }
    return $null
}

function Get-RegValueSafe {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if (-not (Test-Path $Path)) { return $null }
    $props = Get-ItemProperty -Path $Path -ErrorAction SilentlyContinue
    if ($null -eq $props) { return $null }
    $member = $props.PSObject.Properties[$Name]
    if ($null -eq $member) { return $null }
    return $member.Value
}

function Add-CheckResult {
    param(
        [Parameter(Mandatory = $true)]$List,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][bool]$Passed,
        [Parameter(Mandatory = $true)][string]$Detail,
        [switch]$Required
    )

    $List.Add([pscustomobject]@{
        name = $Name
        passed = $Passed
        required = [bool]$Required
        detail = $Detail
    }) | Out-Null
}

if (-not $EndpointId) {
    $EndpointId = Get-EndpointFromMarker
}

$results = New-Object 'System.Collections.Generic.List[object]'

$clsidPath = "HKLM:\SOFTWARE\Classes\CLSID\$expectedClsid"
$apoRegPath = "HKLM:\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\$expectedClsid"

Add-CheckResult -List $results -Name 'CLSID key present' -Passed:(Test-Path $clsidPath) -Detail:$clsidPath -Required
Add-CheckResult -List $results -Name 'APO registration key present' -Passed:(Test-Path $apoRegPath) -Detail:$apoRegPath -Required

$servicesOk = $true
$serviceNames = @('AudioEndpointBuilder', 'Audiosrv')
foreach ($svc in $serviceNames) {
    $status = (Get-Service -Name $svc -ErrorAction SilentlyContinue).Status
    $ok = ($status -eq 'Running')
    if (-not $ok) { $servicesOk = $false }
    Add-CheckResult -List $results -Name ("Service running: {0}" -f $svc) -Passed:$ok -Detail:("status={0}" -f $status) -Required
}

$endpointBindingVerified = $false
if ($EndpointId) {
    $fxKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$EndpointId\FxProperties"
    if (Test-Path $fxKey) {
        $slot14 = Get-RegValueSafe -Path $fxKey -Name ("{0},14" -f $endpointFmtid)
        $slot6 = Get-RegValueSafe -Path $fxKey -Name ("{0},6" -f $endpointFmtid)

        $slot14List = @()
        if ($slot14 -is [array]) {
            $slot14List = @($slot14 | ForEach-Object { [string]$_ })
        } elseif ($null -ne $slot14) {
            $slot14List = @([string]$slot14)
        }

        $slot6List = @()
        if ($slot6 -is [array]) {
            $slot6List = @($slot6 | ForEach-Object { [string]$_ })
        } elseif ($null -ne $slot6) {
            $slot6List = @([string]$slot6)
        }

        $in14 = ($slot14List -contains $expectedClsid)
        $in6 = ($slot6List -contains $expectedClsid)
        $endpointBindingVerified = ($in14 -or $in6)

        Add-CheckResult -List $results -Name 'Endpoint binding contains TeeDSP CLSID' -Passed:$endpointBindingVerified -Detail:("endpoint={0}; inSlot14={1}; inSlot6={2}" -f $EndpointId, $in14, $in6) -Required:$RequireEndpointBinding

        if ($RequireSlot14Primary) {
            $slot14Primary = $false
            if ($slot14List.Count -gt 0) {
                $slot14Primary = ($slot14List[0] -eq $expectedClsid)
            }
            Add-CheckResult -List $results -Name 'Slot 14 primary CLSID is TeeDSP' -Passed:$slot14Primary -Detail:("endpoint={0}; slot14First={1}" -f $EndpointId, ($(if ($slot14List.Count -gt 0) { $slot14List[0] } else { '<missing>' }))) -Required
        }
    } else {
        Add-CheckResult -List $results -Name 'Endpoint FxProperties key present' -Passed:$false -Detail:$fxKey -Required:$RequireEndpointBinding

        if ($RequireSlot14Primary) {
            Add-CheckResult -List $results -Name 'Slot 14 primary CLSID is TeeDSP' -Passed:$false -Detail:'FxProperties key missing.' -Required
        }
    }
} else {
    Add-CheckResult -List $results -Name 'Endpoint resolved' -Passed:$false -Detail:'No EndpointId provided and scripts/installed_endpoint.txt not found.' -Required:$RequireEndpointBinding

    if ($RequireSlot14Primary) {
        Add-CheckResult -List $results -Name 'Slot 14 primary CLSID is TeeDSP' -Passed:$false -Detail:'No endpoint resolved.' -Required
    }
}

if ($ProbeSharedMapping -or $RequireSharedMapping) {
    Add-Type -Namespace Native -Name Kernel32MapProbe -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError=true, CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr OpenFileMappingW(uint dwDesiredAccess, bool bInheritHandle, string lpName);
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError=true)]
public static extern bool CloseHandle(System.IntPtr hObject);
'@

    $mappingFound = $false
    $h = [Native.Kernel32MapProbe]::OpenFileMappingW(4, $false, 'Global\TeeDspApo_v1')
    if ($h -eq [IntPtr]::Zero) {
        $h = [Native.Kernel32MapProbe]::OpenFileMappingW(4, $false, 'Local\TeeDspApo_v1')
    }

    if ($h -ne [IntPtr]::Zero) {
        $mappingFound = $true
        [Native.Kernel32MapProbe]::CloseHandle($h) | Out-Null
    }

    Add-CheckResult -List $results -Name 'Shared mapping probe' -Passed:$mappingFound -Detail:'Global\\TeeDspApo_v1 or Local\\TeeDspApo_v1' -Required:$RequireSharedMapping
}

$failedRequired = @($results | Where-Object { -not $_.passed -and $_.required })
$overallPass = ($failedRequired.Count -eq 0)

$report = [ordered]@{
    timestamp = (Get-Date).ToString('o')
    endpointId = $EndpointId
    overallPass = $overallPass
    checks = $results
}

$report | ConvertTo-Json -Depth 6 | Set-Content -Path $ReportPath -Encoding UTF8

Write-Host ''
Write-Host 'APO contract verification summary:'
foreach ($r in $results) {
    $status = if ($r.passed) { 'PASS' } else { 'FAIL' }
    $req = if ($r.required) { 'required' } else { 'optional' }
    Write-Host (" - [{0}] {1} ({2}) :: {3}" -f $status, $r.name, $req, $r.detail)
}
Write-Host ("Report written to: {0}" -f $ReportPath)
if ($overallPass) {
    Write-Host 'TEE_DSP_CONTRACT_STATUS=PASS'
} else {
    Write-Host 'TEE_DSP_CONTRACT_STATUS=FAIL'
}

if (-not $overallPass) {
    Write-Error 'One or more required contract checks failed.'
    exit 1
}
