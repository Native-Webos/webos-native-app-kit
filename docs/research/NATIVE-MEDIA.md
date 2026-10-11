# Native media

Evidence tags: see [README.md](README.md). Link prefixes: see [PROJECT-COMPARISON.md](PROJECT-COMPARISON.md).

## The three native decoder APIs (webosbrew/samples README) `[SRC]`

| | Starfish media pipeline (SMP) | NDL DirectMedia | LGNC |
|---|---|---|---|
| Library | `libplayerAPIs.so.1` (C++) | `libNDL_directmedia.so.1` (C) | `liblgncopenapi` |
| Firmware | all `[FWDB]` | 3.9.2+ (v1 `NDL_DirectMediaInit`), 5.3.1+ (v2 `NDL_DirectMediaLoad`) `[FWDB]` | webOS 1–4; links on 5 but does not play; gone in 6 (samples claim) |
| Input | elementary-stream buffers via JSON `Feed()` | ES buffers via `NDL_DirectVideoPlay`/`NDL_DirectAudioPlay` | ES buffers |
| Video plane | the app's job: ACB (≤4.x) or SDL exported window (5+) | managed by the API (`NDL_DirectVideoSetArea`) | managed by the API |
| Timestamps | ns | v1 none; v2 yes (unit disputed, below) | none |
| ABI risk | High: C++ class, `std::string` ABI changes at webOS 4, object size unknown | Low: C, but not thread-safe (punktfunk) | — |

All three take **elementary streams**: H.264/HEVC access units and audio frames. A container
(MP4/MKV/TS) or HLS/DASH source needs a **demuxer** in the app. PlxNative bundles FFmpeg
libavformat for this (`docs/ffmpeg-demuxer-plan.md`, `tools/ffabi-dump.sh`) `[SRC]`. Whether Starfish
can play a URL directly for a native app was **not** found in any source read `[UNK]`.

## NDL DirectMedia v2 (webOS 5+), the most-trodden path

Call sequence, from samples `media/ndl/directmedia/directmedia_v2.c` `[SRC]` with
`[UPSTREAM-HW]` on a 65UP7560 (webOS 6.5.2) and an OLED77C5 (webOS 10.3.1):

```
NDL_DirectMediaInit(appId, resource_released_cb)
NDL_DirectMediaSetAppState(FOREGROUND)
NDL_DirectMediaLoad(&NDL_DIRECTMEDIA_DATA_INFO_T{video{w,h,type}, audio{...}}, load_cb)
NDL_DirectVideoSetArea(x, y, w, h)
loop: NDL_DirectVideoPlay(buf, size, pts) / NDL_DirectAudioPlay(buf, size, pts)   (non-zero = buffer full)
NDL_DirectMediaUnload()
NDL_DirectMediaQuit()
```

- Load callback events share the Starfish `PF_EVENT_T` ids: `0x16` load completed, `0x1a` playing,
  `0x17` unload completed, `0x1c` end of stream (not seen with buffer sources on webOS 10.3.1) `[SRC]` (samples comment).
- v2 audio types in the sample: PCM, MP3, Opus. **No AAC** `[SRC]` (samples comment). punktfunk: v2 Opus
  sample rate is given in kHz (`48.0`) `[SRC]`.
- **Library must be dlopened.** `DT_NEEDED` breaks startup on webOS 4 (punktfunk) `[SRC]`. The
  sysroot `.so` is a stub `[SRC]`.
- **Not thread-safe:** punktfunk serialises every NDL call behind one mutex `[SRC]`.
- **Warm init breaks the first load:** *"NDL init lazy inside `load()`. Warm init at startup causes
  'not loaded' on first load"* (punktfunk) `[SRC]` (claim).
- moonlight-tv: *"DO not init video subsystem before NDL/LGNC initialization"* (`app.c`) `[SRC]`.
  The two claims are not contradictory. Exact ordering on webOS 6 is `[UNK]`; it needs a device test.
- **Resource leak across launches:** NDL stays acquired until `NDL_DirectMediaQuit`. lgnome
  releases it on fatal signals and `atexit` `[SRC]`.
- **Pacing needs an audio plane:** video-only loads ignore PTS and stutter above 60 Hz; one Opus prime
  frame at load fixes it (punktfunk, measured on CX) `[SRC]`, `[UPSTREAM-HW]`.
- **Flush pitfalls:** never flush before `LOADCOMPLETED`; a flush can silently kill audio for the
  rest of the session; audio PTS must never go backwards (punktfunk) `[SRC]`, `[UPSTREAM-HW]`.
- **HDR:** `NDL_DirectVideoSetHDRInfo` forces the panel into HDR on any call. Call it only with real
  metadata, and only on change (punktfunk) `[SRC]`.

