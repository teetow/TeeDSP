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
| [driver/](driver/) | Component and Focusrite extension INFs, package staging and signing |
| [tests/](tests/) | Direct-render and COM-registration probe |
| [install/Uninstall.ps1](install/Uninstall.ps1) | Legacy loose-DLL cleanup only |

## Deployment

The supported path is a **componentized driver package**, not a loose DLL in
`System32`. The package supplies an AudioProcessingObject software component
and a device-extension INF which associates it with the target endpoint.

For the Focusrite development device, build and stage the package with the
files in [`driver/`](driver/). The staging script validates both INFs with
Inf2Cat; installation remains a separate, deliberate driver-stack change.

The old scripts in `install/` are retained only to remove an existing legacy
installation. They are not the deployment mechanism for the componentized
APO.

## Legacy cleanup

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

## Limitations (known, accepted at this stage)

- **Exclusive-mode** and **ASIO** streams bypass the audio engine and
  therefore bypass the APO. There is no fix for this — it's
  architectural.
- **AirPods Hands-Free mode** (HFP, 16k mono) is a separate endpoint
  from A2DP stereo. You'll see TeeDSP "stop working" when an app opens
  the mic and Windows switches endpoints. Not a concern per project
  notes — user does not use HFP mode.
- **No UI bridge yet.** Stage 2.

## What remains to verify

The componentized package now passes Inf2Cat validation. It has not yet been
test-signed and installed, so `audiodg.exe` load and processing remain the
next hardware-validation gate. That installation is intentionally deferred
until the package certificate and test-signing/reboot requirements are ready.
