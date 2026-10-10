# Licensing and reuse

Licence files were read directly from each repository at the pinned revisions `[SRC]`. This is not
legal advice. It records what the kit will and will not do.

| Project | Licence | Kit policy |
|---|---|---|
| PlxNative | GPL-3.0-or-later | **Reusable** (the kit is GPL-3.0-or-later). Copied files keep their copyright header and are listed in THIRD-PARTY-NOTICES. Rust code is not directly usable in a C++ kit; C files, Python tools and CMake/Make logic are. |
| moonlight-tv | GPL-3.0 | Reusable with notice if needed. Check each file: if it is GPL-3.0-*only*, the combined work becomes GPL-3.0-only. |
| mariotaku/ss4s | LGPL-3.0 | Compatible with GPL-3.0; could be used. Not planned. |
| webosbrew/samples | **No licence file** → all rights reserved by default | **Study only.** Contact the authors if reuse is ever wanted. |
| punktfunk client-webos | MIT OR Apache-2.0 | Reusable with notice. The glibc shim ideas are generic; reimplement rather than copy unless needed. |
| lgnome | MIT | Reusable with notice. |
| mariotaku/commons-c | MIT | Reusable with notice (not planned). |
| webosbrew/SDL-webOS | zlib | **Planned dependency** (bundled `libSDL2`), with notice in THIRD-PARTY-NOTICES. |
| webosbrew/webos-userland | Unlicense | Headers/struct layouts usable. |
| webosbrew/dev-toolbox-cli | Apache-2.0 | Tooling; may be built and invoked. Its firmware symbol **data** describes LG firmware and is used locally, not redistributed. |
| webosose/samples | Apache-2.0 | Background reference only (OSE ≠ retail). |
| LG firmware libraries (`libNDL_directmedia`, `libplayerAPIs`, `libAcbAPI`, system SDL, EGL/GLES…) | Proprietary/LG | **Never redistributed.** Resolved at runtime from the TV. NDK stubs are used only to link. |

## Planned third-party components (to be confirmed per milestone)

| Component | Licence | Use |
|---|---|---|
| SDL-webOS 2.30.x | zlib | Window, input, GL context, audio |
| stb_truetype, stb_image | MIT / public domain (dual) | Glyph rasterisation, PNG/JPEG decode |
| A bundled UI font (for example Noto Sans or Inter) | SIL OFL 1.1 | Text |
| doctest (or similar single-header test framework) | MIT | Host unit tests |
| JSON library (yyjson or nlohmann/json) | MIT | JSON parsing |
| FFmpeg libavformat/libavcodec (M7, optional) | LGPL-2.1+ when built without GPL parts | Demuxing for media playback. Must be a shared library or otherwise meet LGPL relinking terms. |

**Kit licence (decided 2026-10-11): GPL-3.0-or-later**, see `../../LICENSING.md`. Every planned dependency above (zlib, MIT, public domain, OFL, LGPL-2.1+, Apache-2.0) is compatible with GPLv3. webosbrew/samples still has no licence and remains study-only.
