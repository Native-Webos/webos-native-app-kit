# Instructions for coding agents

## Ground rules
- Evidence first: platform behaviour must come from source, firmware inventory or a device run, never
  from a symbol name or a web-app example. Tag claims as in `docs/research/README.md`.
- Never claim device behaviour without a TV record (model, webOS release, date).
- The TV is Developer Mode, **not rooted**: no root ssh, no scp deploy. Every device run is
  build → package → `tools/install.sh` → `tools/launch.sh`.
- Licence is GPL-3.0-or-later. Code taken from elsewhere keeps its header and is listed in
  `THIRD-PARTY-NOTICES.md`. webosbrew/samples has no licence: read only.
- Firmware-variable libraries are `dlopen`ed, never added to `DT_NEEDED`. Never bundle libstdc++
  (link it statically) or any LG library.

## Gates before handing over a change
1. `tools/test.sh` (and `tools/test.sh host-asan` for memory-sensitive code).
2. `tools/build-webos.sh`: the ARM build must succeed and its ipk must pass `check-package.py`.
3. `tools/verify.sh <ipk> --runtime-libs cmake/sdl-webos-runtime-libs.txt` must be 7/7 for 5.3.1+.
4. If the executable's libraries change, update `applications/**/expected-needed.txt` deliberately.
5. If `WNK_SDL_WEBOS_TAG` or SDL flags change, re-derive `cmake/sdl-webos-runtime-libs.txt`.

## Where things are
- Research and decisions: `docs/research/` (ARCHITECTURE-DECISIONS.md, PACKAGING-AND-FIRMWARE.md).
- Plan and status: `docs/MILESTONES.md`.
