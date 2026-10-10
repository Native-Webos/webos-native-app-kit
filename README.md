# webOS Native App Kit

A reusable foundation for **native** (C++/SDL/OpenGL ES) applications on retail LG webOS TVs:
build, package, verify, install and launch, with the platform knowledge from PlxNative,
moonlight-tv and the webosbrew projects turned into tools and checks.

Status: **Milestone 2 (native runtime)**. See [docs/MILESTONES.md](docs/MILESTONES.md) for what is
verified and what is not. Nothing here has run on a TV yet.

## Quick start

```sh
tools/doctor.sh                       # what this machine has; installs nothing
tools/test.sh                         # host build + unit tests (C++ and Python)
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
```

Set `ARES_DEVICE` to choose a TV. Set `WEBOS_NDK` if the NDK is not in `~/webos-ndk/…` or `/opt/…`.

## What is here

| Path | Contents |
|---|---|
| `logging/` | Thread-safe structured logger, secret redaction, safe log files (0640, no symlinks) |
| `platform/` | Process setup, launch params, app/runtime paths, device and storage probe |
| `applications/examples/hello/` | Example A: GLES2 window, key capture, lifecycle log, device probe |
| `cmake/` | Target detection, SDL-webOS build, `wnk_add_webos_package()` |
| `tools/` | `mkipk.py` (reproducible ipk), `check-package.py`, `fw-check.py`, shell tools |
| `tests/` | C++ unit tests and Python packaging tests |
| `docs/research/` | Evidence the design is based on, with sources |

## Licence

GPL-3.0-or-later. See [LICENSING.md](LICENSING.md) and [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
