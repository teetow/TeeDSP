# TeeDSP architecture

TeeDSP uses one Qt-free processor and one Qt editor across two hosts. The host
decides how audio reaches the processor; the editor decides how parameters and
telemetry are displayed. Neither host embeds the editor in its audio callback.

| Layer | Source | Windows | CM3588 |
| --- | --- | --- | --- |
| DSP and parameter IDs | `src/dsp/`, `src/shared/` | APO | CLAP plugin |
| Editor model and controls | `src/editor/DspController.*`, `src/ui/MainWindow.*`, `src/ui/widgets/` | Qt desktop | Qt WebAssembly |
| Editor transport | `src/editor/Transport.h` | `ApoTransport` | `webqt/Client` |
| Platform controls | `src/ui/MainWindowWindows.cpp` | endpoint, tray, recovery | — |
| Platform controls | `webqt/MainWindowRemote.cpp` | — | route, master volume |
| Audio routing | `apo/`, `src/host/` | Windows audio engine | — |
| Audio routing | `linux/` | — | PipeWire, AirPlay, CLAP host |

The processor's parameter contract lives in `src/shared/TeeDspParams.h`.
`DspController` holds the editable snapshot and produces changes; it does not
know whether the processor is local or remote. Each editor executable links one
implementation of `editor::createTransport`. Transports load accepted settings,
submit edits, return meters, and publish pre/post spectra. The APO adapter owns
shared memory, the heartbeat, persistence and FFT. The browser adapter owns HTTP
polling, sparse serialized patches and connection state. The shared Qt window
contains the EQ, dynamics, metering and spectrum widgets; only the top control
row and host status are platform-specific.

Windows routes system audio through an APO in `audiodg.exe`. CM3588 routes only
AirPlay through PipeWire into the CLAP host, then to the UA-25. The browser
editor never processes audio and may close without affecting playback. Master
volume lives after the DSP on CM3588 and is exposed through the browser adapter;
the EQ and dynamics parameters still belong to the common processor contract.

Build boundaries are deliberate: Windows `CMakeLists.txt` compiles the APO
transport and Windows window hooks; `webqt/CMakeLists.txt` compiles the HTTP
transport and remote window hooks. The common editor has no platform compile
switch. `webqt/tests/editor-model.cpp` checks that loading a saved snapshot does
not write defaults back to the host.
