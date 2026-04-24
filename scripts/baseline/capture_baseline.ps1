<#
.SYNOPSIS
    Captures a snapshot of the Windows audio stack that can be used as a
    known-good baseline or compared against the current state.

.DESCRIPTION
    Snapshots:
      - MMDevices\Audio\Render + Capture FxProperties for every endpoint
      - Per-role default endpoint IDs (Console/Multimedia/Communications)
      - Audio-related Windows services (status + start type)
      - PnP audio devices (AudioEndpoint + MEDIA classes)
      - Installed driver packages relevant to audio (Realtek + any MEDIA-class)
      - TeeDspApo COM registration status
      - Machine/OS info

    Two artifacts are written next to this script:
      1. realtek_clean_baseline.json  - structured summary (checked into the repo)
      2. mmdevices.reg                - full reg export of MMDevices subtree
                                         (for byte-for-byte restoration)

.PARAMETER OutputName
    Basename (no extension) for the pair of artifacts. Defaults to
    "realtek_clean_baseline".

.NOTES
    Requires administrator privileges to read protected FxProperties keys.
#>
param(
    [string]$OutputName = 'realtek_clean_baseline'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'capture_baseline.ps1 must run elevated.'
}

$outDir = $PSScriptRoot
$jsonPath = Join-Path $outDir "$OutputName.json"
$regPath  = Join-Path $outDir 'mmdevices.reg'

