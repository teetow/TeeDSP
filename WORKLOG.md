# WORKLOG — Low-latency / APO bringup

## Goal
Genuinely low-latency, system-wide processing via a system-effects APO: get
`TeeDspApo.dll` loaded into `audiodg.exe`. This is the right architecture; the
user-space bridge is a fallback, not a replacement.

## STATE — POC complete on Realtek (2026-06-20)
End-to-end works on the Realtek analog endpoint: APO loads + processes
system-wide; the real `TeeDsp.exe` UI drives the chain live; full analysis
(level/VU/GR meters, BS.1770 LUFS, pre/post spectrum + heatmap) is read from the
APO's shared telemetry. UI is the APO-era control surface (Device picker, status
line, master Bypass; bridge Start/Stop/Output-picker + capture→render engine
retired). Shared block is now **V4** (`Global\TeeDspApoSharedV4`): telemetry +
seqlock `ChainParams` + heartbeat + meters/LUFS + mono pre/post sample ring.
New since the `POC` tag — NOT committed yet.

**Next, in order:**
1. ✅ DONE — **Always-on persistence**: APO loads `dsp::ChainParams` from
   `C:\ProgramData\TeeDSP\params.bin` (`[magic][ChainParams]`) at stream start
   and processes regardless of the app running; heartbeat-gated bypass dropped;
   "off" = persisted `bypassed`. UI writes the file atomically (QSaveFile) on
   every change, plus the live shared block when connected. Verified: with the
   app killed (`uiAlive=0`), the APO still processes a stream (calls climbing,
   in≠out). Kill switches remain: relaunch+Bypass, or Sound→enhancements off.
2. ✅ DONE — **VB-Cable retired.** It was only the old bridge's capture device;
   the APO path never used it. User uninstalled it. The stale autostarting
   bridge app (`%LOCALAPPDATA%\Programs\TeeDsp`, 06-08 build) was eradicated:
   process killed, `HKCU\…\Run\TeeDsp` removed, directory deleted.
3. ✅ DONE — **Lean editor** (commit `c09f22d`): removed the in-app audio engine
   (AudioEngine + WASAPI capture/render/notifier/resampler) and the CLAP host
   from the app. `DspController` is APO-only (snapshot → shared block + params
   file; meters ← APO). Kept WasapiDevices, Fft, SpectrumAnalyzer, ApoSharedClient.
   `teedsp.clap` stays as a standalone DAW-plugin target.
4. ✅ DONE — **Boot autostart + tray** (commit `9bd197a`): editor deployed
   self-contained (windeployqt) to `%LOCALAPPDATA%\Programs\TeeDsp`, `HKCU\…\Run`
   re-pointed there; close-to-tray already worked. Tray icon lights (color vs
   grayscale) only when TeeDSP is bound to the **current default output** and not
   bypassed — read from that endpoint's MFX slot. NOTE: the autostart copy is the
   *installed* build; reinstall (copy exe+theme, windeployqt) to update it.
5. Minor polish: meters/spectrum freeze on idle (feed silence when the status
   poll sees no processing — UI-only).
5. **Real-output gate (off POC):** bind the APO to the endpoints actually used —
   Focusrite, then AirPods/Bluetooth ([[project_airpods_priority]]). TeeDSP
   currently only processes the Realtek endpoint.
6. Production signing (WHQL/attestation) so it loads into a *protected* audiodg;
   `DisableProtectedAudioDG` is dev-only.

## ✅ PROVEN: APO PROCESSES audio in audiodg (2026-06-20, later)
`APOProcess` is confirmed executing on real buffers inside audiodg, verified
deterministically (not by ear) via a cross-process telemetry block the APO
writes from its RT thread (`Global\TeeDspApoSharedV1`, see
[src/shared/TeeDspApoShared.h](src/shared/TeeDspApoShared.h)): `calls` and
`frames` climb in lock-step with playback (ch=2 sr=48000 bpf=8, ~480-frame/10 ms
buffers, `lastFlags=BUFFER_VALID`). The APO now links `teedsp_dsp` and runs a
real `dsp::ProcessorChain` (currently a hard-coded +15 dB 200 Hz low-shelf proof
config in `LockForProcess`).

**Critical bug fixed: COM aggregation.** The engine aggregates the APO; our
hand-rolled class factory returned `CLASS_E_NOAGGREGATION` (`0x80040110` — the
foobar "Unrecoverable playback error"). Fixed the factory to accept aggregation
AND fixed `NonDelegatingQueryInterface` to AddRef **through the returned pointer**
(public interfaces delegate to the controlling unknown) — calling
`NonDelegatingAddRef()` unconditionally corrupted the aggregated refcount and
crashed audiodg (symptom: `GetMixFormat` → `0x800706BE RPC_S_CALL_FAILED`).

