# Project comparison

Evidence tags: see [README.md](README.md). Link prefixes used below:

- PLX = `https://github.com/GLinnik21/plx-native/blob/c8553f02102c0060601a748a48063d3e7fb81d4c/`
- MLT = `https://github.com/mariotaku/moonlight-tv/blob/cf96ecc72cdab3aae7ba0698f463b89065ae7e3b/`
- PF = `https://github.com/punktfunk/client-webos/blob/c85b6c9d4af34747815294b390e81dbcfeef5925/`
- LGN = `https://github.com/truebest/lgnome/blob/3882ae4ea50ead3ea53d022aa1f2a9d80396cd11/`
- SDLW = `https://github.com/webosbrew/SDL-webOS/blob/263629dab0c89e75f9872eed66e197174952ce02/`
- SMP = `https://github.com/webosbrew/samples/blob/e95eed3afa7896894f811b84d8e9e277e9e2b74c/`
- SS4S = `https://github.com/mariotaku/ss4s/blob/dfba721b85420ccabf91dac65be73984bf1865f9/`
- OSE = `https://github.com/webosose/samples/blob/df5fd71410733d120315321e30909e08864f0155/`

## Summary table

| | PlxNative | moonlight-tv | punktfunk client-webos | lgnome | webosbrew/samples | webosose/samples |
|---|---|---|---|---|---|---|
| Kind | Plex client | Game streaming | Game/desktop streaming | RDP client | API samples | OSE samples |
| Language | Rust (~490k lines) + C boot shim | C | Rust | C (+ Rust RDP staticlib) | C | C / C++ / Qt |
| Build | Makefile + cargo (nightly, `-Z build-std`) | CMake + CPack | cargo + Taskfile | CMake + Docker | CMake | CMake |
| Window / input | LG **system** SDL2, pinned 2.0.4 headers | **Bundled** webosbrew SDL 2.30.12-webos.6 | **Bundled** webosbrew SDL3 3.4.16-webos.5 | System SDL2 from NDK sysroot | System SDL2 | Raw Wayland + `wl_webos_shell` |
| UI | Own GPU UI (Rust) | LVGL on `SDL_Renderer` | punktfunk shared shell (Skia on GL) | LVGL | none | none |
| Video | Starfish (`libplayerAPIs`) ES feed + ACB / exported window; bundled FFmpeg demuxer | ss4s modules (NDL, SMP, LGNC) | NDL DirectMedia v2/v1 (dlopen) | NDL DirectMedia (dlopen) | SMP, NDL, LGNC | — |
| Minimum webOS | 4.0 | 3.x (via modules) | 5 (v1 path for 3.5–4) | not stated in what was read | 1–2 per sample | OSE only |
| Licence | GPL-3.0+ | GPL-3.0 | MIT / Apache-2.0 | MIT | none | Apache-2.0 |
| Reuse here | Study only | Study only | Code reusable with notice | Code reusable with notice | Study only | Code reusable with notice |

## Per project

### PlxNative (`../../../plx-native`)

- **Entry point.** `src/main.c` `main()` opens the event/crash/stderr logs with `O_NOFOLLOW`,
  installs the crash tracer, sets `SDL_WEBOS_ACCESS_POLICY_KEYS_BACK=true` with `setenv`, then
  calls the Rust `plex_run()` (PLX`src/main.c`) `[SRC]`.
- **Video plane.** `src/starfish.c` `vp_mode()` dlopens `libAcbAPI.so.1` (webOS 2.2.3–4.10). If that
  fails, it dlsyms `SDL_webOSCreateExportedWindow`/`SDL_webOSSetExportedWindow` from the loaded
  SDL (webOS 5.3.1–11.2). The source states this from 14 firmware inventories `[SRC]`, and our
  matrix agrees (`libAcbAPI.so.1` absent from 5.3.1 on) `[FWDB]`. The file comment says the
  webOS 5 path is **not device-verified by the author** `[SRC]`.
- **Starfish C++ ABI.** Calls the mangled `StarfishMediaAPIs` symbols directly through `__asm__`
  labels. Over-allocates the object into a 64 KB in-place slot because the object size is not
  known. Interposes `callbackFunctionHook` so late callbacks after unload are dropped (PLX`src/starfish.c`) `[SRC]`.
- **Keys.** `docs/remote-keys.md` documents the LG system SDL fork's `SDL_KeyboardEvent` as
  shifted by 4 bytes against stock SDL headers: an `inputSource` field is inserted. Remote keys are
  matched on scancode (`wcode`) for codes ≥300 (482 Back, 505 Exit, 450 Play…), measured on the
  author's 2019 set `[SRC]` (doc), `[UPSTREAM-HW]`.
- **Paths.** `rust-modules/base/src/paths.rs` resolves the app dir from `/proc/self/exe`, not
  `$HOME`. It persists the session at `/media/developer/<id>-auth.json`, then
  `/media/internal/.<id>-auth.json`, then the app dir, then `/tmp` as a non-durable last resort `[SRC]`.
