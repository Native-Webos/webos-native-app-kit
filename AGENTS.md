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
1. `tools/test.sh` (and `tools/test.sh host-asan` for memory-sensitive code). This includes renderer
   pixel tests on Mesa and end-to-end runs of Example A driven by `WNK_HELLO_KEYS`.
2. `tools/build-webos.sh`: the ARM build must succeed and its ipk must pass `check-package.py`.
3. `tools/verify.sh <ipk> --runtime-libs cmake/sdl-webos-runtime-libs.txt` must be 7/7 for 5.3.1+.
4. If the executable's libraries change, update `applications/**/expected-needed.txt` deliberately.
5. If `WNK_SDL_WEBOS_TAG` or SDL flags change, re-derive `cmake/sdl-webos-runtime-libs.txt`.

## Architecture rules (enforced by CMake target dependencies)
- `input/` core and `focus/` never include SDL or GL; only `input/sdl` and `render/gles2` do.
- Widgets/app code act on `wnk::input::Action`, never on raw scancodes. New key codes go into a keymap
  table with a `Provenance`; only codes captured on a TV may be marked `Measured`.
- All drawing goes through `wnk::render::Renderer` in logical units; GL state belongs to `Gles2Renderer`.

## Commits
- Follow `CONTRIBUTING.md`: imperative subject ≤ 72 chars saying what changed; body explains why and
  lists what was added; a verification paragraph at the evidence level actually reached.
- Never mention milestones/phases ("M3", "milestone") in commit messages.
- Commit only when the owner asks.

## Where things are
- Research and decisions: `docs/research/` (ARCHITECTURE-DECISIONS.md, PACKAGING-AND-FIRMWARE.md).
- Plan and status: `docs/MILESTONES.md`.