**Dev deploy loop (fast, no re-sign):** audiodg is unprotected
(`DisableProtectedAudioDG=1`), so it loads an unsigned DLL. Repoint
`HKLM\SOFTWARE\Classes\CLSID\{B7E1A0C0-…}\InprocServer32` →
`C:\ProgramData\TeeDspDev\TeeDspApo.dll` (world-readable, audio service can read),
then per iteration: build → stop Audiosrv → copy DLL → start Audiosrv → probe.
Build target: `cmake --build out\build\apoverify --target TeeDspApo --config Release`.
Scratchpad scripts: `deploy-verify.ps1` (deploy + telemetry read).
NOTE: InprocServer32 now points at the dev path, NOT the DriverStore package —
restore it to the DriverStore DLL (or reinstall the package) for the "real" path.

## ✅ Milestone 2 (live UI control) — VERIFIED 2026-06-20
The UI drives the system-wide APO over shared memory. Verified with the real
`TeeDsp.exe` (normal-user/Medium integrity) + a live stream: `uiAlive=1`,
`paramGen==appliedGen` (UI→APO loop closed), heartbeat climbing ~20 Hz, process
counters climbing. Hard-coded proof config removed.

- Shared block v2 (`Global\TeeDspApoSharedV2`, src/shared/TeeDspApoShared.h):
  telemetry + seqlock `dsp::ChainParams` (UI→APO) + UI heartbeat.
- `dsp::applyChainParams` (src/dsp/ChainParamsApply.h) mirrors the CLAP plugin's
  mapping (notably `*Enabled` → `stage.setBypass(!on)`).
- APO: applies params when `paramGen` advances; **bypasses (passthrough) when
  the heartbeat goes stale** (UI not running) — so closing the app returns the
  endpoint to clean audio. Publishes `appliedGen`/`uiAlive` for verification.
- UI: `host::ApoSharedClient` + a 20 Hz timer in `DspController` opens the
  section lazily (the APO, a service, must create the Global section first),
  writes the snapshot on change, pulses the heartbeat.
- **Integrity gotcha:** audiodg runs at higher integrity than a user UI, so the
  section is created with SDDL `D:(A;;GA;;;WD)S:(ML;;NW;;;LW)` (everyone DACL +
  Low mandatory label) so a Medium-integrity UI can write to it.
- NOT yet committed (POC tag = pre-M2). Dev-grade: NULL-ish DACL + Low label are
  permissive; production should scope them and gate behind a debug flag.

**Next — endpoint coverage (the actual "drop VB-Cable" gate):** the APO is bound
only to the Realtek analog endpoint. To replace the loopback bridge it must be on
the endpoints actually used for output — Focusrite and (the killer use case)
AirPods/Bluetooth. Focusrite never accepted the binding earlier, but that was
before we understood the five gates + aggregation + `Disable_SysFx`; worth
retrying now. Per-endpoint extension INF binds the MFX slot.

## ✅ PROVEN: APO loads into audiodg (2026-06-20)
`teedspapo.dll` is confirmed **mapped into `audiodg.exe`** (PID 31864, read via
its module list), from
`…\teedspapocomponent.inf_amd64_661e2dbc6e8db947\teedspapo.dll`, in the effect
chain alongside `audioeng.dll` and Realtek's `RltkAPOU64.dll`. The system-effects
APO path works end-to-end. This was the gating blocker for the whole project.

**The full chain that made it load (all five are required):**
1. Signed componentized driver package (DriverStore + INF + `PETrust=true`) →
   the binding is accepted.
