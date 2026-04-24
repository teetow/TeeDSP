# APO Intent and Runtime Contract

## What the APO must do
1. Register as a COM in-proc server and AudioEngine APO.
2. Bind into endpoint FxProperties in MFX slots.
3. Negotiate compatible PCM/float formats for in-place processing.
4. Process interleaved audio buffers in real time with no blocking/heap use.
5. Apply DSP chain in order: EQ -> Compressor -> Exciter.
6. Publish post-DSP meter samples and compressor GR to shared memory.
7. Consume parameter snapshots from shared memory without RT-thread stalls.

## Reliability constraints
- Must survive endpoint DSP chain changes (e.g. Realtek effects toggles).
- Must not deadlock or spin indefinitely in RT callbacks.
- Must gracefully bypass on unsupported/edge formats.
- Must keep endpoint audio alive even when UI process is absent.

## Operational constraints
- For unsigned dev builds on Windows 11, audiodg protected mode may block DLL loading from non-system paths.
- Endpoint binding should prefer modern MFX slot key {d04e05a6...},14 and keep {...},6 for legacy fallback.
- Installation scripts must never leave audio services stopped after failures.

## Verification checklist
- COM CLSID exists under HKLM\SOFTWARE\Classes\CLSID\{B3A4C5D6-E7F8-4A1B-9C2D-3E4F5A6B7C8D}
- APO registration exists under HKLM\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\{B3A4...}
- Target endpoint FxProperties contains MFX binding in slot 14 (and optionally 6)
- APO log shows Initialize + LockForProcess + APOProcess activity during active playback
- Audible output responds to EQ/compressor/exciter parameter changes
