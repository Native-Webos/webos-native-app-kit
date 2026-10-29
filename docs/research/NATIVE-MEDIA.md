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
- **Pause and seek without the black picture (2026-10-11):** unloading NDL blanks the video plane until
  the next load presents a frame. Stopping the feed instead keeps the decoder loaded: the ~1 s queued
  plays out, the picture holds on the last frame, and feeding again later resumes in sync (NDL's clock
  waits for data). A seek that flushes nothing and shifts later PTS to continue 200 ms after the last one
  fed shows the new content once the queue has played out (0.5-0.8 s at a 1 s lead), with sound in sync,
  through bursts of quick seeks. `NDL_DirectVideoFlushRenderBuffer` (webOS 5.3.1+) flushed video in
  4-15 ms, but **sound was gone for the rest of the session**, as punktfunk warned for flushes.
  `NDL_DirectAudioPause`/`Resume` exist only in the v1 library (webOS 4.x firmware), not in v2.
- **24 fps judders, 25 fps does not (2026-10-11):** the same Big Buck Bunny footage played at 24 fps shows
  pulldown judder on pans and is smooth retimed to 25 fps; the network was ruled out (LAN and internet
  alike, feeding never starved). NDL v2 has no frame-rate field; `video.unknown1 = 24` was accepted and
  changed nothing. PlxNative avoids this on Starfish by declaring `adaptiveStreaming.maxFrameRate` as the
  stream's rate class (it measured 13 fps presented for 24p declared at 60, 24.1 declared at 24) `[SRC]`.
- AV1 (360p, no audio track) and AAC 44.1 kHz decoded to PCM play correctly on this TV.
- PlxNative (Starfish) has a real `Pause()`, but on webOS 5+ it seeks by a full Unload + Load: its
  in-place seek relies on structure offsets read off webOS 4.5 binaries and is disabled elsewhere
  (`rust-modules/media/src/player/engine.rs`) `[SRC]`.

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

### Decoded audio: stereo PCM on Starfish, AC-3 for more channels `[HW]`

