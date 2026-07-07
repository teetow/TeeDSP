<#
.SYNOPSIS
    Snapshot of AirPods A2DP render-endpoint state for the churn A/B test.

.DESCRIPTION
    Lists every "Headphones" render endpoint under MMDevices with its registry
    key last-write time, device state, and whether the TeeDSP SFX binding still
    advertises the COMMUNICATIONS processing mode. Run once to baseline,
    disconnect/reconnect the AirPods, then run again: a brand-new endpoint GUID
    appearing (and the old one going UNPLUGGED) means a mint happened.

    NOTE (2026-07-07): the timestamp is RegQueryInfoKey last-write — the last
    state change, NOT creation time (registry keys have no creation time). For
    tombstones it approximates abandonment time. For actual mint/reuse history
    and churn rates use Get-AirPodsEndpointHistory.ps1, which reads the
    Audio/Operational event log and is immune to tombstone cleanup.

    Non-elevated; read-only.
#>
$ErrorActionPreference = 'Stop'
$sig = @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class RegTime {
  [DllImport("advapi32.dll")] static extern int RegQueryInfoKey(IntPtr hKey, StringBuilder c, ref uint cb, IntPtr r,
    out uint a, out uint b, out uint d, out uint e, out uint f, out uint g, out uint h, out long ft);
  public static DateTime Get(Microsoft.Win32.RegistryKey k) {
    uint cb=0; uint a,b,c,d,e,f,g; long ft;
    RegQueryInfoKey(k.Handle.DangerousGetHandle(), null, ref cb, IntPtr.Zero, out a,out b,out c,out d,out e,out f,out g,out ft);
    return DateTime.FromFileTime(ft);
  }
}
'@
if (-not ('RegTime' -as [type])) { Add-Type -TypeDefinition $sig }

$TEE  = 'B7E1A0C0-7E5D-4D8B-9E2A-1C4F8D3A2B11'
$COMMS = '98951333-B9CD-48B1-A0A3-FF40682D73F7'
$SFX_MODES = '{d3993a3f-99c2-4402-b5ec-a92a0367664b},5'
$SFX_CLSID = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5'
$NAMEKEY   = '{a45c254e-df1c-4efd-8020-67d146a850e0},2'

$base = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
    'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render')

$rows = foreach ($n in $base.GetSubKeyNames()) {
    $k = $base.OpenSubKey($n)
    $props = $base.OpenSubKey("$n\Properties")
    $name = if ($props) { $props.GetValue($NAMEKEY) } else { $null }
    if ($name -ne 'Headphones') { if ($k){$k.Close()}; if($props){$props.Close()}; continue }
    $fx = $base.OpenSubKey("$n\FxProperties")
    $isTee = $false; $hasComms = $false
    if ($fx) {
        $isTee = ("$($fx.GetValue($SFX_CLSID))" -match $TEE)
        $modes = $fx.GetValue($SFX_MODES)
        $hasComms = ($modes -join ' ') -match $COMMS
        $fx.Close()
    }
    [PSCustomObject]@{
        LastWrite = [RegTime]::Get($k)
        State    = switch ((Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$n").DeviceState) {
                       1 {'ACTIVE'} 2 {'DISABLED'} 4 {'NOTPRESENT'} 8 {'UNPLUGGED'} default {"0x$('{0:x}' -f $_)"} }
        TeeSFX   = $isTee
        CommsMode= if ($isTee) { $hasComms } else { '-' }
        Endpoint = $n.Substring(0,14)
    }
    $k.Close(); if ($props){$props.Close()}
}

$rows = $rows | Sort-Object LastWrite
$rows | Format-Table @{n='LastWrite';e={'{0:MM-dd HH:mm}' -f $_.LastWrite}}, State, TeeSFX, CommsMode, Endpoint -AutoSize
"Total AirPods 'Headphones' endpoints: $($rows.Count)"
$newest = $rows | Select-Object -Last 1
if ($newest) { "Most recently written: $($newest.Endpoint)  lastWrite $('{0:yyyy-MM-dd HH:mm:ss}' -f $newest.LastWrite)  state=$($newest.State)  commsMode=$($newest.CommsMode)" }
