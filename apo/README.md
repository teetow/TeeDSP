# TeeDSP APO

Stage 1 of moving TeeDSP off the virtual-cable architecture and into the
Windows audio engine itself, as an [Audio Processing Object](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/windows-audio-processing-objects).

## What this stage delivers

A pass-through APO. No DSP. Audio plays through unchanged. The point of
this stage is to validate the COM/registration plumbing on real hardware
before we touch any DSP code:

- COM CLSID registration
- APO discovery by the Windows audio engine
- Endpoint binding via `PKEY_FX_ModeEffectClsid`
- Format negotiation succeeds for whatever the endpoint hands us
- Buffers flow through `APOProcess` without dropouts

If audio plays through cleanly with TeeDSP bound to your AirPods, the
hard part is done. Stage 2 adds the IPC bridge to the UI; Stage 3 plugs
[`ProcessorChain`](../src/dsp/ProcessorChain.h) into `APOProcess`.

## Files

| File | Purpose |
|---|---|
| [Guids.h](Guids.h) | CLSID — must never change after first install |
| [TeeDspApo.h](TeeDspApo.h) / [.cpp](TeeDspApo.cpp) | The APO itself |
| [DllMain.cpp](DllMain.cpp) | DLL exports, class factory, `DllRegisterServer` |
| [TeeDspApo.def](TeeDspApo.def) | Export names |
| [CMakeLists.txt](CMakeLists.txt) | Build target — separate from the main app |
| [install/Install.ps1](install/Install.ps1) | Copy + register + bind to endpoint |
| [install/Uninstall.ps1](install/Uninstall.ps1) | Reverse |
| [install/SignDev.ps1](install/SignDev.ps1) | Self-sign for test-signing mode |
| [install/_Interop.ps1](install/_Interop.ps1) | Shared C# COM shim |

## First-time setup

```powershell
# 1. Build (top-level CMake, with TEEDSP_BUILD_APO=ON, the default)
cmake --build out/build/vs2022-local --target TeeDspApo --config Release

# 2. Enable test signing (one-time, requires reboot)
bcdedit /set testsigning on

# 3. Self-sign the DLL (elevated)
.\apo\install\SignDev.ps1

# 4. Reboot

# 5. Install — binds to current default render endpoint by default
.\apo\install\Install.ps1

# Or pick an endpoint explicitly
.\apo\install\Install.ps1 -List
.\apo\install\Install.ps1 -EndpointId "{0.0.0.00000000}.{...}"
```

After install, play audio. It should sound identical (pass-through). If
it doesn't:

1. Open Event Viewer → Applications and Services Logs → Microsoft →
   Windows → Audio. APO load failures and format-rejection events show
   up there with the full HRESULT.
2. Run [Process Explorer](https://learn.microsoft.com/sysinternals/downloads/process-explorer)
   and inspect `audiodg.exe`'s loaded modules. `TeeDspApo.dll` should be
   listed.
3. Confirm endpoint binding stuck:
   ```powershell
   . .\apo\install\_Interop.ps1
   [TeeDsp.Apo.Helpers]::ListRender()
   # then check the property store via certmgr-style tooling, or just
   # re-run Install.ps1 and watch for "endpoint already bound" patterns
   ```

## Uninstall

```powershell
.\apo\install\Uninstall.ps1            # unbinds default endpoint
.\apo\install\Uninstall.ps1 -All       # unbinds all active render endpoints
```

## Slot choice

We bind to the **MFX (Mode Effect)** slot — `PKEY_FX_ModeEffectClsid`,
PID 6 of `{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D}`. That puts us
**post-mix**: every stream into the endpoint hits us mixed together,
exactly once. SFX (per-stream pre-mix) would run TeeDSP separately for
every Spotify+browser+Discord stream — wrong shape for a master-bus
processor.

If MFX doesn't load on a given Windows configuration, the legacy GFX
slot (`PostMixCLSID`, PID 2) is the fallback and gets the same audio.

## Limitations (known, accepted at this stage)

- **Exclusive-mode** and **ASIO** streams bypass the audio engine and
  therefore bypass the APO. There is no fix for this — it's
  architectural.
- **AirPods Hands-Free mode** (HFP, 16k mono) is a separate endpoint
  from A2DP stereo. You'll see TeeDSP "stop working" when an app opens
  the mic and Windows switches endpoints. Not a concern per project
  notes — user does not use HFP mode.
- **No UI bridge yet.** Stage 2.

## What I haven't verified

This branch was authored without the ability to actually load the DLL on
the target machine. The first run on hardware is the test. Expect the
first install to surface at least one issue — most likely either:

- A signing-policy rejection in audiodg (visible in Event Viewer) →
  re-run `SignDev.ps1`, confirm test signing is on
- Format negotiation failure on a specific endpoint → check the
  `IsXxxFormatSupported` path in [TeeDspApo.cpp](TeeDspApo.cpp)
- Property-key value-type mismatch (`VT_LPWSTR` vs `VT_CLSID`) → if
  `Install.ps1` reports `0x80070057` (E_INVALIDARG) from `SetValue`,
  swap to a `VT_CLSID` payload
