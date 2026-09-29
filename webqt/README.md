# Shared Qt editor in the browser

The browser builds the existing `MainWindow`, `DspController`, theme and all six
custom widgets with Qt 6.10.1 / Emscripten 4.0.7. This is WebAssembly with a small
HTML/JS loader, not a second implementation of the EQ. The editor model uses an
HTTP transport in the browser and an APO transport on Windows. Audio stays in the
ARM64 PipeWire service, including when the page closes.

At `http://teedsp.local/` the service redirects to `/qt/index.html` when the compiled
assets are installed. `/index.html` retains the lightweight UI. The Qt editor uses
the desktop layout; narrow screens scroll horizontally. It requires browser
WebAssembly and WebGL. Single-threaded Qt works on the existing LAN HTTP origin.
The first download includes roughly 14 MB of uncompressed Wasm.

## Included

- Original EQ response curves and dynamic gain reduction, drag frequency/gain,
  wheel Q, double-click EQ reset, right-click type selection and full band reset.
- Original knobs, per-band dynamics, compressor, exciter, stereo width, input and
  output levelers, spectral leveler, bypass, pre/post spectra and heatmap overlay.
- Stereo input/output peaks, output RMS and momentary LUFS, compressor reduction,
  per-band reduction and spectral corrections. Output readings follow master gain.
- Post-DSP master volume/mute and Music Assistant volume synchronization.

The CLAP parameter contract now permits Q up to 20 and compressor makeup down
to -12 dB, matching the existing Qt editor. Parameter IDs and saved values are
unchanged. Remote knob ranges also include values entered in the simple UI.

The Windows device picker, APO management/recovery and startup/tray controls are
replaced by the CM3588 route/status and master controls. They do not apply to this
remote DSP. The Qt heatmap overlay matches desktop; the scrolling waterfall remains
available in the simple UI.

Settings are loaded from the service before controls become editable. Opening a
page never pushes browser defaults. Changes are coalesced and serialized as sparse
parameter patches, preserving unrelated edits from another client. Failed writes
retry, and stale parameter responses cannot replace a newer gesture. The service
atomically persists accepted edits. Meter data is polled at 20 Hz; Qt interpolates
spectra and smooths the meters between updates.

## Build on WSL (x86 is fine for the browser target)

One-time SDK installation (outside the repo):

```sh
python3 -m venv ~/.cache/teedsp-wasm/venv
~/.cache/teedsp-wasm/venv/bin/pip install aqtinstall
~/.cache/teedsp-wasm/venv/bin/aqt install-qt all_os wasm 6.10.1 wasm_singlethread \
  -O ~/.cache/teedsp-wasm/Qt --archives qtbase
~/.cache/teedsp-wasm/venv/bin/aqt install-qt linux desktop 6.10.1 linux_gcc_64 \
  -O ~/.cache/teedsp-wasm/Qt --archives qtbase icu
git clone https://github.com/emscripten-core/emsdk.git ~/.cache/teedsp-wasm/emsdk
~/.cache/teedsp-wasm/emsdk/emsdk install 4.0.7
~/.cache/teedsp-wasm/emsdk/emsdk activate 4.0.7
bash webqt/build.sh
```

The script writes ignored assets to `linux/web/qt/`. It accepts `TEEDSP_QT_SDK`,
`TEEDSP_EMSDK` and `TEEDSP_WEB_BUILD` overrides. Keep the Qt/Emscripten versions
paired. Copy the source changes and generated assets to CM3588, then run
`docker compose build && docker compose up -d` there. The Docker image serves
these static files; there is no separate UI server. Build the native audio service
on ARM64. No Windows APO deployment is involved.

## Verification

`tests/params.cpp` checks all 69 IDs at defaults and both limits, plus gesture
clamping. `tests/editor-model.cpp` checks transport initialization, persistence
handoff, and updates from another client. Configure `webqt/tests` with the desktop
Qt SDK as `CMAKE_PREFIX_PATH`, build, and run CTest.

`tests/browser.cjs` runs the actual Wasm editor in Chromium against an intercepted
API: it never changes live audio settings. It covers startup without writes,
EQ drag/Q/reset/context menu, sparse patches, failed-save retry, external updates,
master mute, disconnect/reconnect, and a screenshot with synthetic meter data.
Use `PLAYWRIGHT_MODULE`, `CHROMIUM_PATH`, and optional `TEEDSP_URL` to select the
existing tools and deployment. The original Windows editor also builds unchanged
in its normal MSVC configuration.
