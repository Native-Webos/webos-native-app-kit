# Licensing

The webOS Native App Kit is licensed under the **GNU General Public License, version 3 or (at
your option) any later version** (`GPL-3.0-or-later`). The full text is in `LICENSE`. This is the
same licence as [PlxNative](https://github.com/GLinnik21/plx-native), which the kit follows in design.

What this means for you:

- You may use, modify and redistribute the kit, including commercially, under the GPL's terms.
- **Applications built on the kit are derivative works and must also be distributed under a
  GPL-3.0-compatible licence**, with complete corresponding source, when you distribute them.
- Every source file the kit adds carries `SPDX-License-Identifier: GPL-3.0-or-later`.
- Code taken from other projects keeps its original copyright notice and licence and is listed in
  `THIRD-PARTY-NOTICES.md`. Anything copied from PlxNative is attributed to its authors in the file header.
- LG's firmware libraries (Starfish, NDL, ACB, system SDL, EGL/GLES, Luna…) are resolved on the TV at
  runtime and are never redistributed. Like PlxNative, we treat them as System Libraries under GPL section 1.
- Every package built with `wnk_add_webos_package` carries `licenses/`: this GPL text,
  `THIRD-PARTY-NOTICES.md`, the licence of each component compiled in or bundled (SDL, stb,
  nlohmann/json, curl headers, and FFmpeg's LGPL where its libraries ship) and `SOURCE.txt`.
  `tools/check-package.py` refuses a package without them.
- **When you distribute an app, publish its complete source and set `SOURCE_URL`** (or
  `-DWNK_SOURCE_URL=...`) so `licenses/SOURCE.txt` names it. For packages with FFmpeg, `SOURCE.txt`
  also names the exact FFmpeg tarball and checksum and points to `tools/build-ffmpeg.sh`; offering that
  tarball with your source satisfies LGPL-2.1 §6.

Audit (2026-10-12): every bundled or compiled-in component is GPL-3.0-compatible (zlib, MIT or public
domain, OFL-1.1 for fonts, LGPL-2.1-or-later for FFmpeg built without `--enable-gpl` or
`--enable-nonfree`); nothing GPL-incompatible or proprietary is in a package; LG's libraries and
libcurl are used from the TV only. Network test streams and the TVmaze catalogue are fetched at run
time and never packaged (TVmaze data is CC BY-SA and credited on Example C's About screen).