Audio Starfish cannot take as it is (FLAC, ALAC, LPCM, Opus, MP3, Vorbis, TrueHD, DTS) is decoded in
the app. A mono or stereo track is fed as PCM (`"audio":"PCM"` with `contents.pcmInfo`: S16LE,
interleaved, `channelMode` mono or stereo, `sampleRate` in kHz); a wider track is re-encoded to AC-3
5.1, as before, because libpf takes PCM's channel count from `channelMode` alone. Measured on the
OLED65C1PTB (webOS 6.5.3, 2026-10-29): `channelMode` stereo and dual give 2 channels, mono, multi,
5.1 and 6 give 1, and a `channels` key is ignored, so 5.1 and 7.1 PCM cannot be declared; NDL's PCM
has only the same `channelMode` (its `NDL_DirectAudioSupportMultiChannel` answers 1, "no device
takes multichannel PCM", with an eARC soundbar). A PCM Load runs on a `PCMMC` mixer channel, AC-3 on
`ADEC`.

`BackendCaps::pcm_rates` lists the rates a backend can be told (Starfish: libpf's 8-48 kHz table plus
44.1; NDL: its enum); the source resamples anything else with libswresample to 44.1 kHz (multiples of
it) or 48 kHz (`pcm_output_rate`), so 96 kHz FLAC now plays on NDL too, which refused it.

A PCM Load skips the decoder path's lip-sync stage, so PCM plays about 0.4 s ahead of the picture
unless it is stamped later (PlxNative #1087, filmed on webOS 4.10 TV speakers). `StarfishBackend`
adds a lead by the audio stack (its sink plugin) and the sound output (settingsservice
`soundOutput`): liblxa TV speakers 400 ms, Bluetooth 250 ms, other outputs 400; lgaudiosink 400;
adecsink and unknown stacks 0 (`StarfishOptions::pcm_lead_ms` overrides; Example D
`{"pcmLeadMs":N}`). The OLED65C1PTB is liblxa and its soundbar reports `external_arc`: 400 ms by
default, not yet judged by ear.

### Playback speed `[HW]`

`PlaybackController::set_speed` (0.5-2x in Example D's track menu) is done in the app, as PlxNative
does it (#805): LG's `SetPlayRate` cannot drive the buffer-feed pipeline (reset to 1x by the next
SEGMENT, a freeze mid-play, muted audio off 1x) and is not exported on any image in the inventory.
Packets keep content time; the backend divides every timestamp it gives the decoder by the speed
(`MediaBackend::to_sink`) and reports positions multiplied back, so the decoder plays at its own 1x
and is told the frame rate it is actually fed. The source decodes the audio to stereo PCM and
time-stretches it with `TimeStretch` (WSOLA, pitch kept; host-tested at 0.5-2x: length within 50 ms,
pitch within 4 Hz, no clicks). A change reloads the decoder at the position; a speed whose fed frame
rate would pass 60 fps, DASH/HLS, and audio that cannot be decoded are refused.

Measured on the OLED65C1PTB (player 0.27.0, 2026-10-29), a 24 fps H.264+AAC clip: the sink displayed
36 fps at 1.5x, 48 at 2x and 12 at 0.5x, the position advanced at that rate, a native seek at 2x
landed in 214 ms, 0 frames dropped throughout. Not judged by ear (pitch, lip sync).

### Seeking with LG's Seek `[HW]`

`StarfishMediaAPIs::Seek(const char *millis)` (exported on every release 1.2-11.2) runs a
`FLUSH | KEY_UNIT` seek and starts a new segment at the milliseconds passed (PlxNative's decompile of
webOS 4.5, its #568). `StarfishBackend` uses it on webOS 5+ and webOS 3 (PlxNative: 80 seeks on
3.9.3); webOS 4 keeps splicing. `flush()` marks a Seek pending; the first packet after it issues
`Seek(<its own ms>)` on the feeding thread before it is fed, and the timeline becomes content time
(`PtsSplice::reanchor`, kept non-negative). A refused Seek reloads the decoder and the backend splices
from then on. `{"seekMode":"splice"}` in Example D compares the two.

Measured on the OLED65C1PTB (webOS 6.5.3, player 0.24.0, 2026-10-29), the same 90 s H.264+AAC clip and
five scripted seeks (+20, -10, +30, -25, +40 s):

| | First new frame after the request | Dropped frames |
|---|---|---|
| LG's Seek | 0.17-0.33 s (100-270 ms after the call) | 0 of 911 |
| Splice | 1.0-1.9 s (seek settle, then the decoder's backlog: 0.6-1.45 s) | 2 of 1026 |

A 4K Dolby Vision Profile 7 clip (fed as 8.1) kept the panel in `dolbyHdr` through five native seeks,
three of them 0.4 s apart (each landed in 127-206 ms), with 0 of 930 frames dropped.

### Dolby Vision Profile 7 `[HW]`

UHD Blu-ray remuxes carry Profile 7: an HDR10 base layer, an enhancement layer and RPUs. LG's
pipeline takes single-layer DV only, so `FfmpegSource` feeds such a stream as Profile 8.1, as
`dovi_tool -m 2` and PlxNative (#825) do (`wnk/media/dovi.hpp`): every enhancement-layer NAL unit
(UNSPEC63, or nuh_layer_id > 0) is dropped and every RPU (UNSPEC62) is rewritten by libdovi
(quietvoid, MIT; `cmake/LibDovi.cmake` builds the pinned crate with cargo). Starfish is told
`DolbyHdrInfo` profileId 8. Only records with an enhancement layer and a base layer compatible with
HDR10 (`dv_bl_signal_compatibility_id` 6) convert; an RPU that fails to convert is dropped, never
passed on as Profile 7. Lost: the enhancement layer's detail above the base layer. Without libdovi
(`-DWNK_DOVI=OFF`) the same stream plays as its HDR10 base layer.

Measured on the OLED65C1PTB (webOS 6.5.3, 2026-10-29) with a 60 s cut of a 4K Profile 7 remux
(compatibility 6, TrueHD 7.1): the panel reports `dynamicRange dolbyHdr` in `dolbyHdrCinema`, 24 fps
with 0 dropped frames over 1066 frames and a seek, one RPU converted and 18 enhancement-layer units
dropped per frame. The player used 38% of one core with the conversion and 38% with the RPU and
enhancement-layer units removed beforehand, so the conversion's cost is below what `top` resolves
(the rest is the remux's HTTP read, demux and the TrueHD -> AC-3 re-encode).

### webOS 3.x and 4.x (2016-2019 sets) `[FWDB]`, `[SRC]`

Before this change the kit played nothing on these sets: `StarfishBackend` needed the webOS 5
exported window, and `NdlBackend` needs `NDL_DirectMediaLoad`, which only exists from 5.3.1 (3.9-4.10
ship the v1 `NDL_DirectVideoOpen` API: H.264 SDR only, no PTS input, per client-webos's `ndl/v1.rs`).
`StarfishBackend` now follows Kodi's `MediaPipelineWebOS.cpp` there:

| Difference | webOS 3.x / 4.x | webOS 5+ | Source |
|---|---|---|---|
| Video plane | ACB (`libAcbAPI.so.1`, type MSE): `setSinkType`, `setMediaId(getMediaID())` and `setState(LOADED)` at loadCompleted, `setState` on PLAYING/PAUSED/UNLOADED, every `STR_VIDEO_INFO` (4) to `setMediaVideoData`, `STR_AUDIO_INFO` (7) to `setMediaAudioData` (absent on 3.4), `setCustomDisplayWindow` places it | SDL exported window, `option.windowId` | Kodi, ss4s `smp_resource_acb.c`, PlxNative |
| `Feed` reply | 3.x: pre-C++11 copy-on-write string (`_ZN17StarfishMediaAPIs4FeedEPKc`), released with the firmware's `_ZNSsD1Ev`; 4.4.2+: `B5cxx11` | `B5cxx11` | inventory, PlxNative |
| Callback types above ENDOFSTREAM (0x1c) | two lower (dropped/displayed frames 46/47) | 48/49 | Kodi `type += 2`, PlxNative decompile of 4.10 |
| HDR10 from the container | no `setHdrInfo` (absent through 4.10.0): a PQ stream the pipeline calls `hdrType none` has its video-info envelope rewritten to HDR10 with `mediaSei`/`mediaVui` before ACB gets it | `setHdrInfo` with `sei`/`vui` | PlxNative `vp9_hdr.rs`, Kodi |
| Dolby Vision declaration | never on 3.x (a declared Load never started on 3.9.3) | when the TV says supported | PlxNative `caps.rs` |

The release comes from `/var/run/nyx/os_info.json` `webos_release` (`StarfishOptions::webos_major`
overrides it). 1-4 try ACB first, anything else the exported window first; each falls back to the other.
Seeks keep the kit's PTS splice (no `Seek`/`flush` call), which is era-independent. Not measured on a
3.x/4.x TV: the kit's bundled SDL 2.30 on those compositors, and whether the UI plane is transparent
over the ACB video plane (PlxNative needed SDL's opaque region cleared with LG's system SDL 2.0.4/2.0.5).

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
