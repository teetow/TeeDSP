# TeeDSP

System-wide real-time DSP for Windows (Parametric EQ → Compressor → Exciter).
The chain runs as a system-effects **APO inside `audiodg.exe`**, always-on,
directly on the render endpoints (Realtek speakers, AirPods A2DP); `TeeDsp.exe`
is a lean editor that drives it live over shared memory and shows metering.
See [apo/README.md](apo/README.md) for architecture, deployment, and known
limitations.

> The user-space loopback bridge this README once described (capture from a
> virtual cable, process, re-render) was retired in commit `c09f22d` —
> VB-CABLE is no longer used or required.

![TeeDSP screenshot](image/screenshot.png)

## Requirements

- Windows 10/11
- Qt 6 (Core, Gui, Widgets)
- Visual Studio 2022 with the C++ workload (or any MSVC that can build Qt 6 apps)
- CMake 3.21+
- For the APO itself: Windows SDK (Inf2Cat/signtool) and the dev-signing setup
  described in [apo/driver/README.md](apo/driver/README.md); currently
  dev-signed only

## How it fits together

```
Apps → audio engine (audiodg.exe: TeeDSP APO — EQ → Comp → Exciter) → endpoint
             ▲ params + heartbeat / meters + telemetry ▼
                    TeeDsp.exe editor (shared memory)
```

The APO loads persisted parameters from `C:\ProgramData\TeeDSP\params.bin` at
stream start, so processing works with the editor closed. Deployment goes
through the componentized driver package (`scripts\deploy-apo.ps1` for the APO,
`scripts\publish.ps1` for the editor) — see [apo/README.md](apo/README.md).

## Build

Qt 6 must be discoverable by CMake. If it is not in a standard location (the
usual case on Windows), copy `CMakeUserPresets.json.example` to
`CMakeUserPresets.json`, update the Qt path inside it, and substitute
`vs2022-local` / `vs2022-local-release` for the preset names below.

```
cmake --preset vs2022
cmake --build --preset vs2022-release --parallel
```

The executable is `TeeDsp.exe` in the build output directory.

## Settings

Persisted as they change (never only on exit): UI state via `QSettings`
(per-user registry key `HKCU\Software\TeeDSP\TeeDSP`), and the DSP chain
baseline atomically to `C:\ProgramData\TeeDSP\params.bin`, which the APO
reads at stream start so settings apply even with the editor closed.
