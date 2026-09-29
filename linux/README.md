# TeeDSP on CM3588

Headless ARM64 service, built with Docker on CM3588. It runs the existing
TeeDSP CLAP implementation in a minimal static host, retaining all 69 parameter
IDs, defaults and telemetry. Windows APO/editor targets are unchanged.

## Use

- Shared Qt web editor: `http://teedsp.local` (fallback `http://cm3588.lan:8790`).
  See [webqt/README.md](../webqt/README.md) for the WebAssembly build and parity.
  The simple UI is still available at `/index.html`.
- AirPlay to the existing Shairport Sync receiver; its stereo output is routed
  through TeeDSP to the EDIROL UA-25. havoice remains directly connected.
- Master volume is applied **after** DSP, including in bypass; it persists in
  `linux/data/volume.json`. A fresh installation starts at zero volume.
- The top button toggles bypass. Controls save automatically to
  `linux/data/params.json`; closing the browser does not stop audio processing.
- The Qt editor has the original interactive EQ, dynamics meters, stereo peaks,
  LUFS, spectra and heatmap overlay. The simple UI retains its scrolling waterfall.
  Both connect to the same parameters, meters and post-DSP master controls.

## Build/deploy

The deployment checkout is `/home/teetow/apps/teedsp` on `cm3588.lan`.
Deploy from a clean WSL TeeDSP checkout. The command builds the Qt browser editor
locally, runs its model tests, pushes the source commit to the private NAS Git
remote, fast-forwards the CM3588 checkout, and builds a release-tagged ARM64 image
there. It switches containers only after the image build and Dockerfile tests pass,
then runs the browser interaction test against the deployed page. A failed health
or browser check restores the previous image.

```sh
bash scripts/deploy-cm3588.sh
bash scripts/deploy-cm3588.sh rollback
```

The release tag combines the source commit and browser-bundle hash. The active
and previous tags are recorded in ignored `linux/data/deploy-state`; the old
Docker image stays tagged for rollback. The deployment checkout must be clean
apart from ignored runtime files. `.env`, `linux/data/params.json`, and
`linux/data/volume.json` stay on the host across releases. The original Windows
workspace and APO deployment are separate.

For a first installation, create `.env` from `.env.example`, set the MQTT
credentials, create `linux/data` owned by uid 1001, and run
`docker compose build && docker compose up -d` on CM3588. Later releases use the
script above. `linux/teedsp.code-workspace` remains available over Remote SSH
for investigation.

The container runs as `1001:1001`, using the existing host PipeWire server.
The user's runtime directory is mounted read-only so socket replacement after
a PipeWire restart is visible. There is no `/dev/snd` passthrough, privileged
container, PulseAudio server, or change to the system default sink/source.
Host PipeWire/WirePlumber owns the hardware. Audio is fixed at 48 kHz stereo,
matching the UA-25 and host graph. The web port is published once, so the
CM3588's existing vibe-routes service creates the Caddy/mDNS route automatically.

The route supervisor repairs selected stream links when a stream or device
returns. An orderly stop restores direct AirPlay playback before terminating
the DSP process, copying master attenuation onto the AirPlay source mixer first.
Starting TeeDSP restores that mixer to unity once DSP links are in place.
A DSP child failure also attempts restoration before Docker
restarts the service. Link switching can cause a brief overlap; bypass is the
appropriate way to audition the processing while music is playing.

```sh
docker compose stop       # restore direct AirPlay playback
docker compose start      # insert TeeDSP again
```

Settings are atomically replaced on disk. HTTP validates IDs, types and ranges;
parameter snapshots cross to the audio thread using a nonblocking try-lock.
FFT and HTTP work are outside the realtime callback; its sample buffers are
preallocated. Visualization frames can be dropped without affecting audio.
The UI is intended for the trusted home LAN and has no login.

## Checks

The Docker build runs the existing leveler regression suite and headless tests
for bypass, stereo handling, parameter processing, and API input validation.
Routing tests can run without a PipeWire socket or network:

```sh
docker compose run --rm --no-deps --entrypoint python3 \
  -v "$PWD/linux:/tests:ro" teedsp -m unittest discover -s /tests/tests -p 'test_*.py'
```

`/api/health` reports the PipeWire connection. `/api/meters` includes current
routing, processed frame counts, spectrum and metering. A healthy container
alone does not prove an audible source is currently playing.

Verified on CM3588, 2026-09-28: native ARM64 Docker build; both C++ test
executables; seven routing tests; offline container test run; desktop/mobile
Playwright controls, save/restore and invalid-input checks; automatic
`teedsp.local` routing; 48 kHz / 1024-frame live processing; orderly stop
restoring both direct AirPlay links; restart restoring DSP links and preserving
the settings file. User confirmed audible playback. Master-volume checks cover
MASS/HA/TeeDSP feedback, mute/unmute level restoration, bypass, browser controls
and restart preservation.

## Music Assistant volume

CM3588 Speaker (`upb802c983ae07`) uses MASS's Home Assistant provider with
`number.teedsp_master_volume` as its external volume control. MQTT credentials
are in the ignored deployment `.env`; copy `.env.example` when setting up a
new host. The AirPlay child (`ap3bc08728a661`) stays at native volume 100, so
changing MASS volume no longer changes the level entering the compressor.
The TeeDSP slider, HA number and MASS slider synchronize in both directions.

MASS mute uses its built-in `fake` (mute via volume) control: master volume goes
to zero and unmute restores the previous level. This avoids the installed MASS
HA provider's inverted switch mute commands. The HA `switch.teedsp_mute` and
web Mute button separately control TeeDSP's master mute; these do not synchronize
MASS's mute flag. Unmute using the control that originally muted playback.

Master gain ramps over 10 ms. The slider uses 50% = -10 dB, 25% = -20 dB,
and 0% = silence. Other AirPlay clients retain their own upstream volume control.
