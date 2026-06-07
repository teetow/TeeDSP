# Shared C# interop for the TeeDSP APO install / uninstall scripts.
# Dot-source this from Install.ps1 / Uninstall.ps1 before calling
# [TeeDsp.Apo.Helpers]::*.

$cs = @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32;
using System.Security.AccessControl;
using System.Security.Principal;

namespace TeeDsp.Apo
{
    [StructLayout(LayoutKind.Sequential)]
    public struct PROPERTYKEY { public Guid fmtid; public uint pid; }

    [StructLayout(LayoutKind.Explicit)]
    public struct PROPVARIANT
    {
        [FieldOffset(0)]  public ushort vt;
        [FieldOffset(2)]  public ushort r1;
        [FieldOffset(4)]  public ushort r2;
        [FieldOffset(6)]  public ushort r3;
        [FieldOffset(8)]  public IntPtr p;
        [FieldOffset(16)] public long  q;
    }

    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
    public class MMDeviceEnumerator { }

    [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IMMDeviceEnumerator
    {
        int EnumAudioEndpoints(int dataFlow, int stateMask, out IMMDeviceCollection devices);
        int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice device);
        int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice device);
        int RegisterEndpointNotificationCallback(IntPtr client);
        int UnregisterEndpointNotificationCallback(IntPtr client);
    }

    [ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IMMDeviceCollection
    {
        int GetCount(out uint count);
        int Item(uint index, out IMMDevice device);
    }

    [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IMMDevice
    {
        int Activate(ref Guid iid, int clsCtx, IntPtr activationParams,
                     [MarshalAs(UnmanagedType.IUnknown)] out object o);
        int OpenPropertyStore(uint stgmAccess, out IPropertyStore propertyStore);
        int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
        int GetState(out uint state);
    }

    [ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IPropertyStore
    {
        int GetCount(out uint propCount);
        int GetAt(uint property, out PROPERTYKEY key);
        int GetValue(ref PROPERTYKEY key, out PROPVARIANT value);
        int SetValue(ref PROPERTYKEY key, ref PROPVARIANT value);
        int Commit();
    }

    public static class PK
    {
        // {D04E05A6-594B-4FB6-A80D-01AF5EED7D1D}
        public static readonly Guid Fmtid = new Guid("D04E05A6-594B-4FB6-A80D-01AF5EED7D1D");
        public const uint PostMixCLSID    = 2;  // legacy GFX
        public const uint ModeEffectCLSID = 6;  // MFX (Win10+)
    }

    public static class Helpers
    {
        const int eRender   = 0;
        const int eConsole  = 0;
        const int DEVICE_STATE_ACTIVE = 0x1;
        const uint STGM_READ      = 0x0;
        const uint STGM_READWRITE = 0x2;
        const ushort VT_EMPTY  = 0;
        const ushort VT_LPWSTR = 31;

        public static IMMDeviceEnumerator NewEnumerator()
        {
            return (IMMDeviceEnumerator)(new MMDeviceEnumerator());
        }

        public static string DefaultRenderId()
        {
            var e = NewEnumerator();
            IMMDevice d; e.GetDefaultAudioEndpoint(eRender, eConsole, out d);
            string id; d.GetId(out id);
            return id;
        }

        public static System.Collections.Generic.List<string[]> ListRender()
        {
            var e = NewEnumerator();
            IMMDeviceCollection col;
            e.EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, out col);
            uint n; col.GetCount(out n);
            var result = new System.Collections.Generic.List<string[]>();

            // PKEY_Device_FriendlyName
            var nameKey = new PROPERTYKEY {
                fmtid = new Guid("A45C254E-DF1C-4EFD-8020-67D146A850E0"), pid = 14
            };

            for (uint i = 0; i < n; i++) {
                IMMDevice d; col.Item(i, out d);
                string id; d.GetId(out id);
                IPropertyStore ps; d.OpenPropertyStore(STGM_READ, out ps);
                PROPVARIANT pv; ps.GetValue(ref nameKey, out pv);
                string name = (pv.vt == VT_LPWSTR && pv.p != IntPtr.Zero)
                    ? Marshal.PtrToStringUni(pv.p) : "(unknown)";
                result.Add(new[] { id, name });
            }
            return result;
        }

        // The FX effect CLSIDs the audio engine actually consults live in the
        // endpoint's FxProperties registry store — NOT the device property
        // store returned by IMMDevice::OpenPropertyStore. That store is locked
        // to TrustedInstaller, so we take ownership, grant Administrators write,
        // then set/clear the "{fmtid},pid" string value.
        //
        // deviceId looks like "{0.0.0.00000000}.{endpoint-guid}"; the registry
        // subkey under ...\MMDevices\Audio\Render is the trailing {endpoint-guid}.

        static string FxKeyPath(string deviceId)
        {
            int idx = deviceId.LastIndexOf(".{");
            string guid = (idx >= 0) ? deviceId.Substring(idx + 1) : deviceId;
            return @"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\"
                   + guid + @"\FxProperties";
        }

        static string FxValueName(uint pid)
        {
            return "{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D}," + pid;
        }

        static RegistryKey OpenFxKeyWritable(string deviceId)
        {
            string path = FxKeyPath(deviceId);

            // Fast path: already writable (some endpoints/builds permit it).
            // A locked key throws UnauthorizedAccessException OR SecurityException
            // ("Requested registry access is not allowed") — fall through on either.
            try {
                var k = Registry.LocalMachine.OpenSubKey(path, true);
                if (k != null) return k;
            } catch (Exception) { }

            // Take ownership, then (as owner) grant Administrators full control.
            EnablePrivilege("SeTakeOwnershipPrivilege");
            EnablePrivilege("SeRestorePrivilege");
            var admins = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);

            // WRITE_OWNER only: don't read the existing security (we lack
            // READ_CONTROL until we own it). Apply a fresh owner-only descriptor.
            using (var own = Registry.LocalMachine.OpenSubKey(
                       path, RegistryKeyPermissionCheck.ReadWriteSubTree, RegistryRights.TakeOwnership)) {
                if (own == null)
                    throw new Exception("FxProperties key not found for endpoint: " + deviceId
                                        + " (expected HKLM\\" + path + ")");
                var ownerSec = new RegistrySecurity();
                ownerSec.SetOwner(admins);
                own.SetAccessControl(ownerSec);
            }
            // As owner we implicitly have READ_CONTROL + WRITE_DAC, so we can now
            // read and edit the DACL.
            using (var perm = Registry.LocalMachine.OpenSubKey(
                       path, RegistryKeyPermissionCheck.ReadWriteSubTree,
                       RegistryRights.ReadPermissions | RegistryRights.ChangePermissions)) {
                var sec = perm.GetAccessControl(AccessControlSections.Access);
                sec.AddAccessRule(new RegistryAccessRule(
                    admins, RegistryRights.FullControl,
                    InheritanceFlags.None, PropagationFlags.None, AccessControlType.Allow));
                perm.SetAccessControl(sec);
            }
            return Registry.LocalMachine.OpenSubKey(path, true);
        }

        // PKEY_MFX_ProcessingModes_Supported_For_Streaming and the DEFAULT mode.
        // Without a declared streaming mode an MFX CLSID is only *discovered*,
        // never inserted into the audio graph — so the APO never loads.
        const string MfxModesValue = "{D3993A3F-99C2-4402-B5EC-A92A0367664B},6";
        const string ModeDefault   = "{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}";

        public static void SetClsidProp(string deviceId, uint pid, string clsidString)
        {
            using (var k = OpenFxKeyWritable(deviceId)) {
                if (k == null)
                    throw new Exception("Could not open FxProperties key writable for " + deviceId);
                k.SetValue(FxValueName(pid), clsidString, RegistryValueKind.String);
                if (pid == PK.ModeEffectCLSID && k.GetValue(MfxModesValue) == null) {
                    // Only seed the mode list if the endpoint doesn't already
                    // declare one (don't clobber a vendor's existing modes).
                    k.SetValue(MfxModesValue, new string[] { ModeDefault },
                               RegistryValueKind.MultiString);
                }
            }
        }

        public static void ClearClsidProp(string deviceId, uint pid)
        {
            using (var k = OpenFxKeyWritable(deviceId)) {
                if (k == null) return;
                if (k.GetValue(FxValueName(pid)) != null)
                    k.DeleteValue(FxValueName(pid), false);
            }
        }

        // ---- privilege helper (needed to take ownership of the protected key) ----
        [StructLayout(LayoutKind.Sequential)] struct LUID { public uint LowPart; public int HighPart; }
        [StructLayout(LayoutKind.Sequential)] struct TOKEN_PRIVILEGES { public uint Count; public LUID Luid; public uint Attributes; }

        [DllImport("advapi32.dll", SetLastError=true)]
        static extern bool OpenProcessToken(IntPtr h, uint access, out IntPtr token);
        [DllImport("advapi32.dll", SetLastError=true, CharSet=CharSet.Auto)]
        static extern bool LookupPrivilegeValue(string sys, string name, out LUID luid);
        [DllImport("advapi32.dll", SetLastError=true)]
        static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll,
            ref TOKEN_PRIVILEGES newState, uint len, IntPtr prev, IntPtr retLen);
        [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
        [DllImport("kernel32.dll", SetLastError=true)] static extern bool CloseHandle(IntPtr h);

        static void EnablePrivilege(string priv)
        {
            const uint TOKEN_ADJUST_PRIVILEGES = 0x20, TOKEN_QUERY = 0x8, SE_PRIVILEGE_ENABLED = 0x2;
            IntPtr tok;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, out tok))
                return;
            try {
                LUID luid;
                if (!LookupPrivilegeValue(null, priv, out luid)) return;
                var tp = new TOKEN_PRIVILEGES { Count = 1, Luid = luid, Attributes = SE_PRIVILEGE_ENABLED };
                AdjustTokenPrivileges(tok, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero);
            } finally { CloseHandle(tok); }
        }
    }
}
'@

if (-not ('TeeDsp.Apo.Helpers' -as [type])) {
    Add-Type -TypeDefinition $cs -Language CSharp | Out-Null
}
