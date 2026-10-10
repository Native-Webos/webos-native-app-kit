# Toolchain and build

Evidence tags: see [README.md](README.md).

## Host environment audit (2026-10-09) `[BUILD]` (commands run here)

| Tool | Found |
|---|---|
| OS | Linux x86_64, 12 cores, ~46 GB free on `/` (90 % used) |
| gcc / g++ | 13.3.0 |
| clang | **missing** |
| cmake | 3.28.3 |
| ninja | **missing** (GNU Make 4.3 available) |
| pkg-config | 1.8.1 |
| python3 | 3.12.3 |
| rustc / cargo | 1.98.1 |
| node / npm | 20.20.2 / 10.8.2 |
| docker | 29.8.0 |
| host SDL2 | 2.30.0 (`sdl2-config`) |
| ffmpeg | 6.1.1 |
| `ares-package` / `ares-install` / `ares-launch` / `ares-setup-device` | **missing** |
| `webosbrew-ipk-verify` | **missing** |
| webOS NDK | `~/webos-ndk/arm-webos-linux-gnueabi_sdk-buildroot` (openlgtv buildroot-nc4) |

## The NDK `[BUILD]`

- Compiler: `arm-webos-linux-gnueabi-gcc` **12.2.0** (Buildroot `-gd7ed7ee3-dirty`). It runs on
  this x86_64 host. PlxNative's `docs/building.md` says the NDK has no x86_64 Linux build. That is
  wrong for this installation, and webosbrew/native-toolchain's README lists an x86_64 Linux
  download `[SRC]`.
- CMake toolchain file: `share/buildroot/toolchainfile.cmake` (sets `CMAKE_SYSTEM_PROCESSOR armv7l`
  and the Buildroot platform module).
- Sysroot: glibc **2.12.2**, libstdc++ **6.0.30** (`.so` and `.a`), SDL2 **2.24.1** headers and
  libraries plus SDL2_image/ttf/mixer/net, EGL, GLESv1/v2 (GLES3 headers present), Wayland and
  `wayland-webos-*` protocol headers, `libNDL_directmedia`, `libplayerAPIs`,
  `starfish-media-pipeline/StarfishMediaAPIs.h`, `libAcbAPI`, `libluna-service2`,
  `libhelpers` (`webos-helpers/libhelpers.h`), `libpbnjson`, `libPmLogLib`, curl 4.8.0, OpenSSL 1.1,
  libpng16, libjpeg, turbojpeg, freetype, fontconfig, sqlite3, pulse, alsa, GStreamer 1.0, Qt5.
- `libglibc_polyfills.a` is in the sysroot. Its contents have not been examined `[UNK]`.

### Several sysroot libraries are link-time stubs `[SRC]`

- punktfunk: *"`libNDL_directmedia.so.1` is the real device library; the NDK sysroot ships a
  link-time stub."*
- webosbrew/samples: *"the SDK's `libplayerAPIs.so` is a link-time stub that exports every symbol
  at a single address."*
- PlxNative `starfish.c`: *"the NDK's sysroot copy carries stub bodies"* for the SDL exported-window
  functions.

**Consequence:** a successful link against the sysroot proves nothing about the TV. Import
verification against firmware inventories is needed (see below).

## Compile smoke test `[BUILD]`

A C++17 program using `SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_BACK)`, `SDL_Init`,
`glClear` and `std::vector<std::string>` was compiled with:

```sh
arm-webos-linux-gnueabi-g++ -std=c++17 -O2 a.cpp -I$SYSROOT/usr/include/SDL2 \
    -lSDL2 -lGLESv2 -static-libstdc++ -static-libgcc
```

Result: `ELF 32-bit LSB executable, ARM, EABI5`, interpreter `/lib/ld-linux.so.3`;
`NEEDED` = `libSDL2-2.0.so.0 libGLESv2.so.2 libm.so.6 libc.so.6 ld-linux.so.3`; the only
version requirement is **`GLIBC_2.4`**; `Tag_CPU_arch: v7`, `Tag_FP_arch: VFPv3`, softfp calling
convention (no `Tag_ABI_VFP_args`). The binary was **not run** anywhere.

## ABI rules derived from the evidence

1. **Float ABI is softfp** (`arm-webos-linux-gnueabi`, not `gnueabihf`) `[BUILD]`. punktfunk found
   that Rust's default codegen for this target used soft-float maths. Its notes say forcing
   `+vfp3,+neon,-soft-float` codegen was the largest single performance fix `[SRC]`. For C/C++ the
   NDK GCC already emits VFPv3 `[BUILD]`. Whether `-mfpu=neon` / `-mcpu=cortex-a53`-class tuning is
   safe on every target SoC is `[UNK]`. Leave the NDK defaults until a device is measured.
2. **libstdc++ statically linked** `[BUILD]`. The firmware has 6.0.25 on webOS 6 `[FWDB]`. A bundled
   `libstdc++.so.6` with an RPATH could also override the system copy for LG's own C++ libraries
   loaded into the process (punktfunk NOTES) `[SRC]`.
