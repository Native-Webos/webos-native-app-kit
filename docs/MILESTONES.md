# Milestone plan

Status legend: ✅ done · 🟡 in progress · ⬜ not started. "Done" means built and tested at the
level stated. Device items stay open until run on a TV.

| # | Milestone | Status |
|---|---|---|
| 0 | Repository and environment audit | ✅ (see research/TOOLCHAIN-AND-BUILD.md) |
| 1 | Research and architecture | ✅ documents written; ADRs await owner acceptance |
| 2 | Native runtime | 🟡 built, packaged and verified offline; device run pending |
| 3 | Rendering and input | 🟡 implemented, host-tested, and run on an OLED65C1PTB (webOS 6.5.3) |
| 4 | UI and navigation | 🟡 implemented and host-tested; device run pending |
| 5 | Platform services | 🟡 storage, background work, HTTP and images done (TV-verified); secrets, WebSocket pending |
| 6 | Lifecycle and diagnostics | ⬜ |
| 7 | Media foundation | ⬜ |
| 8 | Reference applications | ⬜ |
| 9 | Optional Jellyfin client | ⬜ |
| 10 | Compatibility and release | ⬜ |

## M2 — Native runtime (next)

Packaging and firmware tooling follow the plan in `research/PACKAGING-AND-FIRMWARE.md` Part D.

Goal: a minimal app that builds on the host and for webOS, packages into an `.ipk`, passes import
verification, and (if a TV is available) installs and launches.

1. `CMakeLists.txt`, `CMakePresets.json` (host-debug, host-asan, webos-debug, webos-release).
2. `third_party/sdl-webos`: fetch the pinned tag `release-2.30.12-webos.6` and build it with the NDK
   (standard ABI); stage `libSDL2-2.0.so.0`. **Risk:** first build with this NDK.
3. `platform/` minimal: env setup (`EGL_PLATFORM`, `XDG_RUNTIME_DIR`), SIGPIPE ignore, SIGTERM →
   atomic flag, launch-params JSON from `argv[1]`, log file in the runtime dir.
4. `logging/` minimal structured logger (level, category, monotonic time, thread id; secret redaction hook).
5. `applications/examples/hello`: window, clear to a colour, quit on Back/Exit/close.
6. `tools/`: `doctor.sh` (checks NDK, cmake, ares-cli, inventory), `build-host.sh`, `build-webos.sh`,
   `package.sh` (stage + `ares-package`; fails clearly if ares-cli is missing), `verify.sh` (imports
   vs firmware inventory), `install.sh`/`launch.sh`/`logs.sh` (ares-cli, honour `ARES_DEVICE`).
7. Tests: logger and launch-param parsing unit tests (host).

Exit criteria: host build + tests pass; webOS build produces an ARM binary + bundled SDL whose imports
verify against 5.3.1–11.2 in the inventory; `.ipk` produced if ares-cli is installed. Device launch
only if hardware is available, recorded with model and firmware.

### M2 status (2026-10-11)

Verified here:

| Item | Evidence |
|---|---|
| Host build (GCC 13, system SDL 2.30, Mesa GLES2) with no compiler warnings | `[BUILD]` `tools/build-host.sh` |
| C++ unit tests: logging (11) and platform (8); Python packaging tests (12) | `[TEST]` `ctest --preset host-debug`, also clean under ASan/UBSan (`host-asan`) |
| `wnk-hello` on the host (offscreen driver): window, GLES context, probe, SIGTERM → SDL_QUIT → exit 0, log file 0640 | `[TEST]` manual run 2026-10-11 |
| SDL-webOS `release-2.30.12-webos.6` builds with our NDK, standard ABI; needs only libc/libm/libdl/libpthread/librt ≤ GLIBC_2.9 | `[BUILD]` |
| `wnk-hello` ARM build: `DT_NEEDED` = reviewed list, only `GLIBC_2.4`, RPATH `$ORIGIN/lib`, libstdc++ static | `[BUILD]` |
| `org.wnk.examples.hello_0.1.0_arm.ipk` passes `check-package.py` and is byte-reproducible (same sha256 twice) | `[TEST]` |
| Loadable on all 7 inventoried releases 5.3.1–11.2.0, including the 12 libraries SDL loads with dlopen | `[FWDB]` `tools/fw-check.py`; negative test (system SDL + `SDL_RenderGeometry`) fails as expected |

Not verified (needs the Developer Mode TV):

- install through `ares-install`, launch through `ares-launch`, and whether our own ipk writer's output
  installs (PlxNative reports it does with the same format);
- bundled SDL actually loading through RPATH inside the jail; window, GL strings, panel resolution;
- remote key codes (the app logs every key), Back/Exit delivery, background/foreground events;
- which storage candidates are writable, and whether the `prisoner` user can read `/tmp/<id>/events.log`;
- `ares-cli-rs` installed and configured on this machine (not yet).

