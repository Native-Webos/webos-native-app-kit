# Third-party notices

| Component | Where | Licence | How it is used |
|---|---|---|---|
| [webosbrew/SDL-webOS](https://github.com/webosbrew/SDL-webOS) `release-2.30.12-webos.6` | fetched at build time by `cmake/SdlWebOS.cmake`; shipped as `lib/libSDL2-2.0.so.0` in webOS packages | zlib | Window, input, GL context, app registration |
| [PlxNative](https://github.com/GLinnik21/plx-native) © 2026 Gleb Linnik | `tools/mkipk.py` (derived from `ci/mkipk.py`), `tools/check-package.py` (modelled on `ci/check-package.py`, `ci/check-packaged-elf.py`); design of `logging` `open_log_file` and `platform` paths | GPL-3.0-or-later | Packaging and checks; attribution kept in file headers |
| webosbrew firmware symbol inventory ([dev-toolbox-cli](https://github.com/webosbrew/dev-toolbox-cli)) | read locally by `tools/fw-check.py`, `tools/fw-lib-matrix.py`; **not redistributed** | Apache-2.0 (tools); data describes LG firmware | Offline loadability checks |
| GNU GPL v3 text | `LICENSE` | — | Licence of this kit |

LG firmware libraries (EGL/GLES, Wayland, libhelpers, NDL, Starfish, …) are resolved on the TV at
runtime and are never redistributed. The NDK sysroot copies are used only for linking.