3. **No `DT_NEEDED` on optional firmware libraries.** Use dlopen with a capability result for
   NDL, ACB, playerAPIs and webOS-only SDL entry points `[SRC]` (plx, punktfunk, lgnome, samples).
4. **The SDL headers must match the SDL loaded at runtime.** LG's system SDL inserts `Uint32
   inputSource` into `SDL_KeyboardEvent` and other events. webosbrew SDL-webOS reproduces that
   layout only when built with `SDL_WEBOS_BROKEN_ABI` (SDLW`include/SDL_events.h` L229) `[SRC]`. The
   NDK sysroot headers contain the `inputSource` field `[SRC]`, matching the system library. Mixing a
   standard-ABI header with a broken-ABI library shifts every keyboard field `[SRC]` (plx `remote-keys.md`).
5. **Never use an SDL function the oldest target firmware lacks**, unless SDL is bundled. The system
   SDL on webOS 6 is 2.0.10 `[FWDB]`.

## SDL choices

| Option | Evidence | Pros | Cons |
|---|---|---|---|
| A. System SDL (LG fork) | plx, lgnome, samples | Nothing to ship; LG-maintained | Version differs per release (2.0.10 on webOS 6, 2.0.14 on 7+); NDK headers newer than every TV; broken-ABI events; HOME/RIBBON hints `[UNK]` |
| B. Bundled webosbrew SDL-webOS 2.30.x | moonlight-tv `release-2.30.12-webos.6`, `$ORIGIN/lib/backports` RPATH | One SDL API on every firmware; open source; access-policy hints for BACK/EXIT/HOME/GUIDE/META/RIBBON; SAM `registerApp` built in (SDLW`src/core/webos/SDL_webos_init.c`) | Extra package size; we own its correctness; still depends on firmware `libwayland-webos-client`, `libhelpers`, EGL |
| C. Bundled SDL3 webOS fork | punktfunk (`release-3.4.16-webos.5`) | Newest API | The webOS scancode block moves to 352–375 (punktfunk NOTES); fewer C apps to learn from |

The recommendation is in ARCHITECTURE-DECISIONS.md (ADR-003).

## Packaging `[SRC]`

- An `.ipk` is a Debian-style `ar` archive. Producers seen: `ares-package <stage> -o <dist>
  --force-arch arm` (moonlight-tv `cmake/AresPackage.cmake`, samples `WebOSPackage.cmake`).
- `appinfo.json` minimum fields seen in every native app: `id`, `version` (three integers per plx
  `distribution.md`), `vendor`, `type: "native"`, `main` (path relative to the package root),
  `title`, `icon`. Common optional fields: `largeIcon`, `iconColor`, `splashBackground`,
  `requiredMemory`, `supportTouchMode`, `useAllMouseButtons`.
- Icon sizes per plx `distribution.md`: `icon` 80×80, `largeIcon` 130×130 `[SRC]` (doc claim).
- The launcher caches tiles by app id **and version**. Bump the version, or uninstall, to see icon
  changes (samples README) `[SRC]` (claim).
- Bundled libraries go in `lib/` beside the binary, staged as real files under their SONAME because
  ipks cannot carry symlinks (samples `WebOSPackage.cmake` comment) `[SRC]`. Executable RPATH:
  `$ORIGIN/lib` or `$ORIGIN/../lib` `[SRC]`.

### Tools to install (not present yet)

- `@webos-tools/cli` (npm) provides `ares-package`, `ares-install`, `ares-launch`,
  `ares-setup-device` and `ares-log`. lgnome pins `@webos-tools/cli@3.2.4` `[SRC]`.
- `webosbrew-ipk-verify` (Apache-2.0, Rust, from dev-toolbox-cli). Release builds are arm64
  `.deb` only per plx `fwcompat.py` `[SRC]`. Building it from source with the local cargo is
  `[INF]` feasible. Our own `tools/fw-lib-matrix.py` already reads the same inventory.

## Symbol verification plan

1. After every webOS build, run `readelf --dyn-syms`/`-d` on the binary and each bundled `.so`.
2. For each undefined symbol, look up the library that provides it in the firmware inventory for
   each target release. Fail the build if a required (non-weak) symbol is missing on a targeted
   release.
3. Bundled libraries satisfy their own symbols, and their own imports are checked the same way.
4. A clean result means "will load", never "works" (samples README; plx `fwcompat.py`).

## Build matrix (proposed)

| Preset | Toolchain | Purpose |
|---|---|---|
| `host-debug` | system gcc 13, system SDL2 2.30, Mesa GLES2 | Development, simulator, unit tests |
| `host-asan` | same + ASan/UBSan | Memory/UB checks in CI |
| `webos-release` | NDK toolchainfile, `-static-libstdc++`, bundled SDL | Device package |
| `webos-debug` | same with `-O1 -g` | Device debugging |
