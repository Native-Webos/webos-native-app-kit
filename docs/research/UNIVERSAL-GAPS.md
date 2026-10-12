# What a general-purpose native app needs that the kit lacks

Survey 2026-10-12. The kit was shaped by media apps (PlxNative, a streaming player, a catalogue
browser). This compares it with native webOS apps of other kinds to find what any app (games,
utilities, dashboards, clients of home services) would still have to build itself.

Sources:
- moonlight-tv: a game-streaming client, but it also has settings forms, text entry, gamepads,
  LAN discovery, i18n and Wake-on-LAN.
- punktfunk client-webos (SDL3, Rust).
- PlxNative, including its LG App Self Checklist record (`docs/lg-self-checklist.md`).
- RetroArch's webOS build (`Makefile.webos`).
- The ScummVM webOS port (scummvm-devel, 2026-08, tested on webOS 6.5.3).
- The SDL-webOS source the kit builds.

`[SRC]` marks a finding read in that source; `[KIT]` marks one checked in the kit.

## Findings that change how every kit app behaves today

1. **The screensaver is always blocked.** SDL disables it at `SDL_VideoInit` unless
   `SDL_HINT_VIDEO_ALLOW_SCREENSAVER` is set (`src/video/SDL_video.c:576` `[SRC]`). SDL-webOS
   answers `com.webos.service.tvpower/power/registerScreenSaverRequest` with `ack: false` while it
   is disabled (`src/core/webos/SDL_webos_init.c` `[SRC]`). The kit never sets the hint `[KIT]`.
   A kit app left on a static menu keeps the panel on indefinitely, a burn-in risk on OLED sets.
   moonlight-tv toggles it around streaming (`app_wakelock_common.c` `[SRC]`).
   **Done:** the shell sets the hint, allows the screensaver by default (`AppConfig::allow_screensaver`)
   and apps hold `Context::keep_screen_on()` (`app/wake_lock.hpp`); Example D holds it while video moves.
   Host-tested; not yet seen on a TV.
2. **Relaunch parameters are dropped.** When a running app is launched again with new
   parameters (a deep link, a launcher shortcut, voice search, a phone casting to it), SDL-webOS
   receives `{"event":"relaunch", ...}` and only raises the window (`lifecycleCallbackVersion2`
   `[SRC]`). Measured in this project: relaunching Example D with new parameters changed nothing.
   None of the three reference apps handles it either `[SRC]`.
   **Done:** the kit's SDL build keeps the message (`cmake/patches/sdl-webos-relaunch-message.patch`,
   `SDL_webOSTakeRelaunchMessage`), the shell delivers `Delegate::on_relaunch(params)`, and Example D
   opens the new request. Host-tested with a scripted message; the TV's message shape is not yet seen.
3. **The UI redraws at 60 fps when nothing changes.** Every perf log shows 300 frames per 5 s on idle
   screens `[KIT]`. That is CPU, GPU and heat for no picture change. PlxNative measured its
   options for not redrawing (`docs/perf-damage-tracking-verdict.md` `[SRC]`).
   **Done:** the GLES2 renderer fingerprints each frame and the shell presents only frames that
   differ (`AppConfig::redraw_on_change`), with a 100 ms probe and a 2 s keepalive as PlxNative uses.
   Host: Example B idle for 7 s presents 7 frames, not ~400. Not yet measured on a TV.

## Gaps by kind of app

### Needed by almost every app