function Get-EndpointSummary {
    param([string]$Root, [string]$Flow)
    if (-not (Test-Path $Root)) { return @() }
    $items = @()
    foreach ($ep in Get-ChildItem $Root -ErrorAction SilentlyContinue) {
        $guid = $ep.PSChildName
        $name = $null
        $propsKey = "$($ep.PSPath)\Properties"
        if (Test-Path $propsKey) {
            try {
                $p = Get-ItemProperty -Path $propsKey -ErrorAction SilentlyContinue
                $nameKey = '{a45c254e-df1c-4efd-8020-67d146a850e0},2'
                if ($p.PSObject.Properties.Name -contains $nameKey) { $name = $p.$nameKey }
                if (-not $name) {
                    $friendly = '{b3f8fa53-0004-438e-9003-51a46e139bfc},6'
                    if ($p.PSObject.Properties.Name -contains $friendly) { $name = $p.$friendly }
                }
            } catch {}
        }
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
        $state = (Get-ItemProperty -Path $ep.PSPath -Name 'DeviceState' -ErrorAction SilentlyContinue).DeviceState
        $items += [pscustomobject]@{
            flow         = $Flow
            guid         = $guid
            name         = $name
            deviceState  = $state
            fxProperties = $fx
        }
    }
    return $items
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
[ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDE {
    int EnumAudioEndpoints(int dataFlow, uint stateMask, out IntPtr devices);
    [PreserveSig] int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMD ppDevice);
}
[ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMD {
    int Activate(ref Guid iid, int clsCtx, IntPtr pActivationParams, out IntPtr ppInterface);
    int OpenPropertyStore(int access, out IntPtr properties);
    [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string ppstrId);
}
[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDEC {}
public static class DH {
    public static string Default(int flow, int role) {
        var e = (IMMDE)new MMDEC(); IMMD d;
        int hr = e.GetDefaultAudioEndpoint(flow, role, out d);
        if (hr != 0) return null;
        string id; d.GetId(out id); return id;
    }
}
'@ -ErrorAction SilentlyContinue

$defaults = [ordered]@{}
foreach ($flow in 0,1) {
    foreach ($role in 0,1,2) {
        $id = [DH]::Default($flow, $role)
        $key = ("{0}.{1}" -f @('Render','Capture')[$flow], @('Console','Multimedia','Communications')[$role])
        $defaults[$key] = $id
    }
}

$svcNames = 'Audiosrv','AudioEndpointBuilder','RtkAudioUniversalService','MMCSS'
$services = foreach ($n in $svcNames) {
    $s = Get-Service -Name $n -ErrorAction SilentlyContinue
    if ($s) {
        $wmi = Get-CimInstance Win32_Service -Filter "Name='$n'" -ErrorAction SilentlyContinue
        [pscustomobject]@{
            Name      = $n
            Status    = [string]$s.Status
            StartMode = if ($wmi) { [string]$wmi.StartMode } else { $null }
            PathName  = if ($wmi) { [string]$wmi.PathName } else { $null }
        }
    }
}

$drivers = Get-WindowsDriver -Online -ErrorAction SilentlyContinue |
    Where-Object { $_.ClassName -eq 'MEDIA' -or $_.ProviderName -match 'Realtek' -or $_.OriginalFileName -match 'realtek|rt640|hdxrt|hdxasus|rtkfilter|rtkbtfilter|realtekapo|realtekservice|realtekhsa|realtekasio' } |
    ForEach-Object {
        [pscustomobject]@{
            Driver           = $_.Driver
            OriginalFileName = $_.OriginalFileName
            ProviderName     = $_.ProviderName
            Version          = [string]$_.Version
            Date             = if ($_.Date) { $_.Date.ToString('yyyy-MM-dd') } else { $null }
            ClassName        = $_.ClassName
        }
    }

$pnp = Get-PnpDevice -Class AudioEndpoint,MEDIA -ErrorAction SilentlyContinue | ForEach-Object {
    [pscustomobject]@{
        FriendlyName = $_.FriendlyName
        Class        = $_.Class
        Status       = [string]$_.Status
        InstanceId   = $_.InstanceId
    }
}

$teeClsid = '{B3A4C5D6-E7F8-4A1B-9C2D-3E4F5A6B7C8D}'
$teeRegistered = Test-Path "HKLM:\SOFTWARE\Classes\CLSID\$teeClsid"

$os = Get-CimInstance Win32_OperatingSystem
$cs = Get-CimInstance Win32_ComputerSystem

$renderEndpoints  = @(Get-EndpointSummary -Root 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'  -Flow 'Render')
$captureEndpoints = @(Get-EndpointSummary -Root 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture' -Flow 'Capture')

$baseline = [ordered]@{
    schemaVersion = 1
    capturedAt    = (Get-Date).ToString('o')
    capturedBy    = $env:USERNAME
    machine = [ordered]@{
        ComputerName = $env:COMPUTERNAME
        Manufacturer = $cs.Manufacturer
        Model        = $cs.Model
        OsCaption    = $os.Caption
        OsVersion    = $os.Version
        OsBuild      = $os.BuildNumber
    }
    teeDspApo = [ordered]@{
        clsid         = $teeClsid
        comRegistered = $teeRegistered
        note          = if ($teeRegistered) { 'TeeDspApo COM server is REGISTERED at capture time' } else { 'TeeDspApo COM server is not registered (clean)' }
    }
    defaultEndpoints  = $defaults
    services          = $services
    pnp               = $pnp
    drivers           = $drivers
    renderEndpoints   = $renderEndpoints
    captureEndpoints  = $captureEndpoints
}

$baseline | ConvertTo-Json -Depth 8 | Set-Content -Path $jsonPath -Encoding UTF8
reg export 'HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices' $regPath /y | Out-Null

Write-Host ("Wrote {0} ({1} KB)" -f $jsonPath, [math]::Round((Get-Item $jsonPath).Length/1KB,1))
Write-Host ("Wrote {0} ({1} KB)" -f $regPath,  [math]::Round((Get-Item $regPath).Length/1KB,1))
Write-Host ("Render endpoints:  {0}" -f $renderEndpoints.Count)
Write-Host ("Capture endpoints: {0}" -f $captureEndpoints.Count)
Write-Host ("Driver packages:   {0}" -f $drivers.Count)
Write-Host ("Default render:    {0}" -f $defaults.'Render.Console')
