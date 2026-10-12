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
| Microsoft BBB 1080p, PlayReady DASH (cenc), transport `MPEG-DASH`, `DASH`, `MPEG_DASH`, `URI` or none | "pipeline died abnormally" within 2.5 s every time |
| The same BBB DASH with no DRM option at all | "pipeline died abnormally" after 0.7 s: encrypted DASH kills the by-URL pipeline whatever the option |
| Microsoft SuperSpeedway clear Smooth (control) and the PlayReady copy, watched by the owner | both play; both stutter once the whole 2-minute title is downloaded (the reported buffer drops from ~45 s ahead to 0 at 60-70 s; the clock never stalls) |
| 24 fps 1080p MP4 through this pipeline and through the kit's Starfish path, watched | both smooth: the pipeline matches the frame rate |

The sequence that works: SDL exported window → `drm/load` → `drm/sendDrmMessage` (initiator XML,
`urn:dvb:casystemid:19219`) → `com.webos.media/load` `{"uri","type":"media","payload":{"option":
{"appId","windowId","drm":{"type","clientId"},"mediaTransportType"}}}` → `com.webos.media/subscribe` →
`play`. Events arrive on the subscription (`loadCompleted`, `playing`, `videoInfo`, `error`).

## Not known yet

1. A way to play encrypted DASH: the by-URL pipeline cannot. LG's browser plays such content through
   MSE/EME, feeding Starfish encrypted samples with DRM details (`setDrmOperation` and the Feed
   payload); those formats are not documented and would have to be worked out from the firmware.
2. Widevine through any of these routes (Widevine content is DASH or HLS, not Smooth).
3. The stutter after the whole title is downloaded: LG's Smooth Streaming pipeline, not the app
   (the clear copy does it too); not tried on a title long enough to keep it downloading.

Protected playback is therefore a separate backend: the platform plays the URL and the
kit's controller, sources and subtitles are bypassed.

## 2026-10-13: how dash.js does it, and the native equivalents

**dash.js** never decrypts. It uses the browser's EME and MSE: `requestMediaKeySystemAccess` →
`MediaKeys` on the video element → on the `encrypted` event (PSSH init data) `session.generateRequest`
→ the CDM's `message` (a licence challenge) is POSTed to the licence server → `session.update(licence)`.
Encrypted segments go to MSE `SourceBuffer.appendBuffer`; the browser's CDM and the TV's secure decode
path do the rest. On an LG TV that CDM is the firmware's `libwidevine_ce_cdm_shared.so` (present on the
OLED65C1PTB with `libplayreadycdm.so` and FairPlay CDMs).

**Native equivalents found:**

| Route | Where | Status |
|---|---|---|
| NDL media: `NDL_MediaLoadWithOption(uri, {option.drm {type, licenseUrl, useLicenseCallback}})`, `drmEncrypted` events with a challenge, `NDL_MediaSetDrmOperation(systemId, licence, "update", service, kid, sessionId, requestId)` | LG's own Flutter plugin (github.com/lg-flutter-webos/plugins, `packages/video_player_drm/webos`), Widevine and PlayReady | `NDL_MediaSetDrmOperation` only in the newest inventoried firmware (43.21.70); absent on this TV |
| `com.webos.media/setDrmOperation {mediaId, systemId, drmData, drmMsgType, service, kid, sessionId, requestId}` | `libumedia_api` (`uMediaClient::setDrmOperation`) on this TV | Exists, but this TV's pipeline never raises `drmEncrypted` (tested with `useLicenseCallback:true`) |
| webOS DRM Bridge (github.com/lennylxx/webos-drm-bridge): a native Luna service driving GStreamer DASH, the CE CDM and SecurePath directly | webOS 25 | Experimental; not for webOS 6 |

**Measured on the OLED65C1PTB (webOS 6.5.3), Example D 0.19.0 `{"drmTest":{...,"license":"url|callback|service","transportInOption":false}}`,
Shaka angel-one Widevine DASH:** with `mediaTransportType` beside `option` (as LG's plugin sends it) the
by-URL pipeline no longer dies on encrypted DASH: it parses the MPD (10 audio tracks), selects tracks,
buffers, and raises EME's `encrypted` event (`initDataType cenc`, the Widevine PSSH), then
`error 552 Encrypted Content` and `drmRightsError` (system `edef8ba9-…`) while it waits for a licence.
`option.drm.licenseUrl` does not make it fetch one; `useLicenseCallback` raises no challenge; a DRM
service client (`drm/load widevine`, `clientId` in `option.drm`) is accepted but obtains no licence by
itself.

**What remains on webOS 6:** the licence must reach the DRM service client. For PlayReady the initiator
message is known (Smooth Streaming played that way); for Widevine the `sendDrmMessage` message type and
format (EME's generateRequest/update over Luna) are undocumented. Next experiments, if resumed: PlayReady
DASH with the initiator and `mediaTransportType` beside `option`; then the Widevine message format from
`libdrmcontroller`/`libdrmhandler` behaviour. On firmware with `NDL_MediaSetDrmOperation`, LG's plugin
shows the complete flow.
