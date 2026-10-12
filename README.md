# webOS Native App Kit

A reusable foundation for **native** (C++/SDL/OpenGL ES) applications on retail LG webOS TVs:
build, package, verify, install and launch, with the platform knowledge from PlxNative,
moonlight-tv and the webosbrew projects turned into tools and checks.

Status: the platform layers, the media player and four reference apps (A–D) build, pass their host
tests and run on an LG OLED65C1PTB (webOS 6.5.3) in Developer Mode. See [docs/MILESTONES.md](docs/MILESTONES.md)
for what is verified and what is not, [docs/compatibility/records.json](docs/compatibility/records.json) for each
TV result, and [docs/AGENT-APP-GUIDE.md](docs/AGENT-APP-GUIDE.md) to build an app on the kit.

## Quick start

```sh
tools/doctor.sh                       # what this machine has; installs nothing
tools/test.sh                         # host build + unit tests (C++ and Python); also host-asan, host-tsan
tools/build-webos.sh                  # ARM build + build/webos-release/dist/*.ipk (checked)
tools/verify.sh build/webos-release/dist/org.wnk.examples.hello_0.1.0_arm.ipk \
    --runtime-libs cmake/sdl-webos-runtime-libs.txt
```

On a **Developer Mode** TV (no root needed), after installing
[ares-cli-rs](https://github.com/webosbrew/ares-cli-rs/releases) and running `ares-setup-device`:

```sh
tools/install.sh build/webos-release/dist/org.wnk.examples.hello_0.1.0_arm.ipk
tools/launch.sh org.wnk.examples.hello
tools/logs.sh org.wnk.examples.hello     # or download /tmp/org.wnk.examples.hello/events.log with Dev Manager
tools/device-test.sh                     # run the platform-independent unit tests on the TV's CPU
```

Set `ARES_DEVICE` to choose a TV. Set `WEBOS_NDK` if the NDK is not in `~/webos-ndk/…` or `/opt/…`.

## What is here

| Path | Contents |
|---|---|
| `logging/` | Thread-safe structured logger, secret redaction, safe log files (0640, no symlinks) |
| `platform/` | Process setup, launch params, app/runtime paths, device and storage probe, Luna calls |
| `core/` | Shared geometry (logical 1920x1080 units) |
| `input/` | Raw key → semantic `Action` mapping with per-entry provenance; SDL adapter in `input/sdl` |
| `focus/` | Focus manager: directional navigation, explicit neighbours, scopes, modal trapping, history |
| `async/` | Cancellation tokens, worker pool, main-thread dispatcher, lifetime guards |
| `net/` | HTTP(S) client: libcurl loaded at runtime, cancellation, limits, retries, main-thread delivery; WebSocket client (RFC 6455 over curl CONNECT_ONLY) |
| `image/` | Image service: fetch, PNG/JPEG decode, downscale, memory and disk caches, coalescing; textures via the renderer |
| `storage/` | Key/value store with revisions: DB8 on the TV, atomic files on the host; secret store (keymanager3 AES-GCM where available) |
| `render/` | Renderer interface, fonts/UTF-8/atlas packing (CPU), GLES2 backend in `render/gles2` |
| `i18n/` | Locale (TV settings, `LANG`), string catalogs with CLDR plural forms, placeholders, number formatting |
| `audio/` | App sound: WAV and Ogg Vorbis decoding to 48 kHz float, a mixer (gain, pan, loops, streams), output through SDL audio (PulseAudio/ALSA on the TV) |
| `ui/` | Widget toolkit: view tree, Row/Column/Grid layouts, Label, ImageView, Button, Card, TextField, form rows (Toggle, Slider, Choice, ValueRow, Tabs, TextView), QrCode, virtualised lists/grids, ScrollView, dialogs, toasts, navigation stack, focus ring, animation helpers |
| `app/` | App shell (`wnk::app::run` + `Delegate`): SDL/GL setup, main loop, input routing, lifecycle state machine (Running ⇄ Suspended), timed shutdown under a watchdog, metrics overlay |
| `diagnostics/` | Frame timing (per loop phase), process memory, crash record (async-signal-safe; `tools/symbolize-crash.py` resolves it offline), capabilities (Supported/Unsupported/Unknown) and device info without identifiers |
| `services/auth/` | Optional sign-in service: credentials sealed through `SecretStore`, account list, session state, authorized requests with single-flight token refresh, sign-out |
| `media/` | Media model, playback controller, FFmpeg source (bundled FFmpeg via `tools/build-ffmpeg.sh`), NDL v2 backend, no-output pacing backend |
| `applications/examples/hello/` | Example A: diagnostics without the app shell: tile grid with focus ring, modal dialog, key log, device and network probes |
| `applications/examples/tvui/` | Example B: sidebar, poster rails, 1000-item grid, details screen, dialogs, a settings page of form controls and a QR code; `ui/` on the `app/` shell |
| `applications/examples/shows/` | Example C: networked app on the TVmaze API: paged grid, search, details, cancellation, error states, saved copy and favourites in DB8 |
| `applications/examples/player/` | Example D: playback of bundled A/V-sync clips (`tools/make-test-clips.sh`) and public DASH/HLS streams through Starfish or NDL, with seek, pause, and an audio and subtitle track menu |
| `cmake/` | Target detection, SDL-webOS build, `wnk_add_webos_package()` |
| `tools/` | `mkipk.py` (reproducible ipk), `check-package.py`, `fw-check.py`, shell tools |
| `tests/` | C++ unit tests and Python packaging tests |
| `docs/research/` | Evidence the design is based on, with sources |

## Trying the demo on the host

```sh
build/host-debug/applications/examples/hello/wnk-hello      # arrows, Enter, Escape (Back), F12 (Exit)
# scripted, headless: inject keys and save a screenshot
SDL_VIDEODRIVER=offscreen WNK_HELLO_KEYS=Right,Down,Return WNK_HELLO_SHOT=/tmp/shot.bmp \
    build/host-debug/applications/examples/hello/wnk-hello
build/host-debug/applications/examples/tvui/wnk-tvui        # Example B; WNK_TVUI_KEYS / WNK_TVUI_SHOT likewise
build/host-debug/applications/examples/shows/wnk-shows      # Example C (needs the network); WNK_SHOWS_KEYS / _SHOT
build/host-debug/applications/examples/player/wnk-player    # Example D (no picture on the host: pacing backend); WNK_PLAYER_KEYS / _SHOT
```

## Releasing

`tools/release.sh --check-reproducible` builds every package from a clean tree with the tests, package
and firmware checks, the source archive, the FFmpeg source and generated release notes. See
[docs/RELEASING.md](docs/RELEASING.md).

## Licence

GPL-3.0-or-later. See [LICENSING.md](LICENSING.md) and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md); every package
carries its licence texts in `licenses/`.