### Measured with the kit (OLED65C1PTB, webOS 6.5.3, 2026-10-11) `[HW]`

- Init 305-465 ms cold, first load after install 1.9 s, later loads 80-170 ms; callbacks 0x16 and 0x1a
  arrive right after the first packets, 0x17 on unload, **no 0x1c** with buffer feeding.
- **No backpressure:** `NDL_DirectVideoPlay`/`NDL_DirectAudioPlay` accepted a whole 15 s clip in 0.6 s.
  Fed like that, the picture froze after a few seconds and there was no sound. Feeding paced to stay within
  1 s of a real-time clock plays H.264 and HEVC 1080p25 smoothly with PCM audio in sync.
- PCM info `"S16LE"` / `"interleaved"` / `"stereo"` / 48 kHz enum works (AAC and AC-3 5.1 decoded and
  downmixed by the app). MP3 passed through (mono 48 kHz) loaded but played **silently**.
- `NDL_DirectMediaSetAppState(FOREGROUND)` right after init returned -1.
- PTS unit: µs and ms fed in real time looked identical, so the question below stays open.

### Disputed: v2 PTS unit `[UNK]`

| Source | Unit |
|---|---|
| ss4s `ndl_player.c` `SS4S_NDL_webOS5_GetPts` | ms of `CLOCK_MONOTONIC` since load |
| punktfunk NOTES | "PTS = milliseconds since `NDL_DirectMediaLoad`" |
| samples `directmedia_v2.c` | µs (`pts_ns / 1000`), "verified" playback |

The two ms sources are streaming clients that stamp by wall clock, so either unit may "play" while
A/V sync is wrong. **This needs a measured device test.**

## Starfish (SMP) + video plane

- PlxNative calls mangled `StarfishMediaAPIs` methods directly (`Load`, `Feed`, `Play`, `Pause`,
  `Unload`, `flush`, `pushEOS`, `setTimeToDecode`, `notifyForeground`) and handles in-place seek
  through `CustomPipeline` internals (struct offsets "verified by decompile") (PLX`src/starfish.c`) `[SRC]`.
- `Play()` before `LOADCOMPLETED` is queued and returns true anyway. Re-issue `Play()` at
  `LOADCOMPLETED` (samples README) `[SRC]`.
- Four ABI build variants are needed across webOS 1–5+ (samples README) `[SRC]`. For webOS 5+ there is
  one: `__cxx11` strings, 4-arg `Load`, `(const char*)` constructor.
- Video plane on 5+: `SDL_webOSCreateExportedWindow(0)` → id goes into the `Load` payload as
  `option.windowId` → `SDL_webOSSetExportedWindow` places it (plx `vp_create_window`, `vp_place`) `[SRC]`.
- SMP supports seek, pause and richer containers (plx's player) but carries the highest C++/ABI risk.

## ss4s module pattern (moonlight-tv) `[SRC]`

Backends are separate `.so` modules (`ndl-webos5`, `ndl-webos4`, `smp`, `lgnc`, `alsa`, `pulse`,
`sdl`…) selected at runtime from a module list with OS-version checks. The interface splits Player,
Audio and Video drivers (`modapi.h`). The structure is a good model; the code is LGPL-3.0.

## Audio

- Software-decoded audio output through SDL audio is the default in punktfunk; NDL audio is the
  alternative `[SRC]`.
- `libpulse.so.0` and `libasound.so.2` exist on all firmware `[FWDB]`. Which one SDL audio uses on webOS
  is `[UNK]` (moonlight ss4s has `alsa` and `pulse` modules).
- lgnome mixes with miniaudio and feeds PCM to NDL `[SRC]` (README).

## Subtitles

- PlxNative renders ASS/SSA with a bundled libass (`libass-plx.so.0`, `src/ass.c`) and image
  subtitles; text subtitles are drawn by the UI, not the decoder `[SRC]`.
- The exported-window type `SUBTITLE` (1) exists in `SDL_webOS.h` `[SRC]`. Its use by any app was not found `[UNK]`.
- **Kit implication:** subtitles are a UI overlay fed by the media engine's track/cue model, not a
  decoder feature.

## Kit implications

1. Provider-independent `MediaSource` (URI + container/codec hints + track list) → `PlaybackController`
   (state machine) → `MediaBackend` interface.
2. First real backend: **NDL v2** (webOS 5+), dlopened, one mutex, lazy init in `load`, emergency release on fatal exit.
3. A **demuxer** is mandatory for file/HTTP playback. FFmpeg libavformat is the evidence-backed
   choice (plx). Its LGPL licence and bundling are a decision for M7.
4. Host backend: a real software path (for example FFmpeg + SDL) or explicitly test-only doubles. Never a fake decoder.
5. Controller logic (states, seek, buffering, errors) is unit-tested on the host with a scripted backend double.
