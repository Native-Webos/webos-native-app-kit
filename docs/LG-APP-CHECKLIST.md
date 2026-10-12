# LG App Self Checklist: what the kit covers and how to show it

LG's App Self Checklist is the set of checks a TV app should pass before it is submitted. This file
maps its areas onto the kit: what the kit already does for an app built on it, and how to produce the
evidence on a TV. Row numbers follow LG's checklist as PlxNative recorded it
(`plx-native/docs/lg-self-checklist.md`); the wording here is the kit's.

Copy the **Evidence** column into your app's own record and fill it in with a device, firmware and
date for every row you claim. A host test is not evidence for a TV row.

## Tools

| Tool | What it does |
|---|---|
| `tools/lifecycle-check.sh APP_ID` | Cold start, relaunch with parameters, background (by launching another app), foreground, close; then `tools/check-lifecycle-log.py` checks the event log (start time, resume time, relaunch delivery, clean shutdown, no crash or ERROR). |
| `tools/soak.sh APP_ID --hours 4` | Leaves the app running, pulls the log every 15 minutes, then `tools/soak-report.py` fits memory growth after warm-up from the shell's once-a-minute heartbeat, counts late frames, restarts and crash records. |
| `tools/logs.sh APP_ID` | Pulls `/tmp/<APP_ID>/events.log`. |
| `{"wnkMetrics":true}` | Frame timing overlay. |
| `{"wnkLocale":"ja-JP"}`, `{"wnkSpeak":true}` | Try a language or Audio Guidance without changing the TV's settings. |
| `{"wnkCrashTest":"segv"}` | Crashes on purpose to prove the crash record. |

## Areas

| Rows | Area | What the kit does | How to show it on a TV | Evidence |
|---|---|---|---|---|
| 1, 2 | Starts and shows its main screen | Shell, first frame logged with its time | `tools/lifecycle-check.sh`: "first frame within 3000 ms" | |
| 3 | Lifecycle: power, Recent list, relaunch | Suspended/Resuming states, `Delegate::on_lifecycle`, relaunch parameters (`on_relaunch`), timed shutdown | `tools/lifecycle-check.sh`; then by hand: power off/on with the app open, AC unplug, close from the Recent list | |
| 6, 22 | Text is correct, other languages | Catalogs (`i18n/`), per-character font fallback, system font search | Launch with `{"wnkLocale":"ja-JP"}` (or set the TV's language) and look for missing-glyph boxes; the log says which system font was found | |
| 7, 27–31, 33 | Focus, Magic Remote pointer, wheel | Focus ring, hover focuses, click is OK, wheel scrolls | By hand with the Magic Remote | |
| 8 | No flickering | Frames are presented only when they change | Watch transitions; `{"wnkMetrics":true}` for late frames | |
| 13 | No lock-up over a long run | Heartbeat, crash record, watchdog on shutdown | `tools/soak.sh APP_ID --hours 4` (with video, `--params` that start playback) | |
| 14 | Network failure | `Context::network()`, `Delegate::on_network`, HTTP errors as sentences (Example C) | Unplug the cable / turn Wi-Fi off during use: the app says it is offline and recovers | |
| 15–17 | Keyboard: cursor, characters, linked buttons | `TextField` with LG's on-screen keyboard (SDL text input), Backspace/arrows/Enter routed to the field | Type in a field with LG's keyboard: move the cursor, erase, enter `\` and capitals, close the keyboard with Back | |
| 21 | Sign out | `auth::Session::sign_out` | By hand in the app | |
| 26, 36–40 | Remote keys: IR remote, HOME, LIVE, BACK, EXIT, unsupported keys | Key map with provenance, Back at the root asks for a second Back, Exit quits | Press every key; unmapped keys must do nothing (`unmapped_keys_have_no_effect` covers the host) | |
| 45 | Playback keys | Play, pause, stop, rewind, fast-forward mapped | By hand in Example D or the app's player | |
| 46–51 | Video: replay, subtitles, resume, resolutions and codecs | `media/` (see the media section of the agent guide) | The app's own playback matrix | |
| — | OLED: screensaver while idle | The screensaver is allowed unless the app holds `keep_screen_on` | Leave a still screen: the TV's screensaver must start; during playback it must not | |
| — | Accessibility: Audio Guidance | Focus is spoken through the TV's TTS when the setting is on | Turn Audio Guidance on; move focus | |
| — | Packaging and licences | `check-package.py`, `fw-check.py`, `licenses/` in every package | `tools/release.sh` output | |

## Not covered by the kit

- Store artwork, descriptions, ratings and the submission itself.
- Content rules (what the app shows), and the app's own privacy statement.
- Rows that depend on the app's service (server reachability, account flows beyond sign-in and out).
