# TeeDSP Implementation Notes

TeeDSP uses the native APO path only.

## Runtime model

1. APO DLL
- Loaded by audiodg as an MFX APO.
- Registers as COM in-proc server and audio engine APO.
- Processes interleaved PCM/float buffers in place.

2. DSP chain
- Processing order: EQ -> Compressor -> Exciter.
- Hard real-time constraints apply in APOProcess:
  - no blocking
  - no heap allocation
  - bounded, deterministic CPU

3. App-to-APO control path
- TeeDspConfig writes parameters to shared memory using seqlock snapshots.
- APO reads snapshots opportunistically and applies updates between process calls.
- APO publishes meter samples and compressor gain-reduction telemetry.

## Operational constraints

1. Endpoint registration
- Installer writes MFX endpoint properties and keeps compatibility fallback keys.
- Audio services are restarted after install/uninstall actions.

2. Reliability
- Unsupported formats must bypass safely instead of failing audio.
- APO must keep output alive even when the UI is closed.

3. Deployment
- Unsigned development builds may fail to load in protected audio paths depending on local policy.