- **Testing.** Host simulator, an on-device harness and firmware compatibility tooling
  (`tools/fwcompat.py` against the webosbrew fw-symbols DB) `[SRC]`.

### moonlight-tv (`../../../moonlight-tv`)

- **Entry point.** `src/main.c`: `setenv("EGL_PLATFORM","wayland")`,
  `setenv("XDG_RUNTIME_DIR","/tmp/xdg")`, `RLIMIT_CORE=0` in release builds, then
  `app_init` → `app_ui_open` → `while (app.running) app_run_loop()` → `app_deinit` (MLT`src/main.c`) `[SRC]`.
- **Init order** (`src/app/app.c` `app_init`): `SDL_Init(0)`, settings, backend, then the webOS
  hints (KEYS_BACK, KEYS_EXIT, CURSOR_SLEEP_TIME, optional KEYS_HOME/META, RIBBON=false), then
  `SDL_InitSubSystem(SDL_INIT_VIDEO)`. A comment there says *"DO not init video subsystem before
  NDL/LGNC initialization"* `[SRC]`.
- **Loop.** `app_run_loop` = `app_process_events` + `lv_task_handler()` + `SDL_Delay(1)` `[SRC]`.
- **Cross-thread dispatch.** `src/app/platform/sdl/bus.c` posts `SDL_USEREVENT` carrying a function
  pointer. `app_bus_post_sync` blocks the caller on an `SDL_cond` until the main thread runs it `[SRC]`.
- **Lifecycle.** `app_event_filter` handles `SDL_APP_WILLENTERBACKGROUND` (interrupts streaming),
  `SDL_APP_DIDENTERFOREGROUND` (invalidates the screen) and window focus/hidden/close `[SRC]`.
- **SDL.** CMake sets `SDL2_BACKPORT_RELEASE "release-2.30.12-webos.6"`, installs it to
  `lib/backports`, and sets `CMAKE_INSTALL_RPATH "$ORIGIN/lib/backports:..."` (MLT`CMakeLists.txt` ~L103, L145–150) `[SRC]`.
- **TLS.** Builds mbedTLS from source if it is not found `[SRC]`. curl: ships a placeholder
  `libcurl` named by SONAME ("Fake library for cURL ABI issue", MLT`cmake/PackageWebOS.cmake`;
  implementation in commons-c `platform/webos/curl-abi-fix`). **What ABI issue it fixes is not
  stated** `[UNK]`.
- **Packaging.** CPack External generator → `ares-package` → optional `webosbrew-gen-manifest`.
  Targets for `webosbrew-ipk-verify`, `ares-install` and `ares-launch` (MLT`cmake/PackageWebOS.cmake`, `cmake/AresPackage.cmake`) `[SRC]`.
- **Paths.** `path_webos.c` uses `$HOME/conf` and `$HOME/cache`, with `/tmp/moonlight-tv/...` as the
  non-persistent fallback `[SRC]`. This contradicts PlxNative's advice not to trust `$HOME`; see
  NETWORKING-AND-STORAGE.md.

### punktfunk client-webos (`../../../client-webos`)

- Rust, target `armv7-unknown-linux-gnueabi` with `+neon,+vfp3,-soft-float` codegen. Its notes call
  soft-float codegen the single biggest performance fix, ~300 ms → ~30 ms render (PF`docs/NOTES.md`) `[SRC]` (claim).
- **libstdc++ linked statically, never bundled.** webOS 10/11 system libraries need newer
  `GLIBCXX` than the SDK copy, and a bundled `libstdc++.so.6` would take precedence over theirs
  (PF`docs/NOTES.md` "Toolchain") `[SRC]`, `[UPSTREAM-HW]`.
- **glibc shims** for `getauxval`, `gettid`, `sendmmsg`, because the sysroot glibc is 2.12
  (PF`src/platform/webos/glibc_compat_shim.c`) `[SRC]`. Our sysroot is also glibc 2.12.2 `[BUILD]`.
- **NDL.** `libNDL_directmedia.so.1` must be dlopened. A `DT_NEEDED` entry breaks startup on
  webOS 4. The audio-plane, flush and HDR pitfalls are documented in detail (PF`docs/NOTES.md`
  "NDL's audio plane", "Video decode") `[SRC]`, `[UPSTREAM-HW]` (LG CX webOS 5.6, LG G5 webOS 10.3).
- **Device info.** Reads `/var/run/nyx/os_info.json`, `/var/run/nyx/device_info.json` and
  `/etc/prefs/properties/machineName`, all treated as optional (PF`src/platform/webos/device.rs`) `[SRC]`.
- **Black-screen pitfall.** NDL punch-through composites only for the SAM foreground app, so
  the app must be launched through the real lifecycle (`luna-send …/launch`), not over ssh `[SRC]` (claim).

### lgnome

- C + LVGL + system SDL2. NDL backend in a separate MIT submodule (`backend_ndl`, dlopen)
  (LGN`third_party/PROVENANCE.md`) `[SRC]`.
