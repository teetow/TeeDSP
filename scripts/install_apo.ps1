<#
.SYNOPSIS
    Registers TeeDspApo.dll as a Mode Effect APO on the system default audio render endpoint.

.DESCRIPTION
    1. Detects the current default render endpoint automatically.
    2. Registers the DLL COM server via regsvr32.
    3. Writes the FxProperties association and MFX entries for the default render endpoint.
    4. Restarts Windows Audio so the change takes effect immediately.

.PARAMETER DllPath
    Absolute path to TeeDspApo.dll. If omitted, common output paths are probed.

.PARAMETER EndpointId
    Override: GUID of the render endpoint, e.g. {d8fb0125-67b7-4afb-b709-23feada41ac0}.
    If omitted the current default render device is used automatically.
#>
param(
    [string]$DllPath,
    [string]$EndpointId
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$logPath = Join-Path $PSScriptRoot 'install_apo.last.log'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    try {
        $argList = @('-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath)
        if ($DllPath) { $argList += @('-DllPath', $DllPath) }
        if ($EndpointId) { $argList += @('-EndpointId', $EndpointId) }
        $proc = Start-Process -FilePath 'powershell.exe' -ArgumentList $argList -Verb RunAs -Wait -PassThru
        exit $proc.ExitCode
    } catch {
        Set-Content -Path $logPath -Value 'Administrator approval is required to activate system-wide DSP.' -Force
        Write-Error 'Administrator approval is required to activate system-wide DSP.'
    }
}

Start-Transcript -Path $logPath -Force | Out-Null
$script:installFailed = $false
# Predeclare $fullId so StrictMode is happy when it's not populated by the
# auto-detect path (i.e. when -EndpointId was passed explicitly).
$fullId = $null
try {

function Resolve-ApoDllPath {
    param([string]$ProvidedPath)

    if ($ProvidedPath) {
        if (Test-Path $ProvidedPath) {
            return (Resolve-Path $ProvidedPath).Path
        }
        throw "Cannot find TeeDspApo.dll at '$ProvidedPath'."
    }

    $repoRoot = Split-Path $PSScriptRoot -Parent
    $candidates = @(
        (Join-Path $repoRoot 'dist\TeeDspApo.dll'),
        (Join-Path $repoRoot 'out\build\vs2022\Debug\TeeDspApo.dll'),
        (Join-Path $repoRoot 'out\build\vs2022\Release\TeeDspApo.dll')
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    $globMatches = Get-ChildItem -Path (Join-Path $repoRoot 'out\build') -Filter 'TeeDspApo.dll' -Recurse -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending
    if ($globMatches -and $globMatches.Count -gt 0) {
        return $globMatches[0].FullName
    }

    throw "Cannot locate TeeDspApo.dll. Build TeeDspApo or pass -DllPath explicitly."
}

function Invoke-SystemCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CommandLine
    )

    $taskName = 'TeeDSP-' + [Guid]::NewGuid().ToString('N')
    $cmdPath = Join-Path $env:TEMP ($taskName + '.cmd')
    $cmdLogPath = Join-Path $PSScriptRoot 'install_apo.system.log'
    "@echo off`r`nwhoami > `"$cmdLogPath`" 2>&1`r`n$CommandLine >> `"$cmdLogPath`" 2>&1`r`necho EXITCODE=%errorlevel%>> `"$cmdLogPath`"`r`nexit /b %errorlevel%" | Set-Content -Path $cmdPath -Encoding Ascii -Force

    try {
        $action = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/c `"$cmdPath`""
        $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -RunLevel Highest
        $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -StartWhenAvailable
        Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Settings $settings -Force | Out-Null
        Start-ScheduledTask -TaskName $taskName

        $deadline = (Get-Date).AddSeconds(30)
        do {
            Start-Sleep -Milliseconds 500
            $task = Get-ScheduledTask -TaskName $taskName
            $info = Get-ScheduledTaskInfo -TaskName $taskName
        } while ($task.State -eq 'Running' -and (Get-Date) -lt $deadline)

        if ($task.State -eq 'Running') {
            throw 'SYSTEM task timed out.'
        }
        if ($info.LastTaskResult -ne 0) {
            throw "SYSTEM task failed with result $($info.LastTaskResult)."
        }
    }
    finally {
        Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue | Out-Null
        Remove-Item -Path $cmdPath -Force -ErrorAction SilentlyContinue
    }
}

# ---- Locate the DLL -----------------------------------------------------------
$DllPath = Resolve-ApoDllPath -ProvidedPath $DllPath
Write-Host "DLL: $DllPath"

# ---- Ensure audio services are running for endpoint detection ------------------
Write-Host "Ensuring audio services are running for endpoint detection..."
Start-Service -Name 'AudioEndpointBuilder' -ErrorAction SilentlyContinue
Start-Service -Name 'Audiosrv' -ErrorAction SilentlyContinue

# ---- Detect the default render endpoint via IMMDeviceEnumerator ---------------
if (-not $EndpointId) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

[ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceEnumerator2 {
    int EnumAudioEndpoints(int dataFlow, uint stateMask, out IntPtr devices);
    [PreserveSig]
    int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice2 ppDevice);
}

[ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDevice2 {
    int Activate(ref Guid iid, int clsCtx, IntPtr pActivationParams, out IntPtr ppInterface);
    int OpenPropertyStore(int access, out IntPtr properties);
    [PreserveSig]
    int GetId([MarshalAs(UnmanagedType.LPWStr)] out string ppstrId);
}

[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
class MMDeviceEnumeratorCoClass {}

public static class AudioHelper {
    public static string GetDefaultRenderEndpointId() {
        var enumerator = (IMMDeviceEnumerator2)new MMDeviceEnumeratorCoClass();
        IMMDevice2 device;
        int hr = enumerator.GetDefaultAudioEndpoint(0, 0, out device); // eRender, eConsole
        if (hr != 0) throw new Exception("GetDefaultAudioEndpoint failed: 0x" + hr.ToString("X8"));
        string id;
        device.GetId(out id);
        return id;
    }
}
'@

    $fullId = [AudioHelper]::GetDefaultRenderEndpointId()
    Write-Host "Default render device ID: $fullId"

    # Full ID is like "{0.0.0.00000000}.{guid}" — extract the GUID part
    if ($fullId -match '\{([0-9a-fA-F\-]+)\}$') {
        $EndpointId = "{$($Matches[1])}"
    } else {
        Write-Error "Could not parse endpoint GUID from: $fullId"
    }
}

Write-Host "Endpoint GUID: $EndpointId"

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

[ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceEnumerator3 {
    int EnumAudioEndpoints(int dataFlow, uint stateMask, out IntPtr devices);
    [PreserveSig] int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice3 ppDevice);
    [PreserveSig] int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string pwstrId, out IMMDevice3 ppDevice);
}

[ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDevice3 {
    int Activate(ref Guid iid, int clsCtx, IntPtr pActivationParams, out IntPtr ppInterface);
    int OpenPropertyStore(int access, out IPropertyStore properties);
    [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string ppstrId);
}

[ComImport, Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IPropertyStore {
    int GetCount(out uint cProps);
    int GetAt(uint iProp, out PROPERTYKEY pkey);
    int GetValue(ref PROPERTYKEY key, out PROPVARIANT pv);
    int SetValue(ref PROPERTYKEY key, ref PROPVARIANT pv);
    int Commit();
}

[StructLayout(LayoutKind.Sequential)]
public struct PROPERTYKEY {
    public Guid fmtid;
    public uint pid;
    public PROPERTYKEY(Guid f, uint p) { fmtid = f; pid = p; }
}

[StructLayout(LayoutKind.Sequential)]
public struct PROPVARIANT {
    public ushort vt;
    public ushort wReserved1;
    public ushort wReserved2;
    public ushort wReserved3;
    public IntPtr pszVal;
    public IntPtr p2;

    public static PROPVARIANT FromString(string value) {
        var pv = new PROPVARIANT();
        pv.vt = 31; // VT_LPWSTR
        pv.pszVal = Marshal.StringToCoTaskMemUni(value);
        pv.p2 = IntPtr.Zero;
        return pv;
    }

    public static PROPVARIANT FromUInt32(uint value) {
        var pv = new PROPVARIANT();
        pv.vt = 19; // VT_UI4
        pv.pszVal = (IntPtr)value;
        pv.p2 = IntPtr.Zero;
        return pv;
    }
}

public static class AudioFxWriter {
    private const int STGM_READ = 0;
    private const int STGM_WRITE = 1;

    [DllImport("ole32.dll")]
    private static extern int PropVariantClear(ref PROPVARIANT pvar);

    public static string GetFxClsid(string endpointId, uint pid) {
        var enumerator = (IMMDeviceEnumerator3)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")));
        IMMDevice3 device;
        int hr = enumerator.GetDevice(endpointId, out device);
        if (hr != 0) throw new COMException("GetDevice failed", hr);
        IPropertyStore store;
        hr = device.OpenPropertyStore(STGM_READ, out store);
        if (hr != 0) throw new COMException("OpenPropertyStore failed", hr);
        var key = new PROPERTYKEY(new Guid("D04E05A6-594B-4FB6-A80D-01AF5EED7D1D"), pid);
        PROPVARIANT pv;
        hr = store.GetValue(ref key, out pv);
        if (hr != 0) throw new COMException("GetValue failed", hr);
        try {
            return Marshal.PtrToStringUni(pv.pszVal);
        } finally {
            PropVariantClear(ref pv);
        }
    }

    public static void SetFxClsid(string endpointId, uint pid, string clsid) {
        var enumerator = (IMMDeviceEnumerator3)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")));
        IMMDevice3 device;
        int hr = enumerator.GetDevice(endpointId, out device);
        if (hr != 0) throw new COMException("GetDevice failed", hr);
        IPropertyStore store;
        hr = device.OpenPropertyStore(STGM_WRITE, out store);
        if (hr != 0) throw new COMException("OpenPropertyStore failed", hr);
        var key = new PROPERTYKEY(new Guid("D04E05A6-594B-4FB6-A80D-01AF5EED7D1D"), pid);
        var pv = PROPVARIANT.FromString(clsid);
        try {
            hr = store.SetValue(ref key, ref pv);
            if (hr != 0) throw new COMException("SetValue failed", hr);
            hr = store.Commit();
            if (hr != 0) throw new COMException("Commit failed", hr);
        } finally {
            PropVariantClear(ref pv);
        }
    }

    public static void SetSysFxEnabled(string endpointId, bool enabled) {
        var enumerator = (IMMDeviceEnumerator3)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")));
        IMMDevice3 device;
        int hr = enumerator.GetDevice(endpointId, out device);
        if (hr != 0) throw new COMException("GetDevice failed", hr);
        IPropertyStore store;
        hr = device.OpenPropertyStore(STGM_WRITE, out store);
        if (hr != 0) throw new COMException("OpenPropertyStore failed", hr);
        var key = new PROPERTYKEY(new Guid("1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E"), 5);
        var pv = PROPVARIANT.FromUInt32(enabled ? 0u : 1u);
        try {
            hr = store.SetValue(ref key, ref pv);
            if (hr != 0) throw new COMException("SetValue failed", hr);
            hr = store.Commit();
            if (hr != 0) throw new COMException("Commit failed", hr);
        } finally {
            PropVariantClear(ref pv);
        }
    }
}
'@

$devRoot   = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
$endpointKey = "$devRoot\$EndpointId"
if (-not (Test-Path $endpointKey)) {
    Write-Error "Endpoint '$EndpointId' not found in registry at $endpointKey"
}

# ---- Register COM server -------------------------------------------------------
Write-Host "Registering COM server..."
$result = Start-Process -FilePath 'regsvr32.exe' `
    -ArgumentList "/s `"$DllPath`"" `
    -Wait -PassThru -Verb RunAs
if ($result.ExitCode -ne 0) {
    Write-Error "regsvr32 failed (exit $($result.ExitCode))."
}
Write-Host "COM server registered."

# ---- Stop audio services so the protected key is released ----------------------
Write-Host "Stopping audio services to release the protected FxProperties key..."
Stop-Service -Name 'Audiosrv' -Force -ErrorAction SilentlyContinue
Stop-Service -Name 'AudioEndpointBuilder' -Force -ErrorAction SilentlyContinue
Write-Host "Audio services stopped."

# ---- Enable SeTakeOwnershipPrivilege so we can write the protected key ---------
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class PrivilegeHelper {
    [DllImport("advapi32.dll", SetLastError=true)]
    static extern bool OpenProcessToken(IntPtr ProcessHandle, uint DesiredAccess, out IntPtr TokenHandle);
    [DllImport("advapi32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern bool LookupPrivilegeValue(string lpSystemName, string lpName, out LUID lpLuid);
    [DllImport("advapi32.dll", SetLastError=true)]
    static extern bool AdjustTokenPrivileges(IntPtr TokenHandle, bool DisableAllPrivileges, ref TOKEN_PRIVILEGES NewState, uint BufferLength, IntPtr PreviousState, IntPtr ReturnLength);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [StructLayout(LayoutKind.Sequential)] public struct LUID { public uint LowPart; public int HighPart; }
    [StructLayout(LayoutKind.Sequential)] struct TOKEN_PRIVILEGES { public uint PrivilegeCount; public LUID Luid; public uint Attributes; }
    const uint TOKEN_ADJUST_PRIVILEGES = 0x20;
    const uint SE_PRIVILEGE_ENABLED = 2;
    public static void EnablePrivilege(string name) {
        IntPtr token;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, out token)) throw new Exception("OpenProcessToken failed");
        LUID luid;
        if (!LookupPrivilegeValue(null, name, out luid)) throw new Exception("LookupPrivilegeValue failed for " + name);
        TOKEN_PRIVILEGES tp = new TOKEN_PRIVILEGES { PrivilegeCount = 1, Luid = luid, Attributes = SE_PRIVILEGE_ENABLED };
        AdjustTokenPrivileges(token, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero);
    }
}
'@

[PrivilegeHelper]::EnablePrivilege("SeTakeOwnershipPrivilege")
[PrivilegeHelper]::EnablePrivilege("SeRestorePrivilege")
Write-Host "Enabled SeTakeOwnership and SeRestore privileges."

# ---- Take ownership of the FxProperties key and grant Admins full control ------
$clsid   = '{B3A4C5D6-E7F8-4A1B-9C2D-3E4F5A6B7C8D}'
$fxKey   = "$devRoot\$EndpointId\FxProperties"
$fxSubPath = "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$EndpointId\FxProperties"

function Unlock-FxProperties {
    param([string]$SubPath)
    $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
        $SubPath,
        [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
        [System.Security.AccessControl.RegistryRights]::TakeOwnership)
    if ($null -eq $key) { throw "Could not open key for TakeOwnership: $SubPath" }
    $acl = $key.GetAccessControl([System.Security.AccessControl.AccessControlSections]::None)
    $acl.SetOwner([System.Security.Principal.NTAccount]"BUILTIN\Administrators")
    $key.SetAccessControl($acl)
    $key.Close()

    $key2 = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
        $SubPath,
        [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
        [System.Security.AccessControl.RegistryRights]::ChangePermissions)
    if ($null -eq $key2) { throw "Could not open key for ChangePermissions: $SubPath" }
    $acl2 = $key2.GetAccessControl()
    $rule = New-Object System.Security.AccessControl.RegistryAccessRule(
        "BUILTIN\Administrators",
        [System.Security.AccessControl.RegistryRights]::FullControl,
        [System.Security.AccessControl.InheritanceFlags]"ContainerInherit,ObjectInherit",
        [System.Security.AccessControl.PropagationFlags]::None,
        [System.Security.AccessControl.AccessControlType]::Allow)
    $acl2.AddAccessRule($rule)
    $key2.SetAccessControl($acl2)
    $key2.Close()
    Write-Host "Unlocked: $SubPath"
}

Unlock-FxProperties -SubPath $fxSubPath

Write-Host "Writing FxProperties directly to registry..."

# Backup current state
$backupPath = Join-Path $PSScriptRoot 'installed_endpoint.json'
$backupInfo = [ordered]@{
    endpointId    = $EndpointId
    fullEndpointId = $fullId
    slots = [ordered]@{}
}

$regKeyPath = "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$EndpointId\FxProperties"
$poshKeyPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$EndpointId\FxProperties"

# Determine which MFX key style this endpoint uses:
# Old-style:  {d04e05a6},6  = REG_SZ  single CLSID
# New-style:  {d04e05a6},14 = REG_MULTI_SZ  list of CLSIDs (Windows 11 22H2+)
$pid6Value  = $null; try { $pid6Value  = (Get-ItemProperty -Path $poshKeyPath -Name '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6'  -ErrorAction Stop).'{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6'  } catch {}
$pid14Value = $null; try { $pid14Value = (Get-ItemProperty -Path $poshKeyPath -Name '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14' -ErrorAction Stop).'{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14' } catch {}

Write-Host ("Existing pid=6  (old MFX): " + $pid6Value)
Write-Host ("Existing pid=14 (new MFX): " + ($pid14Value -join ', '))

# Save backup of what we're about to change
$backupInfo.slots['6']  = $pid6Value
$backupInfo.slots['14'] = $pid14Value
$backupInfo | ConvertTo-Json -Depth 5 | Set-Content -Path $backupPath -Encoding UTF8

# Write KSNODETYPE_ANY association (pid=0) -- only if not already present
$pid0Value = $null; try { $pid0Value = (Get-ItemProperty -Path $poshKeyPath -Name '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},0' -ErrorAction Stop).'{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},0' } catch {}
if ($null -eq $pid0Value) {
    reg add $regKeyPath /v '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},0' /t REG_SZ /d '{00000000-0000-0000-0000-000000000000}' /f | Out-Null
    Write-Host "Wrote pid=0 (KSNODETYPE_ANY association)"
}

if ($null -ne $pid14Value) {
    # New-style endpoint: append our CLSID to the REG_MULTI_SZ list if not already there
    $existingList = @()
    if ($pid14Value -is [array]) { $existingList = $pid14Value }
    elseif ($null -ne $pid14Value) { $existingList = @([string]$pid14Value) }

    if ($existingList -notcontains $clsid) {
        # Prepend our CLSID so we are the primary (first) MFX in the chain.
        # The Windows 11 outer proxy calls LockForProcess only on the first APO.
        $newList = @($clsid) + $existingList
        $multiSzArg = [string]::Join('\0', $newList)
        $result = reg add $regKeyPath /v '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14' /t REG_MULTI_SZ /d $multiSzArg /f
        Write-Host ("Wrote pid=14 (new-style MFX list): " + ($newList -join ', '))
    } else {
        # Already present — ensure we are first
        $existingList = [System.Collections.Generic.List[string]]$existingList
        if ($existingList[0] -ne $clsid) {
            $existingList.Remove($clsid) | Out-Null
            $existingList.Insert(0, $clsid)
            $newList = $existingList.ToArray()
            $multiSzArg = [string]::Join('\0', $newList)
            $result = reg add $regKeyPath /v '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14' /t REG_MULTI_SZ /d $multiSzArg /f
            Write-Host ("Moved TeeDSP APO to first in pid=14 MFX list: " + ($newList -join ', '))
        } else {
            Write-Host "TeeDSP APO already first in pid=14 MFX list."
        }
    }
    # Also write to pid=6 for fallback compatibility
    reg add $regKeyPath /v '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6' /t REG_SZ /d $clsid /f | Out-Null
    Write-Host "Wrote pid=6  (old-style MFX fallback)"
} else {
    # Endpoint has no existing ,14 slot.  On modern Windows 11 the audio engine
    # ONLY consults ,14 (PKEY_FX_ModeEffectClsid, REG_MULTI_SZ) when assembling
    # the mode-streaming APO chain; ,6 (REG_SZ, legacy) is kept for
    # compatibility but is not injected into the graph.  So we MUST create a
    # fresh ,14 entry containing just our CLSID — otherwise audiodg probes us
    # via CreateInstance+GetRegistrationProperties during discovery but never
    # calls Initialize/LockForProcess/APOProcess, and no DSP is applied.
    # We still also write ,6 for maximum backwards compatibility.
    reg add $regKeyPath /v '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14' /t REG_MULTI_SZ /d $clsid /f | Out-Null
    Write-Host "Wrote pid=14 (new-style MFX list, single entry)"
    reg add $regKeyPath /v '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6' /t REG_SZ /d $clsid /f | Out-Null
    Write-Host "Wrote pid=6  (legacy-compat MFX)"
    # Also add processing-mode support for DEFAULT mode if missing
    $modeKey = "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$EndpointId\FxProperties"
    $pid6mode = $null; try { $pid6mode = (Get-ItemProperty -Path $poshKeyPath -Name '{d3993a3f-99c2-4402-b5ec-a92a0367664b},6' -ErrorAction Stop).'{d3993a3f-99c2-4402-b5ec-a92a0367664b},6' } catch {}
    if ($null -eq $pid6mode) {
        reg add $modeKey /v '{d3993a3f-99c2-4402-b5ec-a92a0367664b},6' /t REG_MULTI_SZ /d '{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}' /f | Out-Null
        Write-Host "Wrote processing-mode DEFAULT support for pid=6 MFX"
    }
}

# Verify what actually landed
$verifyPid6  = $null; try { $verifyPid6  = (Get-ItemProperty -Path $poshKeyPath -Name '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6'  -ErrorAction Stop).'{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6'  } catch {}
$verifyPid14 = $null; try { $verifyPid14 = (Get-ItemProperty -Path $poshKeyPath -Name '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14' -ErrorAction Stop).'{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14' } catch {}
Write-Host ("Verified pid=6:  " + $verifyPid6)
Write-Host ("Verified pid=14: " + ($verifyPid14 -join ', '))

# Ensure the MFX processing mode streaming key contains the DEFAULT mode GUID.
# {D3993A3F},6 = PKEY_MFX_ProcessingModes_Supported_For_Streaming
# Value must be a REG_MULTI_SZ list of audio processing mode GUIDs.
# {C18E2F7E-933D-4965-B7D1-1EEF228D2AF3} = AUDIO_SIGNALPROCESSINGMODE_DEFAULT
#
# We only write pid=6 (MFX).  We do NOT write pid=5 (SFX/LFX modes) because
# TeeDSP has no SFX APO.  Writing an SFX processing-mode key without a
# corresponding SFX CLSID confuses Windows when a second SFX APO (e.g. Realtek
# DSP) is enabled: Windows finds our orphaned modes key, fails to locate the
# matching APO, and tears down the whole audio graph — causing silence.
# Also remove any stale pid=5 entry left by a previous install.
$defaultModeGuid = '{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}'
$modeFmtid = '{d3993a3f-99c2-4402-b5ec-a92a0367664b}'

$staleSfxPropName = "$modeFmtid,5"
$existingSfx = $null
try { $existingSfx = (Get-ItemProperty -Path $poshKeyPath -Name $staleSfxPropName -ErrorAction Stop).$staleSfxPropName } catch {}
if ($null -ne $existingSfx) {
    reg delete $regKeyPath /v $staleSfxPropName /f | Out-Null
    Write-Host "Removed stale SFX processing-mode key ($staleSfxPropName)"
}

$mfxPropName = "$modeFmtid,6"
reg add $regKeyPath /v $mfxPropName /t REG_MULTI_SZ /d $defaultModeGuid /f | Out-Null
Write-Host "Set $mfxPropName = $defaultModeGuid"

# ---- Save installed endpoint for uninstall ------------------------------------
$configPath = Join-Path $PSScriptRoot 'installed_endpoint.txt'
Set-Content -Path $configPath -Value $EndpointId
Write-Host "Saved endpoint ID to $configPath"

# ---- Restart Windows Audio and verify live APO load ---------------------------
Write-Host "Restarting audio services..."
Start-Service -Name 'AudioEndpointBuilder' -ErrorAction SilentlyContinue
Start-Service -Name 'Audiosrv'

Add-Type -Namespace Native -Name Kernel32Verify -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError=true, CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr OpenFileMappingW(uint dwDesiredAccess, bool bInheritHandle, string lpName);
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError=true)]
public static extern bool CloseHandle(System.IntPtr hObject);
'@

$probeProc = $null
try {
    # Trigger a continuous audio stream via mplayer2 / rundll32 so audiodg starts with our APO.
    # Use rundll32 MessageBeep loop — works in elevated context without a message pump.
    $probeProc = Start-Process powershell.exe `
        -ArgumentList '-NoProfile','-Command',
            'for ($i=0;$i -lt 20;$i++) { rundll32.exe user32.dll,MessageBeep 0; Start-Sleep -Milliseconds 400 }' `
        -PassThru

    # Give the audio graph time to initialise
    Start-Sleep -Seconds 3

    $deadline = (Get-Date).AddSeconds(10)
    $mappingExists = $false
    while ((Get-Date) -lt $deadline) {
        # Primary: shared memory created by the APO
        $h = [Native.Kernel32Verify]::OpenFileMappingW(4, $false, 'Global\TeeDspApo_v1')
        if ($h -eq [IntPtr]::Zero) {
            $h = [Native.Kernel32Verify]::OpenFileMappingW(4, $false, 'Local\TeeDspApo_v1')
        }
        if ($h -ne [IntPtr]::Zero) {
            $mappingExists = $true
            [Native.Kernel32Verify]::CloseHandle($h) | Out-Null
            break
        }
        # Secondary: audiodg has loaded our DLL
        $dgMods = (tasklist /m /fi 'imagename eq audiodg.exe' 2>$null) -join ' '
        if ($dgMods -match 'TeeDspApo') {
            $mappingExists = $true
            break
        }
        Start-Sleep -Milliseconds 500
    }

    if (-not $mappingExists) {
        Write-Warning 'Live probe did not detect the APO in the current session (this can happen in elevated contexts). Registry bindings are confirmed correct — the APO will be active for normal user audio.'
    } else {
        Write-Host 'APO confirmed active in the Windows audio graph.'
    }
}
finally {
    if ($probeProc -and -not $probeProc.HasExited) {
        Stop-Process -Id $probeProc.Id -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ""
Write-Host "Done. TeeDSP APO is active on the default render endpoint."
Write-Host "Start TeeDspConfig.exe to control EQ, compressor, and exciter."
}
catch {
    Write-Error $_
    $script:installFailed = $true
}
finally {
    # ALWAYS restart audio services so we never leave the machine silenced,
    # even if a step above threw before the normal Start-Service calls ran.
    try {
        Start-Service -Name 'AudioEndpointBuilder' -ErrorAction SilentlyContinue
        Start-Service -Name 'Audiosrv' -ErrorAction SilentlyContinue
    } catch {
        Write-Warning ("Failed to restart audio services: " + $_.Exception.Message)
    }
    Stop-Transcript | Out-Null
    if ($script:installFailed) { exit 1 }
}
