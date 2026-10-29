# Milestone plan

Status legend: ✅ done · 🟡 in progress · ⬜ not started. "Done" means built and tested at the
level stated. Device items stay open until run on a TV.

| # | Milestone | Status |
|---|---|---|
| 0 | Repository and environment audit | ✅ (see research/TOOLCHAIN-AND-BUILD.md) |
| 1 | Research and architecture | ✅ documents written; ADRs await owner acceptance |
| 2 | Native runtime | 🟡 built, packaged and verified offline; device run pending |
| 3 | Rendering and input | 🟡 implemented, host-tested, and run on an OLED65C1PTB (webOS 6.5.3) |
| 4 | UI and navigation | 🟡 implemented, host-tested and run on an OLED65C1PTB (webOS 6.5.3); Magic Remote pointer included |
| 5 | Platform services | ✅ storage, background work, HTTP, images, secrets and WebSocket (all run on an OLED65C1PTB, webOS 6.5.3) |
| 6 | Lifecycle and diagnostics | 🟡 shell, lifecycle, crash record, capabilities, metrics overlay done and run on an OLED65C1PTB (webOS 6.5.3) |
| 7 | Media foundation | 🟡 controller, FFmpeg source, NDL v2 backend, Example D; plays H.264/HEVC with sound on an OLED65C1PTB (webOS 6.5.3) |
| 8 | Reference applications | 🟡 A, B and D exist; Example C (networked app) host-tested, launched on an OLED65C1PTB (webOS 6.5.3) |
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
(ADR-010; `platform/luna.hpp`; experiment in Example A). Still unverified: transport/number/volume/Home keys, background/foreground switching, text quality
judged up close. (Key repeat and the Magic Remote pointer were measured later with Example B.)

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

Magic Remote pointer: `input::PointerEvent` (SDL-free) with `pointer_event_from_sdl` converting window
units to drawable pixels; `Ui::handle_pointer` hit-tests the topmost focusable view (clipped cells and
everything behind a modal dialog are excluded), hover focuses, a click is OK on the clicked view, the
wheel goes to the vertical scroller under the pointer (horizontal rails pass it on), an arrow key hands
control back to the D-pad. Hover focus never auto-scrolls a list (it would slide the item away from a
stationary pointer). 7 pointer unit tests, 2 e2e tests (click a card, wheel the grid).

Animation: `Smoothed` is a critically damped spring (gentle start, no overshoot, velocity kept when
retargeted); the focus ring keeps its velocity across rapid moves; scroll offsets are drawn on whole
device pixels so text does not shimmer against images; `Ui::draw` clears instead of drawing a
full-screen background quad.

On hardware (LG OLED65C1PTB, webOS 6.5.3, 2026-10-11; `docs/compatibility/records.json`): Example B runs
at a 16.7 ms average frame interval with ~190 views; D-pad repeat, OK, Back, details → Back focus restore,
dialog trap and restore, double-Back exit; pointer hover, click (left button only, no duplicate Enter),
wheel (one event per notch), arrow while the pointer shows. The owner judged the spring animation
smoother than the first build. Not resolved: 4-14 intervals per 300 frames exceed 20 ms even when idle
(most are made up by the next frame; ~1-2 real drops per 300) and occasional 25-52 ms stalls inside
`SDL_PollEvent`; the SDL-webOS cursor show/hide keys never arrived.