Known limitations: `webosbrew-ipk-verify` is not installed, so only one loadability checker runs.
The firmware inventory (v20260731, 14 images) has one image per release line.

## M3 — Rendering and input

GLES2 renderer (rect, rounded rect, image, text atlas, clip, transform), logical 1920×1080 canvas,
GL capability logging, input adapter + action mapping, key-capture diagnostic screen, focus manager
core (directional, explicit neighbours, scopes). Host golden-image tests on Mesa; mapping/focus unit tests.

### M3 status (2026-10-11)

Implemented: `core/` geometry; `input/` (keymaps with provenance, `ActionMapper` with press/repeat/release,
unpaired-key and unknown-key suppression, `release_all` on focus loss, SDL adapter with static checks of
every hard-coded scancode); `focus/` (Android-style directional search with in-beam preference, explicit
neighbours and `kBlocked`, containing scopes, per-scope memory, modal stack with restore, recovery when the
focused node disappears); `render/` (stb_truetype fonts, UTF-8, shelf-packed glyph atlas with reset,
`Gles2Renderer`: batched SDF rounded rects and borders, RGBA images, glyphs, nested scissor clips,
translation, letterboxed 1920x1080 canvas, destination alpha preserved for video punch-through, GPU info).
Example A is now a tile grid + modal dialog + on-screen key log.

| Item | Evidence |
|---|---|
| 10 input, 15 focus, 8 font/canvas unit tests | `[TEST]` |
| 11 renderer pixel tests on a real GLES2 context (Mesa, SDL offscreen) | `[TEST]` |
| 3 end-to-end tests of Example A through real SDL events (focus restore after dialog, double Back exits, unmapped keys inert) | `[TEST]` |
| All of the above clean under ASan/UBSan | `[TEST]` `host-asan` |
| Screenshots of the demo at 1280x720 reviewed (grid, focus ring, dialog, toast, key log) | manual, host |
| ARM build: `DT_NEEDED` reviewed (adds `libm.so.6`), `GLIBC_2.4` only, package `0.2.0` passes `check-package` | `[BUILD]` `[TEST]` |
| Loadable on 5.3.1–11.2.0 incl. SDL's dlopen'd libraries | `[FWDB]` |

On hardware (LG OLED65C1PTB, webOS 6.5.3, Developer Mode, 2026-10-11; details in
`docs/compatibility/records.json`): our ipk installs and launches; bundled SDL 2.30.12 loads (Wayland);
drawable 1920x1080 on a 3840x2160 @120 Hz panel; Mali-G51 with fragment highp; ~60 fps; D-pad/OK/Back/
colour/CH keys arrive with exactly the SDL-webOS source codes (now marked `Measured`); dialog focus restore,
double-Back exit and SAM close all end in exit 0; the Dev Mode user can pull the 0640 log from /tmp.

Found on hardware: **no durable app-writable filesystem location** in the Developer Mode jail (only tmpfs).
**DB8 called directly from the app works instead** and survived relaunch, reinstall and a full reboot
(ADR-010; `platform/luna.hpp`; experiment in Example A). Still unverified: key repeat, transport/number/volume/Home keys, Magic Remote
pointer, background/foreground switching, text quality judged up close.

Simplifications, deliberate for M3: transforms are translation only (no scale/rotation); text is a
single line, left-to-right, no shaping (Latin/Cyrillic/Greek fine; complex scripts need HarfBuzz later);
glyph atlas eviction is "clear all when full"; pixel tests probe specific pixels rather than comparing
whole golden images.

## M4 — UI and navigation

View tree, layout (stack/row/column/grid), Text, Image, Button, List, Grid (virtualised), Dialog/Modal
with focus trapping, Toast, Spinner, ProgressBar, navigation stack with focus restoration, animation
clock. Example B.

### M4 status (2026-10-11)

