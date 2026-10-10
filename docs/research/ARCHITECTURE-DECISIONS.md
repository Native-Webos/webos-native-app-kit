# Architecture decisions

Each ADR states the decision, the evidence behind it (tags per [README.md](README.md)) and what would
change it. Status: **Proposed** until the owner accepts or the milestone that implements it verifies it.

---

## ADR-001 Language: C++17, statically linked runtime

- **Decision:** the kit is C++17 (C is fine for small platform shims). webOS builds link
  `-static-libstdc++ -static-libgcc`. No C++ types cross a dynamic-library boundary we do not own.
- **Evidence:** NDK GCC 12.2 compiles C++17 for the target; the static runtime leaves only
  `GLIBC_2.4` requirements `[BUILD]`. Firmware libstdc++ ranges 6.0.16–6.0.32 and is 6.0.25 on
  webOS 6 `[FWDB]`. punktfunk links libstdc++ statically for the same reason `[SRC]`.
- **Why not C:** explicit ownership (RAII, `unique_ptr`/`shared_ptr` lifetime tokens) and
  `std::function` are central to the mission's cancellation and callback-safety rules.
- **Revisit if:** binary size becomes a problem on low-memory sets.

## ADR-002 Build system: CMake ≥ 3.22 with presets

- **Decision:** CMake presets `host-debug`, `host-asan`, `webos-debug`, `webos-release`. The webOS
  presets use the NDK `toolchainfile.cmake`. Host: GCC 13 with Make (Ninja optional).
- **Evidence:** NDK ships `toolchainfile.cmake` and a `cmake-3.22` module dir `[BUILD]`; moonlight-tv,
  lgnome and samples all use CMake with that toolchain file `[SRC]`. Host has CMake 3.28 `[BUILD]`.

## ADR-003 Window/input/GL context: bundled webosbrew SDL-webOS 2.30.x

- **Decision:** on webOS, build SDL-webOS from the pinned tag `release-2.30.12-webos.6` with the NDK,
  ship it as `lib/libSDL2-2.0.so.0` with RPATH `$ORIGIN/lib`, and compile against **its** headers.
  On the host, use the system SDL2 2.30.0. Keep a later option to target the system SDL for size, behind
  the same platform adapter.
- **Evidence:**
  - System SDL differs per firmware: 2.0.10 on webOS 5–6, 2.0.14 on 7–11; `SDL_RenderGeometry`
    exists in none `[FWDB]`. NDK headers are 2.24.1 with the broken-ABI `inputSource` field `[BUILD]`.
    Linking system SDL with NDK headers can import functions the TV lacks.
  - moonlight-tv, a mature open-source native app, bundles exactly this SDL `[SRC]`.
  - SDL-webOS exposes the HOME/GUIDE/META/RIBBON access-policy hints and SAM
    registration/lifecycle → SDL events `[SRC]`.
  - SDL-webOS has `SDL_WEBOS_BROKEN_ABI` as a build option; we build **without** it so the events use
    the standard layout, and host and TV share one struct layout `[SRC]`.
- **Risks:** building SDL-webOS with our NDK is not yet done `[UNK]`; its runtime needs firmware
  `libwayland-webos-client`, `libhelpers`, EGL/GLES `[FWDB]` present on 6.4.0. Real behaviour on a
  webOS 6 TV is unverified (moonlight-tv's use is indirect evidence).
- **Revisit if:** the build fails with this NDK, or device tests show regressions versus system SDL.
- **Owner decision:** see blockers list. This is the recommended default.

## ADR-004 Rendering: own GLES2 renderer on an SDL GL context

- **Decision:** a small batched GLES2 renderer (rects, rounded rects, textured quads, glyph runs,
  clip stack via scissor, transform stack) owned by the render thread (main thread). The same GLES2
  backend runs on host (Mesa EGL/GLES2) and TV; only context and display parameters come from platform
  adapters. No `SDL_Renderer`.
- **Evidence:** GLES2/EGL on every firmware `[FWDB]`; host has `libgles-dev`, `libegl-dev` `[BUILD]`;
  PlxNative (own GLES2) and punktfunk (GL + Skia) draw without `SDL_Renderer` `[SRC]`; transparent
  clear for the video plane is required `[SRC]`.
- **Revisit if:** a GPU without the needed GLES2 features appears in device capability logs.

## ADR-005 Text: stb_truetype atlas + bundled OFL font

- **Decision:** rasterise glyphs from a bundled font into a dynamic atlas texture. FreeType is a later
  option if quality requires it.
- **Evidence:** bundling fonts is universal practice `[SRC]`; avoids firmware font and FreeType
  variance; identical output on host and TV `[INF]`.

## ADR-006 Images: stb_image on workers; GPU upload only by the renderer

- **Decision:** decode PNG/JPEG off the main thread to CPU RGBA, bounded size; the renderer uploads
  textures on the main thread and owns their lifetime.
- **Evidence:** libjpeg SONAME changes across firmware `[FWDB]`; punktfunk decodes on the fetch thread `[SRC]`.

## ADR-007 Input: platform key adapter → semantic actions → focus manager

- **Decision:** `SdlInputAdapter` maps SDL events to `KeyEvent` and then a semantic `Action` using a
  per-platform table with provenance. The focus manager consumes only `Action`s. Key-capture
  diagnostics are part of Example A.
- **Evidence:** moonlight `navkey`, plx `classify` `[SRC]`; scancodes differ between SDL builds `[SRC]`.
- **Default hints:** KEYS_BACK=true and KEYS_EXIT=true. HOME not requested.

## ADR-008 Concurrency: main/render thread + worker pool + main-thread dispatcher

- **Decision:** one main thread owns SDL, GL, UI and the event loop. A fixed worker pool runs blocking
  I/O and decoding. Workers deliver results by posting closures to a lock-protected main-thread queue,
  drained once per frame. Every async operation takes a `CancellationToken`. Callbacks capture a
  `std::weak_ptr` lifetime token of their owner and are dropped if the owner is gone.
- **Evidence:** moonlight's SDL user-event bus `[SRC]` (we avoid blocking `post_sync` during
  shutdown); punktfunk serialises NDL behind one mutex `[SRC]`.