Simplifications: transforms are translation only, so focus "zoom" is a ring rather than a scale; cells in
one collection share one size; ScrollView does not combine nested scroll offsets; no pointer
auto-scroll at list edges (the wheel scrolls).

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
Added later the same day: `storage::SecretStore` (keymanager3 AES-256-GCM envelopes in DB8 where the firmware
has it, a labelled plain envelope otherwise, no silent downgrade; on webOS 6.5.3 keymanager3 does not exist)
and `net::WebSocketClient` (RFC 6455 over libcurl CONNECT_ONLY because the TV's curl 7.61 predates curl's
WebSocket API; own I/O thread, keepalive, close handshake, reconnect with backoff that only resets after a
stable connection; ws:// and wss:// verified on the TV). Apps use nlohmann/json directly.

## M6 — Lifecycle and diagnostics

Full lifecycle state machine; deterministic shutdown with join deadlines; crash record; capability
system (`Supported`/`Unsupported`/`Unknown`) + device info; metrics overlay (frame time, textures,
queue depth, cache); machine-readable compatibility DB (`compat/records.json`) with schema.

### M6 status (2026-10-11)

Implemented: `app/` — `Lifecycle` state machine (Created → Initialising → Running ⇄ Suspended → Resuming,
Stopping → Terminated) and the app shell (`wnk::app::run` + `Delegate`) that owns process setup, SDL/GL,
fonts, worker pool, dispatcher, optional HTTP/storage/images, input routing, Exit/double-Back, background
suspension (no drawing, input ignored), low-memory trimming, and a timed shutdown sequence under a
watchdog. `diagnostics/` — per-phase frame metrics with a late threshold that follows the measured
refresh rate, RSS, an async-signal-safe crash record with backtrace and executable mappings (reported on
the next launch; `tools/symbolize-crash.py` resolves it), capabilities (Supported/Unsupported/Unknown)
and device info through an allow-list. `docs/compatibility/schema.json` + `tools/check-compat.py`. An
on-screen metrics overlay. Example B runs on the shell.

| Item | Evidence |
|---|---|
| Lifecycle (4), frame metrics (3), crash (4, forked children crash for real), capabilities/device info (3), probe redaction unit tests; 8 compat checker tests | `[TEST]` |
| e2e: background/foreground (input while suspended dropped), timed shutdown, crash then report on next launch, overlay toggle | `[TEST]` |
| All clean under ASan/UBSan | `[TEST]` `host-asan` |
| ARM build passes check-package; loads on 5.3.1–11.2.0; backtrace, libgcc_s.so.1, sigaltstack on every inventoried firmware | `[BUILD]` `[FWDB]` |

On hardware (OLED65C1PTB, webOS 6.5.3, 2026-10-11; `docs/compatibility/records.json`): another app over
the kit app sends MINIMIZED + WILL/DIDENTERBACKGROUND, returning sends RESTORED + WILL/DIDENTERFOREGROUND
and the first frame follows in 8 ms; the process stays alive and draws nothing meanwhile. SAM close runs
the whole shutdown in 11.3 ms. A deliberate SIGSEGV leaves a record that the next launch reports and that
symbolises to source lines; the firmware preloads `libSegFault.so`, whose handlers ours replace.
Capabilities on this TV: Mali-G51, highp, 8192 textures, 1920x1080 drawable, 3840x2160 panel. In a later
session the output ran at 50 Hz (two builds A/B'd); `SDL_webOSGetRefreshRate` still said 120.

Not verified: the Home key and the Exit key as background/close paths, SAM's SIGKILL deadline, the
watchdog firing on a real hang, low-memory events, the overlay judged on screen.

## M7 — Media foundation

`MediaSource`, `PlaybackController` state machine (host-tested with a scripted backend), NDL v2
backend (dlopen, single mutex, lazy init, emergency release), PTS-unit device experiment, demuxer
decision (FFmpeg libavformat as shared lib vs ES-only sources). Example D. Real playback marked
unverified until seen on a TV.

### M7 status (2026-10-11)

Implemented: `media/` — packet/track model and `MediaSource`/`MediaBackend` interfaces; `PlaybackController`
(playback thread, Idle → Opening → Ready → Playing ⇄ Paused, Seeking, Ended, Error, Stopped; backpressure
plus a 1 s feeding lead on its own clock; coalesced seeks; pause by releasing the decoder when the backend
cannot pause; estimated position); `FfmpegSource` on a bundled, pinned FFmpeg 9.0 (`tools/build-ffmpeg.sh`,
shared LGPL, `-wnk` suffix, same components on ARM and host: MKV/MP4/TS/raw demux, Annex-B filters, AAC/AC-3/
E-AC-3/MP3 decoded to S16 stereo with a 5.1 downmix); `NdlBackend` (dlopen, one mutex, lazy init, release at
exit and on crash, layout asserted against the NDK header); `PacingBackend` for host runs. Example D plays
bundled A/V-sync clips (`tools/make-test-clips.sh`).

| Item | Evidence |
|---|---|
| 12 controller tests (scripted source, paced test backend), 20+10 ThreadSanitizer runs | `[TEST]` |
| 8 FFmpeg source tests on clips generated by the system ffmpeg (MP4/MKV/TS/WebM, seek, errors, downmix) | `[TEST]` |
| 4 e2e runs of Example D on the host (pause/seek/resume, Back stops, end, missing file) | `[TEST]` |
| ARM build: package with the bundled FFmpeg loads on 5.3.1–11.2.0; NDL v2 symbols exist on 5.3.1+ | `[BUILD]` `[FWDB]` |

On hardware (OLED65C1PTB, webOS 6.5.3, 2026-10-11; `docs/compatibility/records.json`): H.264+AAC, HEVC+AC-3 5.1
and H.264+MP3 1080p25 play with picture and sound in sync once feeding is paced; NDL offers no backpressure;
MP3 passthrough is silent (so MP3 is decoded); the PTS unit could not be told apart by eye.

HTTP sources (2026-10-11): `net::HttpStream` reads one resource with byte-range requests through the kit's
`HttpTransport` (new streaming entry point: body to a sink, stall timeout instead of a transfer timeout, no
content encoding), keeps a read-ahead ring buffer filled by its own download thread, serves short seeks from
the buffer and reconnects at the first missing byte after drops, 502/503/504 or truncation (a reconnect must
get 206 for that offset and the same size). `FfmpegSource` opens http(s) URIs through it with a custom
AVIOContext, so the bundled FFmpeg stays without network protocols and TLS follows the kit's rules. Example D
plays `{"uri": "http://..."}` (0.2.0).

| Item | Evidence |
|---|---|
| 12 HttpStream tests on a scripted range server (seek, refill, drops, retries, no ranges, interrupt); 30 ThreadSanitizer runs | `[TEST]` |
| curl streaming against the local server: chunks, receiver stop, ranges, resume after drops, HTTPS | `[TEST]` |
| FFmpeg over HTTP = local file, packet for packet (MP4 index first and last, MKV, TS); 404 reported as "HTTP 404" | `[TEST]` |
| ARM build: player 0.2.0 loads on 5.3.1–11.2.0, no new DT_NEEDED (libcurl stays dlopened) | `[BUILD]` `[FWDB]` |

On hardware (OLED65C1PTB, webOS 6.5.3, 2026-10-11): 180 s 1080p MP4 (index at the end) and MPEG-TS over LAN
HTTP play in sync; the MP4 opens in 78 ms; seeks are back to playing in 170–440 ms and TS seeks start on a
keyframe. The picture went black during every seek and pause because NDL v2 was unloaded and reloaded;
since player 0.3.0 pause only stops feeding and seeks splice the timeline (no black, sound in sync,
measured on the same TV; `docs/research/NATIVE-MEDIA.md`).

Found on the way and fixed: MPEG-TS has no keyframe index, so FFmpeg's seek lands near the timestamp (before
or after it) and video was dropped until the next keyframe. `FfmpegSource::seek` now reads forward to the
target and replays from the last keyframe before it, starting 1–16 s further back (or at the first byte)
when there is none.

Subtitles (2026-10-11): every subtitle track FFmpeg can decode (SubRip, ASS/SSA, WebVTT, MP4 timed text,
PGS, DVD, DVB) becomes cues in a `SubtitleStore` that survives seeks; `SubtitleView` (wnk_media_ui) draws
text with an outline at the bottom of the video and maps bitmaps from their canvas. Example D 0.4.0 shows
the default track and cycles tracks with Down. On the TV: SubRip in MKV in time, switching and seeking.

Starfish backend (2026-10-11): `StarfishBackend` drives LG's StarfishMediaAPIs (dlopened libplayerAPIs,
SDL-webOS exported window, BUFFERSTREAM payload with the stream's frame rate) after PlxNative's seam. On
the TV it plays 24 fps without the NDL judder (24 frames per second presented), with a real pause, the
decoder's position and AAC passed through. Example D 0.6 chooses per file with `AutoBackend`: Starfish when it
takes the video and audio codecs as they are, NDL otherwise (AV1, VP9, MP3/Opus/PCM audio).

AC-3 re-encoding (2026-10-11): audio Starfish cannot take (MP3/MP2, Opus, Vorbis, FLAC, TrueHD, DTS, PCM) is
decoded by the bundled FFmpeg and re-encoded with its native AC-3 encoder (libswresample for 7.1 → 5.1 and
rates other than 32/44.1/48 kHz), so `AutoBackend` keeps such files on Starfish; `audio_path()` holds the
rule (as is, else PCM, else AC-3). Example D 0.7 (`{"ac3": false}` turns it off). On the TV: 7.1 FLAC
96 kHz and Opus in sync; the encoder costs 9% of a core for 7.1 96 kHz, 1.5% for stereo.

DASH and HLS (2026-10-12): `manifest.hpp` parses MPDs (SegmentTemplate/Timeline, SegmentList, SegmentBase +
sidx) and HLS playlists into one rendition model; `net::SegmentReader` reads init + segments as one stream;
`AdaptiveSource` picks a video rendition by codec, size and bandwidth, offers the audio renditions as
tracks, runs one FfmpegSource per rendition and merges them by decode time; seeks reopen at the segment.
Example D 0.8 lists four public streams. On the TV: Dolby 4K HEVC + E-AC-3, Apple HLS fMP4, Mux HLS-TS
and Shaka DASH play and seek in sync. Example D 0.9 declares single-layer Dolby Vision (DolbyHdrInfo) and
Atmos (immersive) to Starfish when the TV reports support: the TV shows both. Example D 0.10 switches
video bitrate with the measured throughput (TV: Mux 720p to 1080p seamlessly); 0.11 shows the manifests'
subtitles (HLS WebVTT, DASH text/vtt and wvtt); 0.12 plays live DASH and HLS (TV: livesim2 and Unified
Streaming). Live subtitles
follow their lists (0.13). DASH TTML subtitles (files and `stpp`) and TTML files beside the media
(0.30, host tests). DASH audio switches bitrate within its AdaptationSet, HLS audio follows the
video's variant into its audio group (0.32, host tests). Open: TTML in HLS,
live, subtitles from manifests, WebM renditions, DRM (refused).
Several Periods in a static MPD play in sequence and WebM DASH (VP9, Opus, Cues index) plays
(2026-10-20, host tests; public multi-Period and WebM streams on the host).

Configuration: `DeviceMediaCaps` reads HEVC, panel, HDR, Dolby Vision and Atmos support from
com.webos.service.config (the decoder table is not visible in the Developer Mode jail).

Starfish extras (2026-10-19, from Kodi's webOS pipeline and PlxNative): VP9 and AV1 on Starfish where
`getMaxVideoResolution` reports a decoder (an unknown name gets the 1920x1088 default, so only a larger
answer counts), AV1 fed as temporal units, the TV's decoder size bounding the envelope, container
HDR10/HLG metadata declared with `setHdrInfo`, HDR10+ SEI messages removed from HEVC, `ac3PlusInfo` for
E-AC-3, and `AudioOutputCaps` (getARCState). Example D 0.20 logs the panel's dynamic range every 5 s.
On the TV: VP9 and AV1 10-bit PQ play with 0 drops and switch the panel to HDR only with setHdrInfo
(A/B: without it, sdr); DV + Atmos and SDR unchanged; HDR10+ HEVC plays with and without stripping.
TrueHD cannot be passed through from an app (`docs/research/AUDIO-PASSTHROUGH.md`).

Open: subtitles from external files, ASS styling (libass), non-Latin fonts, bitmap subtitles on the TV,
VP9 PQ without mastering values (stays SDR), Dolby Vision profile 7 (base layer only),
Opus passthrough, resampling for sample rates NDL lacks, pause and seek latency (the queued lead, up to 1 s), video-only files (reported to need an audio prime),
a decoder-side position, real pause via Starfish, and a PTS-unit test that does not feed in real time.

## M8–M10

Reference apps A–D polished; Jellyfin client (app-only module) on top; compatibility test runs,
licence audit, release workflow.

### M8 status (2026-10-12)

Example C, `applications/examples/shows` (org.wnk.examples.shows): browses the public TVmaze API
(HTTPS JSON, no key). It is the reference for the network side of an app: requests and JSON parsing on
the worker pool, results delivered through the dispatcher and dropped when the asking screen is gone,
a paged grid (focus near the end fetches the next page; 404 ends it), a details screen whose request is
cancelled on Back, failures turned into one sentence with Try again (transient ones retried by
`HttpClient` first), a saved copy of the catalogue in DB8 shown at launch and kept when offline, and
favourites written with compare-and-set. The catalogue code (`catalogue.cpp`) has no UI or threads and
has its own unit tests.

| Evidence | Tag |
|---|---|
| 10 unit tests of the catalogue code (malformed pages, HTML summaries, saved copy within the value limit, favourites, rails, error text) | `[TEST]` `unit.shows_catalogue` |
| 7 host runs against synthetic `/catalogue` routes of `http_server.py` (details and Back, 503 then Try again, unreadable reply, Back cancels a slow request, paging to the 404, saved copy and favourite used by an offline second launch) | `[TEST]` `e2e.shows.*` |
| On the TV: firmware libcurl 7.61 / OpenSSL 1.1.1t fetches page 0 (~370 KB) in 2.1 s; the saved copy is written to DB8 and on the next launch shown 333 ms after start | `[DEVICE]` records.json |

The app raises `worker_threads` to 4: posters and API requests share the pool, and with 2 workers a
0.36 s details request answered 0.98 s after OK (host).

Example D 0.14 adds the audio half of "audio/subtitle track UI": `PlaybackController::set_audio_track`
switches the audio track during playback (decoder reloaded, restart at the keyframe at or before the
position; paused stays paused), and Up opens a track menu listing audio and subtitle tracks. The
"Audio and subtitle tracks" test clip now has two AAC tracks (1 kHz English, 500 Hz French).

| Evidence | Tag |
|---|---|
| 3 controller tests (switch while playing continues from the position, while paused stays paused, a refused track keeps the current one) | `[TEST]` `unit.media` |
| Host run: menu opens on the playing track, Right + OK plays the French track | `[TEST]` `e2e.player.tracks_menu_switches_audio` |
| On the TV: scripted switches through Starfish and NDL on the clip and on Shaka DASH (Spanish to French) | `[DEVICE]` records.json |

Examples A and B needed no new features against their lists. Example A's header and the README now
describe A as the diagnostics app (no app shell; apps start from B).

Open: the Example C checks with a remote on the TV (details, Back during a load, favourites, paging,
Magic Remote), and judging the switched audio by ear.

### Auth and sessions (2026-10-12)

`services/auth` (`wnk_auth`), the mission's optional authentication service. It sits on the storage
and network layers, and nothing in the platform layers depends on it.
`Credentials` are provider-neutral (user, server, access and refresh token, expiry, extra fields).
`CredentialStore` seals them per account through `SecretStore` and keeps a non-secret account list.
`installation_id` gives a random per-install device id. `Session` runs on the main thread: restore at
launch, sign-in from the app's own login flow, authorized `send` with a refresh before expiry, one
shared refresh on 401 and a retry, sign-out on a rejected refresh, and sign-out with revoke and
removal. The provider-specific parts (`authorize`, `refresh`, `revoke`) are an `Authenticator` the
app implements.

| Evidence | Tag |
|---|---|
| 13 unit tests: storage round trip and account order, restore, concurrent 401s sharing one refresh, rejected and failed refresh, refresh before expiry, 401 without a refresh token, sign-out during a refresh, dead callers, no token in any log line | `[TEST]` `unit.auth` |
| Clean under ASan and under TSan (20 runs) | `[TEST]` |
| The same 13 tests on the TV's CPU (`tools/device-test.sh test_auth`) | `[DEVICE]` records.json |

Not yet used by an app: the first will be the Jellyfin client. Not covered: keymanager3 sealing on a
TV that has it (webOS 6.5.3 does not), and a login UI (the kit has no text input).

### Protected streams: ClearKey (2026-10-12)

Common Encryption with keys the app has. The bundled FFmpeg 9.0 MP4 demuxer already decrypts 'cenc',
'cens', 'cbc1' and 'cbcs' from a key-ID → key dictionary, so `FfmpegSource` passes
`decryption_keys` to it and the decoders receive clear data. A sample still encrypted (no key) fails
the read with a clear message. The DASH parser reads ContentProtection (scheme, default KIDs, and the
ClearKey licence URL in `clearkey:Laurl` / `dashif:Laurl`). `AdaptiveSource` asks a W3C ClearKey
licence server for missing keys and plays encrypted renditions it has keys for. `clearkey.hpp` parses
`kid:key` lists and JWK sets. Example D 0.15 takes `{"keys": ..., "licenseUrl": ...}` and lists three
protected test streams.

HLS `EXT-X-KEY` (Example D 0.16): the playlist parser keeps the key in effect per segment (method,
URI, IV, KEYFORMAT; NONE ends it; EXT-X-MAP takes the key at its position). `SegmentReader` parts can
carry a filter that rewrites their bytes on the download thread; AES-128 segments get an AES-128-CBC
filter (libavutil AES, streamed, PKCS#7 padding removed, IV from the tag or the media sequence number)
whose key is fetched once per URI. SAMPLE-AES / SAMPLE-AES-CTR in fMP4 use the CENC path with the URI's
key as FFmpeg's default key. Refused with a reason: SAMPLE-AES in MPEG-TS, non-identity key formats
(FairPlay). Not covered: DASH key rotation, Widevine/PlayReady.

| Evidence | Tag |
|---|---|
| Key and licence parsing, licence exchange with the test server (ok, 403, no matching key), DASH ContentProtection; a CENC MP4 read with the key equals the clear file packet for packet, a wrong key differs, no key fails | `[TEST]` `integration.clearkey` |
| On the TV: Shaka angel-one-clearkey (cenc, key given), livesim2 eccp_cenc and eccp_cbcs (live, key from the licence server) through Starfish, 0 dropped frames | `[DEVICE]` records.json |
| HLS: AES-128 TS decrypted equals the clear presentation packet for packet, also with the sequence-number IV (segments encrypted by the openssl CLI); EXT-X-KEY parsing; SAMPLE-AES TS, FairPlay and a missing key URI refused | `[TEST]` `integration.clearkey` |
| On the TV: JW Player oceans (AES-128 VOD), Mux deltatre (a new key per segment), a live AES-128 HLS stream from the LAN, through Starfish, 0 dropped frames | `[DEVICE]` records.json |

ffmpeg's own fragmented CENC output does not demux (its sample-encryption boxes are inconsistent),
so the DASH test checks the manifest and licence path, and fragmented decryption is checked with the
public streams.

### M10 status (2026-10-12): licence audit and release workflow

Licence audit: every component compiled into or bundled with a package is GPL-3.0-compatible (zlib,
MIT or public domain, OFL-1.1, LGPL-2.1-or-later FFmpeg built without GPL or non-free parts); LG's
libraries and libcurl come from the TV. Packages now carry `licenses/` (GPL text, notices, the
licence of each bundled library, and `SOURCE.txt` naming the source and, with FFmpeg, the exact
tarball); `check-package.py` refuses packages without them. Details in `LICENSING.md`.

Release workflow: `tools/release.sh` (see `docs/RELEASING.md`) builds every package from a clean
tree, runs the tests and checks, writes the source archive, the verified FFmpeg tarball,
`RELEASE-NOTES.md` (generated from the packages, fw-check and the TV records) and `SHA256SUMS`.

| Evidence | Tag |
|---|---|
| Packaging tool tests, with missing-licence and bundled-FFmpeg cases | `[TEST]` 22 tests |
| Release 0.1.0 from 54ebef1: four packages pass check-package and fw-check 7/7 (webOS 5.3.1-11.2.0) | `[BUILD]` |
| Two builds in different directories give byte-identical packages (after mapping SDL's build paths, whose asserts embedded them) | `[BUILD]` `--check-reproducible` |

Open: installing and launching the release packages on the TV (the TV was off the network when the
release was made), and tagging `v0.1.0`, which the script leaves to the owner.

## Device dependency

Every milestone from M2 has a device column. Without a TV the kit can reach "builds, verifies, host
tests pass" but **not** "works on webOS 6". That gap is recorded honestly in the compatibility DB.