Implemented: `ui/` — `View` tree (measure/layout only when invalidated; translation and scroll offsets
applied at draw time and in reported focus rectangles), `Row`/`Column` (fixed, auto and weighted fill,
gaps, justify, cross alignment), `Grid`, `ScrollView` (follows focus), `Label` (word wrap, long-word
breaking at code point boundaries, ellipsis), `ImageView` (through an `ImageSource`; `ServiceImageSource`
adapts the image service and texture cache; cancels on destruction, reloads if pixels were evicted),
`Button`, `Card`, `Spinner`, `ProgressBar` (determinate/indeterminate), `CollectionView` (virtualised
list/rail/grid; arrows handled by index so fast repeats never skip or escape; smooth scroll with a peek of
the next line; keeps the focused index across data changes), `Navigator` + `Screen` (covered screens keep
their focus nodes registered but unfocusable, so popping restores the exact node and nested row memory),
`Dialog` (modal through the focus manager's modal stack; Back never reaches the screen underneath),
toasts, an animated focus ring that stays attached to scrolling content, `Smoothed`/`Tween` animation on
a clock advanced by `Ui::update`. Input bubbles from the focused view to its ancestors; unhandled arrows
fall back to geometric focus; unhandled Back is returned to the app. Example B (`applications/examples/tvui`)
uses only these pieces.

| Item | Evidence |
|---|---|
| 27 toolkit unit tests (layout, wrapping, focus sync, bubbling, navigation and restore, dialogs, toasts, 1000-item grid virtualisation, rails row memory, image loading/cancel, scroll view) on a recording renderer | `[TEST]` `unit.ui` |
| 4 end-to-end tests of Example B through real SDL key events (details → Back restores the card, grid scroll to item 40, dialog focus restore, double Back exits) | `[TEST]` `e2e.tvui.*` |
| All tests clean under ASan/UBSan, including handlers that close their own screen | `[TEST]` `host-asan` |
| Screenshots of home rails, browse grid, details and dialog at 1280x720 reviewed | manual, host |
| ARM build: `DT_NEEDED` reviewed (as Example A without `libdl`), `org.wnk.examples.tvui_0.1.0` passes `check-package` | `[BUILD]` `[TEST]` |
| Loadable on 5.3.1–11.2.0 incl. SDL's dlopen'd libraries | `[FWDB]` |

Not verified: anything on a TV (the TV was not reachable from this machine on 2026-10-11) — frame time
with ~200 views and 20+ image textures on the Mali-G51, text legibility at 3 m, key-repeat feel when
scrolling the grid. Simplifications: no pointer (Magic Remote) hit-testing yet; transforms are
translation only, so focus "zoom" is a ring rather than a scale; cells in one collection share one size;
ScrollView does not combine nested scroll offsets.

## M5 — Platform services

WorkerPool, MainDispatcher, CancellationToken, timeouts; `HttpClient` + dlopen curl backend (needs
curl headers on the host: vendor them or install `libcurl4-openssl-dev`); JSON; PathProvider probe;
atomic files; prefs; bounded disk cache; ImageService. Example C.

### M5 status (2026-10-11)

Done: `storage/` `KeyValueStore` with revisions and compare-and-set; `Db8Store` (TV) and `FileStore` (host
and caches); one contract suite runs on both, DB8 against a fake built from the errors measured on the TV.
On an OLED65C1PTB (webOS 6.5.3) the demo's launch counter and remembered focus persist through DB8.
`async/`: cancellation sources/tokens with deadlines, `MainDispatcher` (FIFO, budgets, close), `WorkerPool`
(cancel-on-shutdown with a deadline, discards, failure counting, stats), `Lifetime` guards, `run_then_post`.
16 tests, clean under TSan (20 runs) and ASan, and passing on the TV's CPU (`tools/device-test.sh`).
`net/`: `HttpRequest`/`HttpResponse`, `CurlTransport` (system libcurl via dlopen, multi interface for
≤50 ms cancellation, TLS verification always on, size/time limits, http/https only), `RetryPolicy`,
`HttpClient` delivering on the main thread. 8 unit tests + 9 tests against a local HTTP/HTTPS server.
On the TV (libcurl 7.61.0, OpenSSL 1.1.1t) HTTPS works from the app and an expired certificate is refused.
`image/`: PNG/JPEG decoding with header-based size limits enforced before stb runs, area-averaging
downscale, LRU memory cache, size-bounded disk cache (URL-verified entries, atomic writes, corrupt-entry
recovery), request coalescing, per-ticket cancellation that stops unwanted downloads, prefetch, file://
URLs; `ImageTextures` uploads through the renderer within a byte budget. 15 tests. On the TV: bundled and
LAN-served thumbnails load; a relaunch is served from the disk cache with zero downloads.
Known cost: each image is its own texture, so tiles break draw-call batching (27 calls for 12 tiles);
an image atlas would reduce this if profiling on the TV shows it matters.
Pending: secret storage (keymanager), WebSocket, JSON helpers for apps.

## M6 — Lifecycle and diagnostics

Full lifecycle state machine; deterministic shutdown with join deadlines; crash record; capability
system (`Supported`/`Unsupported`/`Unknown`) + device info; metrics overlay (frame time, textures,
queue depth, cache); machine-readable compatibility DB (`compat/records.json`) with schema.

## M7 — Media foundation

`MediaSource`, `PlaybackController` state machine (host-tested with a scripted backend), NDL v2
backend (dlopen, single mutex, lazy init, emergency release), PTS-unit device experiment, demuxer
decision (FFmpeg libavformat as shared lib vs ES-only sources). Example D. Real playback marked
unverified until seen on a TV.

## M8–M10

Reference apps A–D polished; Jellyfin client (app-only module) on top; compatibility test runs,
licence audit, release workflow.

## Device dependency

Every milestone from M2 has a device column. Without a TV the kit can reach "builds, verifies, host
tests pass" but **not** "works on webOS 6". That gap is recorded honestly in the compatibility DB.