## ADR-009 Networking: `HttpClient` interface; first implementation libcurl via dlopen

- **Decision:** interface first, then a libcurl backend that `dlopen`s `libcurl.so.4` at runtime (TV
  and host) and resolves only long-standing `curl_easy_*`/`curl_multi_*` symbols. Missing curl →
  capability `Unsupported`, not a crash. TLS verification is always on, with an optional
  app-supplied CA bundle. A curl multi loop runs on its own worker thread.
- **Evidence:** `libcurl.so.4` on webOS 5.3.1–11.2, TLS backend changes underneath (1.1 → 3) `[FWDB]`;
  plx needed extra CA roots on older sets `[SRC]`; host has `libcurl.so.4` runtime but no headers `[BUILD]`.
- **Implemented** in `net/` (2026-10-11). On an OLED65C1PTB, webOS 6.5.3: system libcurl 7.61.0 with
  OpenSSL 1.1.1t; HTTPS to DigiCert- and Let's-Encrypt-signed hosts works from the app; an expired
  certificate is refused `[HW]`. Only curl API ≤ 7.28 is used (`CURLOPT_PROTOCOLS`, `curl_multi_wait`).
- **Open:** moonlight's undocumented "curl ABI fix" `[UNK]` (not needed here); other firmware.
  Fallback if system curl misbehaves: bundle a static curl + mbedTLS (moonlight bundles mbedTLS) `[INF]`.

## ADR-010 Storage: DB8 for durable app state on webOS, files for cache and host

- **Finding (OLED65C1PTB, webOS 6.5.3, Developer Mode, 2026-10-11):** the app's jail has no durable
  writable filesystem location (only tmpfs). DB8 called from the app process works: `putKind` with
  `owner` = app id, `get`, `put` with `_rev`; data survived relaunch, reinstall and a full reboot. It needs
  `"requiredPermissions": ["database.operation"]` and short `_id`s (≤ 14 characters worked, 33 failed).
  `[HW]` on one TV.
- **Decision:** the webOS `KeyValueStore` backend is DB8, called from the app (no separate service unless
  a firmware proves it necessary; PlxNative needed one on webOS 4). Files remain for disposable cache in
  `/tmp/<id>` and for the host backend. Atomic multi-key updates use `_rev` for optimistic concurrency.
- **Implemented** as `storage/` (`open_default_store`): kind `<app_id>.kv:1` with an index on `key`, so DB8
  assigns `_id`s and keys can be up to 64 characters. Verified on the same TV (records.json).
- **Open:** other firmware versions, Homebrew Channel (non-Dev-Mode) installs, behaviour on uninstall,
  record size limits, secrets (keymanager) — see NETWORKING-AND-STORAGE.md.

## ADR-011 Media: controller/backend split; NDL v2 first; demuxer decision deferred to M7

- **Decision:** `MediaSource` (provider-independent) → `PlaybackController` (state machine, host-tested)
  → `MediaBackend` interface. First device backend: NDL DirectMedia v2, dlopened. SMP + exported
  window is a second candidate for seek-heavy VOD. Container demuxing (likely FFmpeg libavformat as a
  shared library, LGPL) is decided in M7.
- **Evidence:** see NATIVE-MEDIA.md. NDL v2 hardware-confirmed upstream on webOS 6.5.2 `[UPSTREAM-HW]`.

## ADR-012 Optional features as separate CMake targets

- **Decision:** `kit_core` (logging, async, config, storage), `kit_platform_*`, `kit_render`, `kit_input`,
  `kit_ui`, `kit_net`, `kit_image`, `kit_media` (+ backends), `kit_auth`. Apps link only what they use.
  Device-optional libraries are dlopened, so no feature adds `DT_NEEDED` entries the TV may lack.
- **Evidence:** ss4s module separation `[SRC]`; DT_NEEDED startup failures `[SRC]`.

## ADR-013 Kit licence: GPL-3.0-or-later (accepted 2026-10-11, revised the same day)

- **Decision:** GPL-3.0-or-later, the same as PlxNative. PlxNative code may be reused directly with attribution and its file headers kept. Apps built on the kit must be GPL-compatible. An earlier MIT/Apache choice on the same day was withdrawn by the owner.

## ADR-016 Device tooling: webosbrew ares-cli-rs (accepted 2026-10-11)

- **Decision:** `ares-setup-device`, `ares-install`, `ares-launch`, `ares-package` (cross-check only), `ares-pull`/`ares-shell` from ares-cli-rs (amd64 .deb/tarball). The TV is Developer Mode, not rooted, so there are no root-only steps.

## ADR-014 Minimum supported webOS: 5 (5.3.1) runtime target, webOS 6 primary test target

- **Decision:** target releases with NDL v2, `libcurl.so.4` and SDL exported windows (5.3.1+)
  `[FWDB]`. webOS 6 is the primary hardware target per the mission. Older releases are out of scope
  unless the owner asks.
- **Evidence:** the three facts above all begin at 5.3.1 in the inventory.

## ADR-015 Verification gate: imports checked against firmware inventory

- **Decision:** `tools/verify.sh` checks every undefined dynamic symbol of the binary and bundled
  libs against each targeted release in the inventory, using `webosbrew-ipk-verify` if installed,
  otherwise our own checker built on `tools/fw-lib-matrix.py`'s reader. The gate is required before
  any device install.
