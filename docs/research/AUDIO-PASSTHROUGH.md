# Audio passthrough from a Developer Mode app: TrueHD, Atmos and the soundbar

Status 2026-10-19, measured on the OLED65C1PTB (webOS 6.5.3, firmware 03.53.45) with an eARC Atmos
soundbar. Records: `media.audio.truehd_platform_pipeline`, `media.starfish.truehd_ac3_reencode`,
`media.audio.output_arc_state`, `media.starfish.eac3_ac3plusinfo` in `docs/compatibility/records.json`.

## Summary

| Source audio | What reaches the soundbar from a kit app |
|---|---|
| E-AC-3 with Atmos (JOC) | E-AC-3 Atmos as it is (Starfish `AC3 PLUS`, `immersive: ATMOS`, `ac3PlusInfo`) |
| AC-3, E-AC-3 | as it is |
| TrueHD (with or without Atmos) | AC-3 5.1 re-encoded by the kit; the Atmos objects are lost |
| DTS | AC-3 5.1 re-encoded on this TV; DTS as it is only on TVs whose `tv.model.edidType` names DTS (Kodi) |

TrueHD cannot leave the TV from an app, although the soundbar accepts it: the limit is the TV's app
pipelines, not the eARC link.

## Why TrueHD cannot be passed through

- **Starfish (BUFFERSTREAM, what the kit feeds).** libpf's audio codec names, read with `strings` from
  `/usr/lib/libpf-1.0.so.1.0.0` on the TV: `MP3, AC3, AAC, CODA, LBR, WMA, DTS, AC3 PLUS, RA, HE AAC,
  PCM WAV, WMA PRO, VORBIS, OPUS, MPEGH, AC4, FLAC`. No TrueHD. `[SRC]` (firmware strings)
- **The hardware decoder.** `libgstlxaudiodec.so` and `libgstdecproxy.so` sink caps list `audio/x-ac3`,
  `audio/x-eac3`, `audio/x-ac4`, DTS, LPCM, FLAC, Opus and others, but no `audio/x-true-hd`. Only
  `libgstmatroska.so` (`A_TRUEHD` → `audio/x-true-hd`) and `libgstlibav.so` (software `TrueHD`) know
  the format. The decoder enum has `LXADEC_CODEC_MAT` (Dolby MAT, the eARC carrier for TrueHD), with
  no caps that an app's stream could reach. `[SRC]`
- **LG's own pipeline by URL** (`com.webos.media/load`, Example D `drmTest` with `drm: none`): an MKV
  with TrueHD gets `error 200 "Audio Codec Not Supported"`, the track is dropped (`numAudioTracks: 0`)
  and the video plays silently. The same clip with its AC-3 track plays with sound. `[HW]`
- **Kodi** (xbmc master `fe8292d`, `xbmc/cores/VideoPlayer/MediaPipelineWebOS.cpp`) feeds the same
  Starfish path and its codec table has no TrueHD either: TrueHD is decoded and re-encoded to E-AC-3
  (when eARC reports DD+) or AC-3. `[SRC]` Plex and Jellyfin forum answers say the same of their webOS
  apps. `[UPSTREAM-HW]`

## The soundbar side

`luna://com.webos.service.arccontroller/getARCState` answers a Developer Mode app (`AudioOutputCaps`):
`arcActivation: eARC`, `earcCapability: [AAC, DTS, PCM, DD, DD+, TrueHD, MAT2PCM]`,
`earcATMOS: true`, PCM up to 8 channels. `[HW]`

## What is left

1. **7.1 LPCM through Starfish (`PCM WAV`).** The decoded TrueHD bed as 8-channel PCM over eARC would
   be lossless 7.1, still without Atmos objects. Not tried.
2. **Atmos from TrueHD** would need a Dolby E-AC-3 JOC encoder (proprietary; FFmpeg's E-AC-3 encoder
   has no JOC) or a way to MAT passthrough, which no app interface reaches.
3. **An external player on an HDMI input** passes TrueHD Atmos through eARC; that path does not
   involve an app.