2. Realtek UAD `InterfaceSetting` extension → endpoint composite MFX slot
   `{d04e05a6-…},14 = {B7E1A0C0-…}` (TeeDSP) on the analog endpoint
   `{19e04a8d-8c1a-4ff5-b32b-a1b4ec03d013}`. (Digital Output still shows OEM
   `{A296D363-…}`, proving it's our extension.)
3. **`DisableProtectedAudioDG=1`** at
   `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio` → audiodg runs
   UNPROTECTED so it will load our test-signed (non-WHQL) DLL. The switch IS
   honored on this Win11 26200 build (verified: audiodg modules become readable,
   35 modules). Requires an `Audiosrv` restart for audiodg to relaunch
   unprotected; toggling other audio settings can relaunch it protected again.
4. **`Disable_SysFx=0`** on the endpoint (`PKEY_AudioEndpoint_Disable_SysFx`,
   `{1da5d803-…},5` in `…\FxProperties`). It had been left at `1` (enhancements
   OFF) — with it set, the engine skips ALL APOs on the endpoint regardless of
   binding/signing. Cannot be written via the registry even elevated (ACL allows
   only Audiosrv/AudioEndpointBuilder/SYSTEM/TrustedInstaller); set it via the
   Sound control panel "enhancements" toggle, which routes the write through the
   privileged audio service.
5. testsigning ON (from the earlier reboot) — still required for the package.

**Things ruled OUT as blockers along the way:** DLL bitness (it's x64), machine-
hive registration visibility (present in `HKLM\SOFTWARE\Classes`,
`ThreadingModel=Both`), `APOInterface0` value (`IAudioProcessingObject`
`{FD7F2B29}` is correct — same as MS SwapAPO), and signing/PPL rejection (proven
moot: even unprotected it didn't load until `Disable_SysFx` was cleared).

**Detection notes (how we proved it):** audiodg is normally a Protected Process
(PPL) → `tasklist /m` / `Get-Process.Modules` return 0. The kernel image-load
ETW (`Microsoft-Windows-Kernel-Process`, IMAGE keyword) is PPL-proof but needs
admin. With `DisableProtectedAudioDG=1`, audiodg is unprotected and its module
list is directly readable — the simplest proof. NOTE: the probe's own in-proc
`CoCreateInstance` loads the DLL into the *probe* process — that is NOT proof;
only a load into audiodg's PID counts.

## ⚠️ Production caveat / next steps
- `DisableProtectedAudioDG=1` is a DEV switch: it disables the protected/DRM
  audio path. NOT shippable. For production the APO package must be properly
  signed (WHQL / attestation / Store-signed) so it loads into PROTECTED audiodg.
- Stage-1 APO is pass-through; proving `APOProcess` actually runs on the buffers
  (vs just being loaded) is the next confirmation — needs Stage-2 instrumentation
  or an audible/measurable effect.
- Current machine dev state left ON: `DisableProtectedAudioDG=1`, analog endpoint
  `Disable_SysFx=0`, testsigning ON, audiodg unprotected. Revert switch:
  `reg delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio" /v DisableProtectedAudioDG /f`
  then restart Windows Audio.

## Installed packages (Driver Store)
- Component DLL: `…\teedspapocomponent.inf_amd64_661e2dbc6e8db947\teedspapo.dll`
  (its `InprocServer32` is what audiodg loads).
- Active Realtek extension: `teedsprealtekextension.inf_amd64_a9b39ac88c2538ff`.
- Stale Focusrite extensions + older component copy still present (harmless).
- Probe exe: `out\build\apoverify\apo\tests\Release\TeeDspApoProbe.exe`.

## Installed packages (Driver Store)
- Component DLL: `…\teedspapocomponent.inf_amd64_661e2dbc6e8db947\teedspapo.dll`
  (its `InprocServer32` is what audiodg would load).
- Active Realtek extension: `teedsprealtekextension.inf_amd64_a9b39ac88c2538ff`.
- Stale Focusrite extensions + older component copy still present (harmless).
- Probe exe: `out\build\apoverify\apo\tests\Release\TeeDspApoProbe.exe`.

## User-space bridge (fallback, uncommitted)
- Stable 50 ms render-ring target: primes queue, tracks queued frames lock-free,
  corrects clock drift only during silence (avoids pitch-warping live audio),
  logs measured bridge latency.
- Caveats: 50 ms is the app queue alone — the 20 ms render buffer + capture/device
  latency sit on top. Capture still requests a **1 s WASAPI buffer** — the main
  thing to revisit before calling this low-latency.

## Proven facts
- `APOInterface0 = {FD7F…}` is `IAudioProcessingObject` and is **correct** (an
  earlier "wrong GUID" hypothesis was false).
- Supported install model = **signed Audio Component driver package**: DLL in the
  Driver Store, INF-managed registration, `PETrust=true` — NOT an unsigned loose
  DLL in System32. Refs: Microsoft SysVad `ComponentizedApoSample.inx`, `swapapomfx.cpp`.
- Use Windows `RegisterAPO`/`UnregisterAPO` (now wired in `DllMain.cpp`), not
  hand-written registry metadata.
- Componentized package in [apo/driver](./apo/driver) passes Inf2Cat with zero
  errors/warnings; legacy loose-DLL installer is fail-closed.

## Hardware findings
**Focusrite** — dynamically registers KS interfaces. Real render endpoints:
`wr4400_8210` (44.1 kHz) / `wr4800_8210` (48 kHz) with `tr…` topology partners.
Recreates ~8 endpoints on device restart (single-endpoint checks unreliable).
Even with correct targeting, Endpoint Builder never projected FX / loaded the
APO; our package used the non-composite MFX slot (property 6). Restored to OEM MFX.

**Realtek** (better testbed) — analogue endpoint already runs a working
componentized **composite** chain: MFX (property 14), EFX (15), mode declarations
all projected into `FxProperties`. Real interfaces: `RearLineOutWave3` /
`SingleLineOutTopo`. Its OEM extension (`oem9.inf`) uses the **`InterfaceSetting`
table** to bind pre-existing KS interfaces (NOT `AddInterface`); the effects
component registers the APO globally via the Driver Store — the proven UAD
pattern, distinct from the SysVad proxy sample.

Realtek test package built to that exact pattern: signed `SoftwareComponent`
registering TeeDSP + separate extension mapping only `SingleLineOutTopo` to a
TeeDSP composite MFX. No AirPods/Focusrite touch; uninstall restores OEM binding.
Validated (build + Inf2Cat + signature) in `out\driver-package-realtek-r1`.

## Working rules (learned the hard way)
- No exploratory live registry/endpoint changes. Ground every change in MS docs +
  the actual installed driver topology.
- Cheap offline checks first (build, Inf2Cat, signature verify) before any install.
- Keep a clear rollback; only touch the machine at unavoidable reboot/install
  boundaries, one deliberate test at a time.
