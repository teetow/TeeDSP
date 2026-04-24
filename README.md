# TeeDSP

TeeDSP is a Windows system-audio DSP project built around an APO (Audio Processing Object).

The repository contains two deliverables:

1. TeeDspApo
- In-process COM APO DLL loaded by the Windows audio engine (audiodg).
- Applies a real-time DSP chain in this order: EQ -> Compressor -> Exciter.
- Reads parameter snapshots and publishes meter data via shared memory.

2. TeeDspConfig
- Desktop configuration app for APO setup and DSP control.
- Provides controls for system activation, global bypass, EQ, compressor, and exciter.
- Persists user settings and pushes live updates to the APO through shared memory.

## Build

Use the existing CMake presets:

1. Configure
	cmake --preset vs2022

2. Build Debug
	cmake --build --preset vs2022-debug --parallel

3. Build Release
	cmake --build --preset vs2022-release --parallel

## Install / Remove APO

From an elevated PowerShell prompt:

1. Install or repair APO binding
	scripts/install_apo.ps1

2. Remove APO binding
	scripts/uninstall_apo.ps1

The installer targets the default Windows render endpoint unless an explicit endpoint GUID is provided.

## Unattended Validation

Run the default non-invasive validation flow:

powershell -ExecutionPolicy Bypass -File scripts/validate_stack.ps1

Try explicit APO target build and fail if unavailable:

powershell -ExecutionPolicy Bypass -File scripts/validate_stack.ps1 -TryApoTarget -FailOnMissingApo

Include install and uninstall lifecycle checks (requires elevation/UAC):

powershell -ExecutionPolicy Bypass -File scripts/validate_stack.ps1 -TryApoTarget -RunInstall -RunUninstall

Run contract checks without changing system state:

powershell -ExecutionPolicy Bypass -File scripts/validate_stack.ps1 -RunContractChecks

Run strict contract checks after install, including endpoint binding and mapping probe:

powershell -ExecutionPolicy Bypass -File scripts/validate_stack.ps1 -RunInstall -RunContractChecks -RequireEndpointBinding -ProbeSharedMapping -RequireSharedMapping

Require TeeDSP to be primary in slot 14 as well:

powershell -ExecutionPolicy Bypass -File scripts/validate_stack.ps1 -RunInstall -RunContractChecks -RequireEndpointBinding -RequireSlot14Primary -ProbeSharedMapping -RequireSharedMapping
