# TeeDSP Scope

This repository is now scoped to TeeDSP only.

## Included functionality

1. APO runtime
- COM in-proc server registration and APO registration.
- Endpoint MFX binding via install/uninstall scripts.
- Real-time in-place processing in audiodg.

2. DSP processing
- Ordered chain: EQ -> Compressor -> Exciter.
- Parameter snapshot ingestion from shared memory.
- Meter and compressor gain-reduction telemetry publishing.

3. Configuration UI
- Dedicated TeeDSP config application.
- APO activation/repair workflow integration.
- Controls for bypass, EQ, compressor, and exciter.

## Excluded functionality

The repository does not include persistent spectrogram bars or loopback visualizer features.