| Gap | Evidence it is needed and possible | What the kit would add |
|---|---|---|
| **Text entry** (search, sign-in, server address). **Done:** `ui::TextField`, shell text input and editing-key routing, Example C search; host-tested, LG's keyboard not yet seen on a TV. | moonlight-tv calls `SDL_StartTextInput` / `SDL_SetTextInputRect` and takes `SDL_TEXTINPUT` into text areas; on the TV this opens LG's on-screen keyboard (`ui/ui_input.c` `[SRC]`). LG's checklist tests keyboard editing, characters and its voice button (rows 15-17). The kit guide says "no text field, IME, or on-screen keyboard" `[KIT]`. | A `TextField` widget (cursor, selection, password mode) wired to SDL text input and LG's keyboard, plus USB/BT keyboard typing. |
| **Localization**. **Done:** `i18n/` (catalogs, CLDR plurals, numbers), the TV's locale from `localeInfo`, Example C in French, German and Japanese; host-tested. | ScummVM's translations work on webOS. moonlight-tv uses LG's `webosi18n` resource bundles. PlxNative reads `localeInfo` (UI and formatting locales, clock) from `com.webos.settingsservice/getSystemSettings` and bundles catalogs `[SRC]`. | String catalogs with plurals, the TV's locale at start, and number and date formatting. |
| **Non-Latin text**. **Done:** per-character fallback chain in the renderer, `AppConfig::fallback_fonts`, and a lazy search of `/usr/share/fonts` on a worker; host-tested. Which CJK faces a Developer Mode app can read on the TV is not yet seen. | PlxNative bundles a CJK fallback font and lays text out in per-script runs; LG checklist row 22 is a CJK search test `[SRC]`. The kit draws Latin only and has no fallback `[KIT]`. | Font fallback chain (bundled or the TV's own fonts), per-run layout. Shaping for Arabic, Hebrew and Indic would be a later step. |
| **Form widgets**. **Done:** `wnk/ui/forms.hpp` (Toggle, Slider, Choice, ValueRow, Tabs, TextView, QrCode on nayuki/QR-Code-generator), Example B's Settings; host-tested, the QR code decoded from a screenshot. | Every settings screen in moonlight-tv, PlxNative and client-webos `[SRC]`. The kit has Button, Card, Label, ImageView, Spinner, ProgressBar, Dialog, CollectionView `[KIT]`. | Toggle, Slider, option picker, checkbox/radio, tabs, list rows with values, scrolling long text, and a QR code (to finish a sign-in or open a link on a phone, which PlxNative does). |
| **Screensaver control** | Finding 1. | Allow the screensaver by default; a wake lock an app holds while playing video or a game. |
| **Deep links / relaunch** | Finding 2. | Patch the SDL-webOS build to forward relaunch parameters as an event; `Delegate::on_relaunch(params)`. |
| **Opening other apps and system toasts**. **Done:** `platform/system.hpp`, `Context::launch_app/open_url/system_toast` on a worker; payloads unit-tested, not yet sent on a TV. | All three reference apps call `com.webos.applicationManager/launch` (Home, browser); PlxNative uses `com.webos.notification/createToast` `[SRC]`. | `platform::launch_app(id, params)`, `open_url`, `system_toast`. |
| **Network status**. **Done:** connectionmanager subscription (host: interfaces), `Context::network()`, `Delegate::on_network`; reply parsing unit-tested, the TV's replies not yet seen. | Abnormal-network handling is LG checklist row 14 `[SRC]`. | Connectivity state and changes (`com.webos.service.connectionmanager/getStatus`, subscribed), so apps can say "offline" instead of timing out. |

### Games, emulators, interactive apps

| Gap | Evidence | What the kit would add |
|---|---|---|
| **App sound** (effects, music, synthesized audio). **Done:** `audio/` (WAV, Ogg via stb_vorbis, mixer, SDL output), `AppConfig::audio`, Example B interface sounds; host-tested through SDL's disk driver. Not yet heard on a TV. | RetroArch: PulseAudio and ALSA drivers; client-webos: SDL audio callback into PulseAudio, measured on TVs `[SRC]`. The kit only outputs audio through the media backends `[KIT]`. | An audio output with a small mixer (sound effects plus one stream), on SDL audio. |
| **Gamepads**. **Done:** `input/gamepad.hpp` (event model, `GamepadNavigator`), SDL GameController with hot-plug and an optional mapping file, `Delegate::on_gamepad`; host-tested with SDL's virtual pad. No pad tried on the TV yet. | moonlight-tv, client-webos (SDL gamepad API, DualSense details), RetroArch (udev and HID, webOS 4 and later) `[SRC]`. The kit maps remote keys only `[KIT]`. | SDL GameController with hot-plug and a mapping database: gamepad buttons as UI actions, and raw gamepad events for games. |
| **Raw keyboard and pointer** for games. **Done for keys:** `Delegate::on_key` before the keymap. Pointer events already reach views. | client-webos maps USB keyboards and the Magic Remote pointer separately `[SRC]`. | Raw key and pointer events alongside the action layer. |
| **Own GL drawing**. **Done:** `Renderer::draw_custom` and `ui::GlView`, Example B triangle; host-tested (pixels and restored state). | RetroArch and ScummVM render their own GLES frames `[SRC]`. The kit's renderer exposes quads and text `[KIT]`. | A view whose `draw` gets the GLES context and its viewport, for games and visualizers. |
| **Larger saved data**. **Done:** `storage::BlobStore` (chunks, generation swap, CRC-32), host-tested on the file store including an interrupted write. DB8 throughput for MB values not yet measured on a TV. | Save games and caches. The kit's store is DB8 with 64 KB values, and the Developer Mode jail has no durable writable directory (`storage/` docs `[KIT]`). | A chunked blob store on DB8 for saves of a few MB, with a size budget. |

### Clients of home or LAN services

| Gap | Evidence | What the kit would add |
|---|---|---|
| **LAN discovery**. **Done:** `net::mdns_browse` (DNS-SD over legacy unicast mDNS), verified on the host against real Cast/AirPlay devices; not yet run from the TV's jail. SSDP not done. | moonlight-tv (libmicrodns), client-webos (`services/discovery.rs`) `[SRC]`. | mDNS and SSDP browsing. |
| **Wake-on-LAN**. **Done:** `net::wake_on_lan`, packet verified on a local socket. | moonlight-tv, client-webos (`services/wol.rs`) `[SRC]`. | Magic-packet sender. |
| **Phone-to-TV** (DIAL) | client-webos `core/dial.rs` `[SRC]`. | Later; depends on finding 2. |

### Accessibility and store readiness

| Gap | Evidence | What the kit would add |
|---|---|---|
| **Audio guidance** (spoken focus) | RetroArch builds with accessibility (TTS) on webOS `[SRC]`; LG's own apps speak the focused item when Audio Guidance is on. | Read the setting; speak focus changes through the TV's TTS service (needs a device check). |
| **LG App Self Checklist** | PlxNative keeps a per-row record and still owes device runs (lifecycle matrix, 4-hour soak, network failure, IR remote, HOME/LIVE keys) `[SRC]`. | A checklist template in the kit and soak and lifecycle scripts built on `tools/launch.sh`. |
| **Vector icons** | PlxNative renders SVG (`gfx/src/svg.rs`) `[SRC]`. | SVG or an icon font for sharp icons at any size. |

## Already covered (no gap)

The kit already handles these, so they are not on the list above:
- Magic Remote pointer, focus and navigation, dialogs and toasts
- HTTP(S), WebSocket, JSON, images, key/value and secret storage
- crash records, lifecycle, the media player, ClearKey, packaging and release

## Suggested order

1. Screensaver default plus wake lock (small; protects OLED panels now).
2. Text entry with LG's keyboard plus `TextField` (needs device checks of LG's keyboard).
3. Redraw only on change (small API change: `Ui::request_redraw`, animation keeps it awake).
4. Relaunch parameters (SDL patch plus `on_relaunch`).
5. Localization plus font fallback.
6. Form widgets plus QR code.
7. App audio plus gamepads (opens the kit to games).
8. Network status, system launch and toast, LAN discovery and Wake-on-LAN.
9. Own GL view, larger saves, accessibility, checklist tooling.
