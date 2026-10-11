# webOS Native App Kit

A reusable foundation for **native** (C++/SDL/OpenGL ES) applications on retail LG webOS TVs:
build, package, verify, install and launch, with the platform knowledge from PlxNative,
moonlight-tv and the webosbrew projects turned into tools and checks.

Status: **Milestone 3 (rendering, input, focus)**. See [docs/MILESTONES.md](docs/MILESTONES.md) for what is
verified and what is not. Nothing here has run on a TV yet.

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
| `net/` | HTTP(S) client: libcurl loaded at runtime, cancellation, limits, retries, main-thread delivery |
| `image/` | Image service: fetch, PNG/JPEG decode, downscale, memory and disk caches, coalescing; textures via the renderer |
| `storage/` | Key/value store with revisions: DB8 on the TV, atomic files on the host |
| `render/` | Renderer interface, fonts/UTF-8/atlas packing (CPU), GLES2 backend in `render/gles2` |
| `ui/` | Widget toolkit: view tree, Row/Column/Grid layouts, Label, ImageView, Button, Card, virtualised lists/grids, ScrollView, dialogs, toasts, navigation stack, focus ring, animation helpers |
| `app/` | App shell (`wnk::app::run` + `Delegate`): SDL/GL setup, main loop, input routing, lifecycle state machine (Running ⇄ Suspended), timed shutdown under a watchdog, metrics overlay |
| `diagnostics/` | Frame timing (per loop phase), process memory, crash record (async-signal-safe; `tools/symbolize-crash.py` resolves it offline), capabilities (Supported/Unsupported/Unknown) and device info without identifiers |
| `applications/examples/hello/` | Example A: tile grid with focus ring, modal dialog, key log, device probe |
| `applications/examples/tvui/` | Example B: sidebar, poster rails, 1000-item grid, details screen, dialogs; `ui/` on the `app/` shell |
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
```

## Licence

GPL-3.0-or-later. See [LICENSING.md](LICENSING.md) and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
