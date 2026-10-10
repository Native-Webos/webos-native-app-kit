# Third-party notices

| Component | Where | Licence | How it is used |
|---|---|---|---|
| [webosbrew/SDL-webOS](https://github.com/webosbrew/SDL-webOS) `release-2.30.12-webos.6` | fetched at build time by `cmake/SdlWebOS.cmake`; shipped as `lib/libSDL2-2.0.so.0` in webOS packages | zlib | Window, input, GL context, app registration |
| [PlxNative](https://github.com/GLinnik21/plx-native) © 2026 Gleb Linnik | `tools/mkipk.py` (derived from `ci/mkipk.py`), `tools/check-package.py` (modelled on `ci/check-package.py`, `ci/check-packaged-elf.py`); design of `logging` `open_log_file` and `platform` paths | GPL-3.0-or-later | Packaging and checks; attribution kept in file headers |
| webosbrew firmware symbol inventory ([dev-toolbox-cli](https://github.com/webosbrew/dev-toolbox-cli)) | read locally by `tools/fw-check.py`, `tools/fw-lib-matrix.py`; **not redistributed** | Apache-2.0 (tools); data describes LG firmware | Offline loadability checks |
| [stb](https://github.com/nothings/stb) commit `2c980bb59875b0d32144a71867fbdebb2f77cd20`: stb_truetype v1.26, stb_image v2.30, stb_image_write v1.16 | `third_party/stb/` (+ `LICENSE`) | MIT or public domain (choice) | Glyph rasterisation (`render/`), PNG/JPEG decoding (`image/`, PNG and JPEG only), test image generation (tests only) |
| [Atkinson Hyperlegible](https://brailleinstitute.org/freefont) Regular and Bold, © 2020 Braille Institute of America; from google/fonts commit `bd8f81ddb5c74d5c8897b36ad88b440266245103` | `assets/fonts/` (+ `OFL.txt`), shipped in packages under `fonts/` | SIL Open Font License 1.1 | UI text |
| [nlohmann/json](https://github.com/nlohmann/json) v3.11.3 (`json.hpp` sha256 `9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6`) | `third_party/nlohmann/` (+ `LICENSE.MIT`) | MIT | JSON for DB8 calls and tests |
| [curl](https://curl.se) 7.87.0 headers (tag `curl-7_87_0`; `curl.h` identical to the NDK's) | `third_party/curl/include/curl/` (+ `COPYING`) | curl licence (MIT/X-style) | Types and constants for libcurl, which is loaded from the device at runtime and not redistributed |
| GNU GPL v3 text | `LICENSE` | — | Licence of this kit |

LG firmware libraries (EGL/GLES, Wayland, libhelpers, NDL, Starfish, …) are resolved on the TV at
runtime and are never redistributed. The NDK sysroot copies are used only for linking.