- **Termination hooks** (LGN`native/src/native_lifecycle.c`). Orderly teardown runs from
  `SDL_QUIT`/`SDL_APP_TERMINATING`, which SDL's own SIGINT/SIGTERM handlers raise. Fatal signals
  (SEGV, BUS, ILL, FPE, ABRT, HUP, QUIT) run an emergency NDL release on an alternate stack, with
  `alarm(3)` as a guard, then re-raise. `SIGPIPE` is ignored. Reason given: *"webOS keeps the
  DirectMedia pipeline acquired until NDL_DirectMediaQuit… the next launch can fail to init"* `[SRC]`.
- Sets `EGL_PLATFORM`/`XDG_RUNTIME_DIR` like moonlight-tv. Deliberately does **not** request
  KEYS_EXIT, so webOS closes the app itself `[SRC]`.
- Reads USB keyboard/mouse from evdev (`/dev/input`, `EVIOCGRAB`) below the compositor `[SRC]` (README).
- `appinfo.json` uses `"disableBackHistoryAPI"`, `"useAllKeyboardKeys"`, `"cloudgame_active"`. Their
  effect on native apps is `[UNK]`.

### webosbrew/samples

- Minimal C samples for SMP (ACB / exported window), NDL (esplayer, DirectMedia v1/v2) and LGNC.
  `media/common` builds on the host for testable elementary-stream parsing (SMP`README.md`) `[SRC]`.
- Their status table claims hardware verification, including **NDL DirectMedia v2 on a 65UP7560
  (webOS 6.5.2)** and SMP+exported window on the same set `[UPSTREAM-HW]`.
- `sdl_shell.c`: `SDL_WINDOW_FULLSCREEN` (not `FULLSCREEN_DESKTOP`), weak references to
  `SDL_webOSCursorVisibility`/`SDL_webOSGetPanelResolution` so the binary loads on firmware
  without them, and env setup before `SDL_INIT_VIDEO` (SMP`media/common/sdl_shell.c`) `[SRC]`.
- `cmake/WebOSPackage.cmake` `webos_add_ipk()`: staging with `cmake -E`, `ares-package --force-arch`,
  `ares-install`, `webosbrew-ipk-verify -S -d -r "<range>"` `[SRC]`.

### webosose/samples

- `native-apps/downloadable/com.sample.waylandegl`: raw `wl_compositor` + `wl_webos_shell` + EGL `[SRC]`.
- `native-apps/built-in/com.example.app.nativeqt`: calls
  `luna://com.webos.service.applicationmanager/registerApp` with
  `"nativeLifeCycleInterfaceVersion": 2` and handles a `relaunch` event `[SRC]`.
- These are **webOS OSE** samples. The retail service name differs: webosbrew SDL uses
  `luna://com.webos.applicationManager/registerApp` (SDLW`src/core/webos/SDL_webos_init.c`) `[SRC]`.
  OSE samples are therefore background only, not a source of retail API facts.

### mariotaku/ss4s (media abstraction used by moonlight-tv)

- Backend modules are separate shared objects behind a C driver interface (`SS4S_PlayerDriver`,
  `SS4S_AudioDriver`, `SS4S_VideoDriver` in SS4S`modules/interface/include/ss4s/modapi.h`) `[SRC]`.
  A useful pattern for optional dependencies, but LGPL-3.0.
- NDL v2 PTS = milliseconds of `CLOCK_MONOTONIC` since `NDL_DirectMediaLoad`
  (SS4S`modules/webos/ndl/webos5/ndl_player.c` `SS4S_NDL_webOS5_GetPts`) `[SRC]`.

## Cross-project agreements (high confidence)

1. Set `EGL_PLATFORM=wayland` and `XDG_RUNTIME_DIR=/tmp/xdg` (if unset) before SDL video init
   (moonlight-tv, lgnome, samples) `[SRC]`.
2. Request `SDL_WEBOS_ACCESS_POLICY_KEYS_BACK` before the window exists, or Back is never
   delivered (all four apps and the samples) `[SRC]`.
3. Never put firmware-optional libraries in `DT_NEEDED`: dlopen NDL, ACB and optional SDL webOS
   symbols (plx, punktfunk, lgnome, samples) `[SRC]`.
4. A symbol-table check proves only that a binary will load, not that it works (samples README,
   plx `fwcompat.py` docstring) `[SRC]`.
5. Link libstdc++ statically, or avoid C++ in the binary, because firmware libstdc++ varies
   (6.0.16 … 6.0.32) `[FWDB]`, `[SRC]` (punktfunk).

## Disagreements (need device evidence)

| Topic | Position A | Position B |
|---|---|---|
| SDL source | System SDL (plx, lgnome, samples) | Bundled webosbrew fork (moonlight-tv, punktfunk) |
| NDL v2 PTS unit | ms since load (ss4s, punktfunk) | µs (samples `directmedia_v2.c` divides ns by 1000) |
| Persistent path | `$HOME/conf` (moonlight-tv) | `/proc/self/exe`-derived + `/media/developer` / `/media/internal` (plx) |
| Exit key | Requested (moonlight, punktfunk, samples) | Left to the system (lgnome) |
