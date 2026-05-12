# Shared C# interop for the TeeDSP APO install / uninstall scripts.
# Dot-source this from Install.ps1 / Uninstall.ps1 before calling
# [TeeDsp.Apo.Helpers]::*.

$cs = @'
using System;
using System.Runtime.InteropServices;

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

        public static void SetClsidProp(string deviceId, uint pid, string clsidString)
        {
            var e = NewEnumerator();
            IMMDevice d; e.GetDevice(deviceId, out d);
            IPropertyStore ps; d.OpenPropertyStore(STGM_READWRITE, out ps);

            var key = new PROPERTYKEY { fmtid = PK.Fmtid, pid = pid };
            var pv = new PROPVARIANT();
            pv.vt = VT_LPWSTR;
            pv.p  = Marshal.StringToCoTaskMemUni(clsidString);
            int hr = ps.SetValue(ref key, ref pv);
            Marshal.FreeCoTaskMem(pv.p);
            if (hr < 0) throw new System.ComponentModel.Win32Exception(hr,
                "IPropertyStore.SetValue failed (HRESULT 0x" + hr.ToString("X8") + ")");
            ps.Commit();
        }

        public static void ClearClsidProp(string deviceId, uint pid)
        {
            var e = NewEnumerator();
            IMMDevice d; e.GetDevice(deviceId, out d);
            IPropertyStore ps; d.OpenPropertyStore(STGM_READWRITE, out ps);

            var key = new PROPERTYKEY { fmtid = PK.Fmtid, pid = pid };
            // Setting VT_EMPTY clears the property.
            var pv = new PROPVARIANT();
            pv.vt = VT_EMPTY;
            int hr = ps.SetValue(ref key, ref pv);
            if (hr < 0) throw new System.ComponentModel.Win32Exception(hr,
                "IPropertyStore.SetValue(VT_EMPTY) failed (HRESULT 0x" + hr.ToString("X8") + ")");
            ps.Commit();
        }
    }
}
'@

if (-not ('TeeDsp.Apo.Helpers' -as [type])) {
    Add-Type -TypeDefinition $cs -Language CSharp | Out-Null
}
