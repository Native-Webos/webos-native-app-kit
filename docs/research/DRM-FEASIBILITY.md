# Widevine and PlayReady from a native Developer Mode app: feasibility

Status 2026-10-12: **PlayReady Smooth Streaming plays through the platform pipeline; DASH and Widevine open.** ClearKey and HLS AES-128 already play in the kit
(`media/clearkey.hpp`, `AdaptiveSource`), because the app holds the keys and FFmpeg decrypts. Widevine
and PlayReady never give the app a key: decryption happens in LG's DRM stack (a TEE on most models),
so the content has to go through the platform's protected path. This file records what that path
is and how far a native Developer Mode app gets along it.

## What the firmware has

From the firmware symbol inventory (`~/.cache/plxnative/fwsym/data`, 14 images, webOS 3.x–24):

| Piece | Libraries | Present in |
|---|---|---|
| LG DRM service and plumbing | `libdrmcontroller`, `libdrmhandler`, `libdrmadaptor`, `libdrmddi`, `libdile_drm`, `libsm_appdrm` | all 14 |
| Widevine (modular, CENC) | `libwvcdm_shared` / `libwidevine_ce_cdm_shared` 3.x–17.1, `libwidevine-wrapper` (webOS 22+) | all but the oldest |
| Widevine classic | `libWVStreamControlAPI` | older images |
| PlayReady | `libplayready` 2.5 (REE/TEE), 3.0, 4.0, 4.4, 4.6, `libplayreadycdm` | all |
| Secure decode path | `libsecurepathcontrol`, `libdrmdecryptor`, `libNDL_esdrm_impl` (webOS 22+), `libNDL_appdrm` (old) | varies |

None of these is in the NDK, and the kit must not link or bundle them (see `AGENT-APP-GUIDE.md`).

## What the NDK exposes

- `StarfishMediaAPIs::setDrmOperation(const char *)`: the payload is undocumented.
- `uMediaClient` (C++, `libumedia_api`) and its C wrapper (`libumedia_api_c`: create, load(uri,
  audio class, payload), play, pause, seek, unload). This is the platform pipeline that HTML `<video>`
  uses. It parses DASH/Smooth/HLS itself and takes DRM options in the load payload. The C wrapper gives
  no events back.
- The Luna services `com.webos.service.drm` (licences) and `com.webos.media` (uMediaServer). LG's web
  app documentation pairs them: `drm/load` → `clientId`, `drm/sendDrmMessage` with the licence
  initiator, then the media is loaded with `{"option":{"drm":{"type":…,"clientId":…}}}`.

## Measured on the OLED65C1PTB (webOS 6.5.3), Example A 0.8.0 `{"drmProbe": true}`

| Call (from the app's own bus identity) | Reply |
|---|---|
| `drm/load {"drmType":"widevine","appId":…}` | `{"returnValue":true,"clientId":"…"}` |
| `drm/load {"drmType":"playready","appId":…}` | `{"returnValue":true,"clientId":"…"}` |
| `drm/unload {"clientId":…}` | `{"returnValue":true}` for both |
| `drm/getRightsError` without subscribing | 504 "not valid data or format" (expects a subscription) |

So the DRM service accepts a Developer Mode native app: there is no permission wall at the first step.

## Platform pipeline run (Example D 0.17.0, `{"drmTest": ...}`)

| Content | Result |
|---|---|
| Shaka angel-one, clear DASH, `drm: none`, `MPEG-DASH` | loadCompleted, playing (LG picked the VP9 rendition) |
| Microsoft SuperSpeedway 720p, PlayReady Smooth Streaming, `MSIIS` | initiator `DRM_RESULT 0`; loadCompleted, playing, 1280x720 H.264 on a hardware decoder, no rights errors |
| Microsoft Tears of Steel 4K, PlayReady DASH, `MPEG-DASH` | initiator `DRM_RESULT 0`; "pipeline died abnormally" after 3.8 s, nothing buffered |

The sequence that works: SDL exported window → `drm/load` → `drm/sendDrmMessage` (initiator XML,
`urn:dvb:casystemid:19219`) → `com.webos.media/load` `{"uri","type":"media","payload":{"option":
{"appId","windowId","drm":{"type","clientId"},"mediaTransportType"}}}` → `com.webos.media/subscribe` →
`play`. Events arrive on the subscription (`loadCompleted`, `playing`, `videoInfo`, `error`).

## Not known yet

1. Whether the picture shows under the app's transparent SDL window (the events say it plays).
2. Why PlayReady DASH (Tears of Steel 4K) kills the pipeline while Smooth Streaming plays: the 4K
   renditions, the DASH transport name, or something in the DRM option.
3. Widevine modular through the same service: LG documents PlayReady for this route; Widevine on webOS
   is otherwise reached through the browser's EME (Chromium CDM), which a native app does not have.

Protected playback is therefore a separate backend: the platform plays the URL and the
kit's controller, sources and subtitles are bypassed.
