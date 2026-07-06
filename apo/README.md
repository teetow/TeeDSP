# TeeDSP APO

TeeDSP's audio path. The full DSP chain runs system-wide as an
[Audio Processing Object](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/windows-audio-processing-objects)
inside `audiodg.exe` — there is no in-app audio engine. Processing is
always-on: the APO loads the persisted `dsp::ChainParams` baseline at stream
start, and the UI (when running) provides live control and metering over a
shared-memory block (`Global\TeeDspApoSharedV8`, see
[src/shared/TeeDspApoShared.h](../src/shared/TeeDspApoShared.h)) with a
heartbeat so the APO can tell whether a UI is alive.

Loaded and processing in `audiodg` since 2026-06-20; bound to AirPods since
2026-06-22.

## Files

| File | Purpose |
|---|---|
| [Guids.h](Guids.h) | CLSID — must never change after first install |
| [TeeDspApo.h](TeeDspApo.h) / [.cpp](TeeDspApo.cpp) | The APO itself |
| [DllMain.cpp](DllMain.cpp) | DLL exports, class factory, `DllRegisterServer` |
| [TeeDspApo.def](TeeDspApo.def) | Export names |
| [CMakeLists.txt](CMakeLists.txt) | Build target — separate from the main app |
| [driver/](driver/) | Component, Realtek and AirPods extension INFs; package staging and signing |
| [tests/](tests/) | Direct-render and COM-registration probe |
| [install/Uninstall.ps1](install/Uninstall.ps1) | Legacy loose-DLL cleanup only |

## Deployment

The supported path is a **componentized driver package**, not a loose DLL in
`System32` — see [driver/README.md](driver/README.md). For routine dev
iteration use `scripts\deploy-apo.ps1` from the repo root; rebuilding the
project alone never updates the Driver Store copy that `audiodg` actually
loads.

Device targeting is deliberate (see WORKLOG): the APO belongs on **Realtek**
(speakers) and **AirPods** (voice/media — the killer use case). The
**Focusrite is reserved for music production and never gets the APO**; a grey
tray on Focusrite is correct, permanent behavior.

The old scripts in `install/` are retained only to remove an existing legacy
installation. They are not the deployment mechanism for the componentized APO.

## Legacy cleanup

```powershell
.\apo\install\Uninstall.ps1            # unbinds default endpoint
.\apo\install\Uninstall.ps1 -All       # unbinds all active render endpoints
```

## Slot choice

The two endpoints bind differently, by necessity:

- **Realtek** — composite **MFX** (`PKEY_CompositeFX_ModeEffectClsid`, PID 14),
  following the Realtek UAD package's `InterfaceSetting` pattern. Post-mix:
  every stream into the endpoint hits us mixed together, exactly once — the
  right shape for a master-bus processor.
- **AirPods** — **SFX stream effect** (`FX\0`, `PKEY_FX_StreamEffectClsid`),
  because the inbox Bluetooth A2DP driver owns the `MSFX` slots and adding a
  second MFX breaks the graph on reconnect (see
  [driver/TeeDspAirPodsExtension.inf](driver/TeeDspAirPodsExtension.inf)).
  SFX is per-stream pre-mix, so concurrent streams each get their own APO
  instance — the shared telemetry block handles this (refcounted lock,
  meter-owner election).

## Limitations (known, accepted)

- **Exclusive-mode** and **ASIO** streams bypass the audio engine and
  therefore bypass the APO. There is no fix for this — it's architectural.
- **AirPods Communications-mode streams** (Zoom/Teams playback) currently
  bypass the APO: the v1.0.2 COMMUNICATIONS-mode SFX registration broke
  endpoint reuse on Bluetooth reconnect and was removed in v1.0.3. Comms
  processing needs a different mechanism.
- **AirPods Hands-Free mode** (HFP, 16k mono) is a separate endpoint
  from A2DP stereo. You'll see TeeDSP "stop working" when an app opens
  the mic and Windows switches endpoints. Not a concern per project
  notes — user does not use HFP mode.
- **Dev-signed only.** The package uses a self-signed dev cert; production
  needs a properly signed driver package. Known recurrence: `audiodg` can
  relaunch protected, which silently unloads the APO on every endpoint —
  recover with `scripts\Restart-AudioEngine.ps1` (elevated Audiosrv restart).

## Open investigation

AirPods can wedge silently on Bluetooth reconnect (plays a few seconds, then
nothing) with the endpoint GUID reused and active; endpoint churn was fixed by
extension v1.0.3 but the wedge was not. See WORKLOG for the current playbook.
