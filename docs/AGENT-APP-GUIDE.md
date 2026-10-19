# Agent guide: build a native webOS app with this kit

This document is the briefing for a coding agent that must add a **native** LG webOS application
(C++17, SDL, OpenGL ES 2) on top of this repository. Read it before writing code. Then read the
headers it names. Copy structure from the examples; do not invent a second app framework.

The feature list for a particular app arrives separately (a user request, a ticket, a spec). This
guide says which kit pieces implement each kind of feature, which constraints are fixed, and how to
prove the app builds. It does not replace `docs/MILESTONES.md` for what has actually been run on a
television.

Status of the kit lives in `docs/MILESTONES.md` and `docs/compatibility/records.json`. The opening
status paragraph of `README.md` is older than the code: ignore it.

## What you are building

A webOS **native** app is one executable, packaged as an `.ipk`, launched by LG's SAM. It is not a
web app, Enact app, or HTML/CSS UI. The kit already owns the process, the window, the GLES2 context,
the main loop, input, focus, layout, and shutdown. A new app supplies:

1. A `wnk::app::Delegate` (screens, data, playback).
2. A CMake target and `wnk_add_webos_package()`.
3. Icons, fonts, and any bundled files.

Reference apps already in the tree:

| App | Path | What to copy |
|---|---|---|
| Example B, TV UI | `applications/examples/tvui/` | **Default template.** Shell + widget tree + navigation. |
| Example C, shows | `applications/examples/shows/` | REST/JSON on the pool, cancellation on Back, error + Try again, paging, saved copy and favourites in DB8. |
| Example D, player | `applications/examples/player/` | Video, DASH/HLS, audio and subtitle track menu, background pause. |
| Example A, hello | `applications/examples/hello/` | Diagnostics only. It keeps its own SDL loop on purpose. **Do not copy that loop.** |

Put a product app in `applications/<name>/`, not under `examples/`, and add
`add_subdirectory(applications/<name>)` next to the examples in the root `CMakeLists.txt` (inside
the `WNK_BUILD_EXAMPLES` block, or a sibling option you add the same way).

Licence of this repository is **GPL-3.0-or-later**. New app code stays under that licence
(`SPDX-License-Identifier: GPL-3.0-or-later` on every file). Code copied from elsewhere keeps its
header and is listed in `THIRD-PARTY-NOTICES.md`. `webosbrew/samples` has no licence: read only,
never copy.

## Non-negotiable rules

These are enforced by the architecture, the packaging checks, and the way the TV works.

- **One main thread** owns SDL, GL, the UI, and focus. Blocking work (HTTP, DB8, Luna, decode,
  file I/O) goes to `async::WorkerPool`. Workers return with `MainDispatcher::post` or
  `async::run_then_post`. A worker never waits on the main thread.
- Callbacks that touch a screen or service capture `async::Lifetime::Weak` and check `alive()` on
  the main thread before use.
- Shutdown order is fixed inside `wnk::app::run`: `on_stop` → close the dispatcher → shut down the
  pool with a deadline → destroy the UI and GL. Do not tear SDL or GL down yourself.
- Widgets and app code act on `wnk::input::Action`. They do not branch on scancodes. New key codes
  go into a keymap table with a `Provenance`. Only a code captured on a TV may be marked `Measured`.
- All drawing goes through `wnk::render::Renderer` in **logical 1920×1080** units. GL state stays
  inside `Gles2Renderer`. `ui/` never includes SDL or GL.
- Widgets register focus with `set_focusable` and `set_focus_scope`. They never call
  `FocusManager` directly.
- A handler may close its own screen. The `Ui` retires removed views and destroys them at the next
  entry point (`handle` / `update` / `draw` / `sync`).
- Firmware-variable libraries (`libcurl.so.4`, NDL, Starfish `libplayerAPIs`, `libhelpers`) are
  **dlopen**ed. Never add them to `DT_NEEDED`. Never bundle `libstdc++` (the webOS link line
  already uses `-static-libstdc++`) or any LG library.
- The Developer Mode TV is **not rooted**. No root ssh, no scp deploy. Device runs are
  build → package → `tools/install.sh` → `tools/launch.sh`.
- Do not claim that something works on a TV unless `docs/compatibility/records.json` (or a run you
  just performed) names the model, webOS release, and date. Tag claims the way
  `docs/research/README.md` does. "The symbol exists in the firmware inventory" is not "it works".
- Do not log tokens, passwords, or `Authorization` headers. The logger redacts some of these; that
  is a backstop, not permission to format secrets into messages.

## Repository map

| Path | Role for an app |
|---|---|
| `app/` | `wnk::app::run`, `Delegate`, `Context`, lifecycle. Link `wnk_app`. |
| `ui/` | View tree, layout, widgets, navigation, dialogs. Headers under `ui/include/wnk/ui/`. |
| `i18n/` | Locale, catalogs with plural forms, number formatting. Reach it through `ctx.i18n()`. |
| `audio/` | The app's own sounds: WAV/Ogg decoding, a mixer, the SDL audio device. Reach it through `ctx.audio()`. |
| `input/` | `Action`, keymaps. App code includes `wnk/input/input.hpp` only. |
| `focus/` | Used by `Ui`. Apps do not include it. |
| `render/` | `Renderer` interface. Apps draw through widgets, or `ctx.renderer()` for a custom `View::draw`. |
| `async/` | Pool, dispatcher, cancellation, `Lifetime`, `run_then_post`. |
| `net/` | HTTP(S), streaming body, WebSocket. |
| `image/` | Fetch, decode (PNG, JPEG, GIF, BMP, WebP, SVG), caches. `ImageView` uses this through the shell. |
| `storage/` | Key/value store (DB8 on the TV) and `SecretStore`. |
| `services/auth/` | Optional: sign-in state, stored credentials, authorized requests with token refresh. Link `wnk_auth`. |
| `platform/` | Launch params, paths, synchronous Luna. |
| `diagnostics/` | Capabilities, device info (no serials or MACs), crash record, frame metrics. |
| `media/` | Demux, playback controller, NDL, Starfish, DASH/HLS, subtitles. Separate link targets. |
| `logging/` | `WNK_LOGI(category, fmt, ...)`. The shell installs the logger. |
| `cmake/WebOSPackage.cmake` | `wnk_add_webos_package()`. |
| `third_party/nlohmann/json.hpp` | JSON. Add a `SYSTEM` include of `third_party/` on the app target. |
| `assets/fonts/` | Atkinson Hyperlegible. Bundle it; the shell loads it by relative path. |
| `tools/` | Host tests, ARM build, package check, install, launch, logs. |
| `docs/research/` | Evidence and decisions. Read before inventing platform behaviour. |
| `docs/MILESTONES.md` | What is implemented and what was measured on hardware. |

`wnk_app` already links UI, images, storage, async, net, input, GLES2 renderer, platform, and SDL.
A UI-only app links `wnk_app` and nothing else. An app with accounts also links `wnk_auth`.

## Recipe: add the app

### 1. Files

```
applications/<name>/
  main.cpp
  CMakeLists.txt
  icon.png          # 80×80
  largeIcon.png     # 130×130
  expected-needed.txt
```

Copy `applications/examples/tvui/CMakeLists.txt` and `expected-needed.txt` for an app with no
media. Copy `applications/examples/player/CMakeLists.txt` and its `expected-needed.txt` when the
app links FFmpeg.

`appinfo.json` is generated. Do not write one by hand. Id, version, and title live only in
`wnk_add_webos_package()`.

An app kept in its own repository, with the kit as a submodule or a separate checkout, needs no
`add_subdirectory` line in the kit. List its absolute directory in `WNK_EXTRA_APP_DIRS`
(`;`-separated; the environment variable is read on the first configure) and the kit's own
`tools/test.sh`, `tools/build-webos.sh` and packaging build it, with its binary directory at
`build/<preset>/extra-apps/<basename>`:

```sh
WNK_EXTRA_APP_DIRS=$HOME/src/myapp/app tools/test.sh
```

The value is cached: after changing it, set `-DWNK_EXTRA_APP_DIRS=...` or remove
`build/<preset>/CMakeCache.txt`.

### 2. CMake

UI app:

```cmake
# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(wnk-<name> main.cpp)
target_link_libraries(wnk-<name> PRIVATE wnk_app)
target_include_directories(wnk-<name> SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")

set(WNK_FONT_DIR "${PROJECT_SOURCE_DIR}/assets/fonts")
add_custom_command(TARGET wnk-<name> POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${WNK_FONT_DIR}" "$<TARGET_FILE_DIR:wnk-<name>>/fonts"
        VERBATIM)

wnk_add_webos_package(
        TARGET wnk-<name>
        APPID org.example.<name>
        TITLE "Title shown in the launcher"
        VERSION 0.1.0
        VENDOR "vendor"
        ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.png"
        LARGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/largeIcon.png"
        ICON_COLOR "#1f2937"
        EXPECTED_NEEDED "${CMAKE_CURRENT_SOURCE_DIR}/expected-needed.txt"
        EXTRA_DIRS "${WNK_FONT_DIR}")
```

Rules for the package arguments:

- `APPID` is lowercase reverse-DNS. It is the package id and the runtime directory name.
- `VERSION` is three integers (`x.y.z`). LG rejects anything else.
- `ICON` is 80×80 PNG, `LARGE_ICON` is 130×130 PNG.
- `EXTRA_DIRS` copies each directory next to the executable under its own basename (`fonts/`,
  `posters/`). The shell looks for `fonts/AtkinsonHyperlegible-Regular.ttf` relative to the app
  directory. Host `POST_BUILD` copy is what makes `tools/test.sh` and local runs find the same files.
- `REQUIRED_PERMISSIONS` is the ACG list written into `appinfo.json`. Add `database.operation` when
  `AppConfig::storage` is true. Add `securitykey.operation` as well when using `SecretStore`.
- `BUNDLED_LIBS ${WNK_FFMPEG_RUNTIME_LIBS}` when linking `wnk_media_ffmpeg`.
- `PRIVATE_RUNTIMES <dir>...` packages a self-contained program tree that the app starts as a
  separate process (for example a language runtime with its own loader and C library), copied as
  `<basename>/`. `check-package.py --private-runtime <basename>` then checks it as a unit instead of
  with the executable's rules: ARM or AArch64 machine, every `DT_NEEDED` resolved inside the tree, no
  LG firmware library in it, and no ELF outside it loading from it. Pass the same option to
  `tools/verify.sh` and `tools/install.sh`. Never load such a tree into the app's own process.
- If the executable's `DT_NEEDED` changes, update `expected-needed.txt` in the same change. A new
  line is a review: the SONAME must exist on every targeted firmware, or the library must be
  bundled under `lib/`. Start from the tvui list; the player list is the media superset.

Player link line (in addition to `wnk_app`):

```cmake
target_link_libraries(wnk-<name> PRIVATE
        wnk_media_ffmpeg wnk_media_ndl wnk_media_starfish wnk_media_ui)
```

`WNK_FFMPEG` defaults to `ON`. If configure skipped FFmpeg, do not pretend playback works.

### 3. `main.cpp` skeleton

Match `applications/examples/tvui/main.cpp`. Namespace the app's types; keep `main` outside.

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/app/app.hpp"
#include "wnk/log.hpp"
#include "wnk/ui/collection.hpp"
#include "wnk/ui/navigation.hpp"
#include "wnk/ui/widgets.hpp"

namespace {

constexpr const char *kCat = "myapp";

class HomeScreen : public wnk::ui::Screen {
 public:
    HomeScreen() : Screen("home") {
        name = "home";
        auto &col = add<wnk::ui::Column>(24);
        col.set_padding(wnk::ui::Insets::all(80));
        col.add<wnk::ui::Label>("Title", wnk::ui::TextRole::Title);
    }
};

class App : public wnk::app::Delegate {
 public:
    void on_start(wnk::app::Context &ctx) override {
        auto &ui = ctx.ui();
        nav_ = &ui.root().add<wnk::ui::Navigator>();
        nav_->push(ui, std::make_unique<HomeScreen>());
    }

 private:
    wnk::ui::Navigator *nav_ = nullptr;
};

}  // namespace

int main(int argc, char **argv) {
    wnk::app::AppConfig config;
    config.fallback_app_id = "org.example.myapp";  // used when $APPID is unset (host)
    config.title = "myapp";
    config.script_env_prefix = "WNK_MYAPP";
    return wnk::app::run(argc, argv, config, [] { return std::make_unique<App>(); });
}
```

`Context` is valid from `on_start` until `on_stop` returns.

| `Context` member | When it is non-null |
|---|---|
| `ui()`, `renderer()`, `pool()`, `dispatcher()` | Always. |
| `images()` | `AppConfig::images` (default true). |
| `http()` | `AppConfig::http` true **and** `libcurl.so.4` loaded. |
| `store()` | `AppConfig::storage` true **and** the store opened. |
| `launch()` | Always. `app_id`, `params_json` (`argv[1]` when it is a JSON object), `app_dir`. |
| `runtime_dir()` | Logs and other volatile files. Empty if it could not be created. On webOS this is `/tmp/<app id>` (tmpfs). |
| `capabilities()`, `device()` | Always. `device()` never contains a serial, nduid, or MAC. |
| `quit(reason)` | Ends the loop; the shell then runs shutdown. |
| `keep_screen_on(reason)` | Always. Returns a `WakeLock`; the TV's screensaver cannot start while any lock is held. |

`AppConfig` fields an app actually sets:

| Field | Default | Set it when |
|---|---|---|
| `fallback_app_id` | `"wnk app"` | Always, to the same id as `APPID`. |
| `title` | `"wnk app"` | Window title. |
| `http` | false | Any `http://` or `https://` URL (API, images, streams). |
| `images` | true | Posters and thumbnails. Set false for a player with no images. |
| `storage` | false | Settings or session that must survive relaunch. Needs `database.operation`. |
| `audio` | false | The app plays its own sounds (interface clicks, game effects, music). Not needed for video: the player has its own audio path. |
| `double_back_exit` | true | Root Back shows "Press Back again to exit", second Back quits. |
| `redraw_on_change` | true | Leave it. A frame is presented only when it differs from the one on screen (on the TV the compositor's work is most of a frame's cost); input and worker results wake the loop at once, anything that changes with time alone shows within `idle_probe_ms` (100 ms). Animations still run at the display rate. |
| `allow_screensaver` | true | Leave it. The screensaver may start on idle screens (OLED burn-in); hold `ctx.keep_screen_on()` while video plays or a game runs. false blocks it for the whole run. |
| `metrics_toggle` | `Action::Unknown` | e.g. `Action::Blue` to show the frame-time overlay. |
| `script_env_prefix` | `"WNK"` | Unique per app so host tests do not collide. |
| `worker_threads` | 2 | Raise only for a measured reason. |
| `image_memory_bytes` / `texture_bytes` | 48 MiB / 64 MiB | Raise only if posters are larger than the TV budget you measured. |
| `font_regular` / `font_bold` | Atkinson under `fonts/` | Leave these unless you bundle another OFL font and record it in `THIRD-PARTY-NOTICES.md`. |

Launch parameters: SAM passes a JSON object as `argv[1]`. Parse with nlohmann:

```cpp
#include <nlohmann/json.hpp>
const auto params = nlohmann::json::parse(
    ctx.launch().params_json.empty() ? "{}" : ctx.launch().params_json, nullptr, false);
```

Reject a non-object. Do not trust strings as paths outside `app_dir` and URLs you validate.

### 4. Delegate hooks

| Hook | Use |
|---|---|
| `on_start` | Build the tree, open the store, kick off the first fetch. The first frame is drawn after it returns, so do not block. |
| `on_action` | Actions the `Ui` did not consume. Return true to swallow the shell default (Exit quits; Back at the root asks for a second Back). |
| `on_lifecycle` | `Suspended`: pause media and timers. `Resuming` then `Running`: resume. While suspended the shell draws nothing and drops input. |
| `on_relaunch` | The app was launched again while running (deep link, launcher shortcut, voice search, a second `ares-launch` with parameters). `params_json` is the new parameters object; `ctx.launch()` already holds it. Open what it asks for, or ignore it when it asks for nothing. See Example D. |
| `on_gamepad` | Every pad event (added/removed, buttons, sticks, triggers) before the UI. Return true to take it (games). Otherwise D-pad/left stick move focus, A is OK, B is Back, Start is Play/Pause. |
| `on_key` | Every raw key event before text input and the keymap. Return true to take it (a game reading WASD or key-up timing). |
| `on_network` | The network went up or down (`ctx.network()`). Show "offline" instead of waiting for timeouts; refresh when it returns. |
| `on_frame` | Once per drawn frame, after `Ui::update`. Drive a progress label from `PlaybackController::snapshot()`. A still screen is drawn only every `idle_probe_ms` (100 ms), so do not count frames as time: use the `dt` argument or `ui.now_ms()`. |
| `on_stop` | Save state. The pool and dispatcher still run; the UI still exists. Keep it short; a watchdog (default 3 s) ends the process if teardown overruns. |

## Map a feature request onto the kit

Implement a feature with the row below. If the request needs something in
[What the kit does not provide](#what-the-kit-does-not-provide), say so in the result instead of
stubbing a lookalike.

| Requested feature | Build it with |
|---|---|
| Full-screen pages, Back stack, focus restored on Back | `Navigator` + `Screen` on `ui.root()`. See Example B. |
| Sidebar that remembers its row | `set_focus_scope(false, true)` on the sidebar. `contain=false` so Right can leave it. |
| Horizontal poster row ("rail") | `CollectionView(Axis::Horizontal, 1, cell, spacing)` + `Card`. |
| Long vertical list or poster grid | `CollectionView(Axis::Vertical, columns, cell, spacing)`. Cells exist only in the viewport. |
| Details page taller than the screen | `ScrollView` with one `Column` child. It follows focus. |
| Poster / thumbnail | `ImageView` or `Card`. URL is `https://…` or `file://` + `ctx.launch().app_dir`. PNG, JPEG, WebP, GIF (first frame), BMP and SVG, recognised by content. |
| Icons that stay sharp at any size | SVG in an `ImageView`: drawn at the view's size (nanosvg: shapes, paths, gradients, transforms; no text, filters or CSS). Animated GIF/WebP show their first frame. |
| Button, disabled control | `Button`, `set_enabled(false)`. |
| Deep link, launcher shortcut, "open this" from another app | Read `ctx.launch().params_json` in `on_start` and the new object in `on_relaunch`. Log parameter names, not values. |
| Settings page | `wnk/ui/forms.hpp` rows in a `Column`: `Toggle` (switch or `Style::Check`), `Slider` (Left/Right), `Choice` (Left/Right, OK opens the list), `ValueRow` (label + value, OK opens something), `Tabs` over pages (`set_visible`), `TextView` for long text. See Example B's Settings. |
| "Continue on your phone", pairing, a link to open elsewhere | `QrCode(url)`; keep it at least 300 units square. |
| Search box, server address, user name, password | `TextField` (`set_password`, `set_max_length`, `set_on_change`, `set_on_submit`). OK or a click edits it: the shell opens LG's on-screen keyboard; Back ends editing. See Example C's Browse search. |
| Modal confirm | `ui.show_dialog`. Focus is trapped; Back dismisses when `back_dismisses` is true; focus returns to the opener. |
| Transient message | `ui.toast(text)`. |
| Gamepad support | Nothing for UI navigation (`AppConfig::gamepads` is on). A game reads `Delegate::on_gamepad` (buttons by position: A is the bottom face button) and returns true. Ship `gamecontrollerdb.txt` in the app directory for pads SDL does not know. |
| Open a web page, another app, the Home screen | `ctx.open_url(url)`, `ctx.launch_app(id, params_json)`, `ctx.launch_app("com.webos.app.home")`. Each runs on a worker; pass `done` to learn whether the TV accepted. |
| A notice that outlives the app | `ctx.system_toast(text)` (the TV's own notification card). `ui.toast` for anything inside the app. |
| Offline state | `ctx.network()` and `Delegate::on_network`. |
| Find a server, a Cast device, a PC on the LAN | `net::mdns_browse("_myservice._tcp", 1500ms, token)` on a worker (`wnk/net/lan.hpp`); results have name, host, IPv4, port and TXT. |
| Wake a PC before connecting to it | `net::wake_on_lan("aa:bb:cc:dd:ee:ff")` on a worker. |
| A game world, a visualiser, a map drawn with OpenGL | `GlView` with a callback that issues GLES2 calls (viewport and scissor are set; the kit restores its own state). Animated by default; `set_animated(false)` + `invalidate_content()` for still pictures. See Example B's Widgets triangle. |
| Interface sounds, game effects, music | `AppConfig::audio = true`; `audio::load_sound(app_dir + "/sounds/x.ogg")` once, then `ctx.audio()->play(sound, {gain, pan, loop})` from any thread; `play_stream(fn)` for generated or streamed audio. Paused in the background. See Example B (Settings, Widgets). |
| Keep the screen on during video, a slideshow or a game | Hold `ctx.keep_screen_on("video playing")` while it moves; `release()` on pause, end or error. See Example D. |
| Busy and determinate progress | `Spinner`, `ProgressBar` (`set_value` / `set_indeterminate`). |
| Section that changes with the D-pad but not with pointer hover | Example B `NavItem`: select on focus only when `ui.focus_source() != FocusSource::Pointer`; pointer selects on click. |
| Settings that survive relaunch and reboot | `AppConfig::storage = true`, `database.operation`, `ctx.store()` (DB8 on the TV). |
| Audio Guidance (spoken focus for blind and low-vision viewers) | Nothing: the shell follows the TV's setting and speaks each focused view's `accessible_text()`. Set `accessible_label` on views whose own text is not enough (an icon-only button, a poster without a title). `{"wnkSpeak":true}` turns it on for testing. |
| Save games, a cached catalogue, anything over 64 KiB | `storage::BlobStore(*ctx.store())` (`wnk/storage/blob.hpp`) on a worker: chunked, a write replaces the old value whole or not at all, 8 MiB per value by default. |
| Access token / password | `storage::SecretStore` on that store. Check `protection()` before saving if a plain envelope is unacceptable. |
| Accounts, sign-in, sign-out, token refresh | `auth::Session` + `auth::CredentialStore` (`wnk_auth`), with an `Authenticator` for the provider. See "Sign-in and sessions". |
| Device id for an API | `auth::installation_id(*ctx.store())`: random, per install. Never a serial or MAC. |
| REST/JSON API | `AppConfig::http = true`, `net::HttpClient` on `ctx.http()`, `ctx.pool()`, `ctx.dispatcher()`. See Example C. |
| Live updates | `net::WebSocketClient` (`ws://` / `wss://`). The shell does not create one; the app does. |
| File or HTTP video, pause, seek | `FfmpegSource` + `PlaybackController` + `AutoBackend` (Starfish preferred, NDL fallback). Example D. |
| DASH (`.mpd`) or HLS (`.m3u8`), including live | `AdaptiveSource` when `AdaptiveSource::handles(uri)`. |
| Subtitles in the picture | `SubtitleView` (`wnk_media_ui`) fed by `controller->subtitles_now()`. |
| Audio language / track menu | `snapshot().info.tracks` lists them; `set_audio_track(index)` while playing or paused; `snapshot().audio_track` is the one playing. Example D's `open_tracks`. |
| Video visible behind the UI | `ui.set_clear_color(wnk::render::kTransparent)` while the player screen is showing. Restore with `std::nullopt` on hide. |
| Another app covering this one | `on_lifecycle`: `pause_and_release()` on `Suspended`, resume when `Resuming` returns to `Running`. |
| Device model, webOS release, GLES limits | `ctx.device()`, `ctx.capabilities()`. |
| HEVC / UHD / Dolby Vision / Atmos | `media::DeviceMediaCaps::detect()` once at startup (blocking Luna; startup is acceptable). Unknown stays unknown. |
| Soundbar / AVR on ARC or eARC | `media::AudioOutputCaps::detect()` (getARCState): the link and the formats the device takes. It does not widen what an app can send: TrueHD never leaves the TV (`docs/research/AUDIO-PASSTHROUGH.md`). |
| VP9, AV1, HDR10 in the container | Nothing to do: `StarfishBackend` offers VP9/AV1 where the TV reports a decoder and declares container HDR10/HLG metadata with `setHdrInfo`. |
| On-screen frame timing | `config.metrics_toggle = Action::Blue`, or launch param `{"wnkMetrics": true}`. |
| Call another Luna service | `platform::luna_call_sync` from a worker, never from `draw`. |
| Host screenshot / scripted keys | `<PREFIX>_KEYS` and `<PREFIX>_SHOT`. Names are in the comment at the top of `app/src/app.cpp` (`Script`). |

## UI construction

Canvas is 1920×1080 logical units. The renderer letterboxes that onto the drawable. On the measured
TV (OLED65C1PTB, webOS 6.5.3) the drawable is 1920×1080 on a 3840×2160 panel. Design for a couch:
theme defaults are body 32, small 24, title 52, radius 16, focus ring gold.

Sizes are `Dim::automatic()`, `Dim::fixed(px)`, or `Dim::fill(weight)`. Alignment is
`Align::{Start, Center, End, Stretch}`. `Row` / `Column` (`Box`) share leftover space among `Fill`
children. `justify` applies only when nothing fills.

```cpp
auto &page = add<Column>(28);
page.set_padding({80, 60, 80, 60});
page.add<Label>("Continue", TextRole::Body).set_bold(true);
auto &rail = page.add<CollectionView>(Axis::Horizontal, 1, Size{320, 236}, 28);
rail.set_width(Dim::fill());
rail.set_items(count, [&](Ui &, size_t i) {
    auto card = std::make_unique<Card>(titles[i], urls[i]);
    card->image_aspect = 9.f / 16.f;  // height / width of the image area
    return card;
});
rail.on_activate = [&](Ui &ui, size_t i) { nav.push(ui, std::make_unique<Details>(i)); };
rail.on_focus = [&](Ui &, size_t i) { /* remember i if you must persist it */ };
```

`CollectionView` handles arrows by index, so a repeat cannot skip a cell that is not created yet.
At the edge the arrow is unhandled and geometric focus moves (Left from column 0 into a sidebar).
`focus_index` restores a saved index. Replacing items with `set_items` keeps the focused index,
clamped.

`Label::set_max_lines(n)` wraps on spaces, breaks an over-long word at a code-point boundary, and
ellipsizes. Text is a single direction, no HarfBuzz. Latin, Cyrillic, and Greek are the scripts
the font path is built for.

`ImageView` loads through `Ui`'s `ImageSource` once it has a frame, at about the frame size. It
shows a pulsing placeholder, then a muted panel on failure. Remote URLs need `AppConfig::http`.
Bundled files use `file://` + `ctx.launch().app_dir + "/posters/x.jpg"`. PNG, JPEG, WebP, GIF (first frame), BMP or SVG.
Each image is its own GL texture (a grid of posters costs one draw call per tile).

`Card` is focusable. Its focus ring surrounds the image, not the title (`focus_bounds`).

`Dialog::add_button` runs the handler and then dismisses. Extra content goes in `dialog.body()`
above the buttons.

`Navigator::push` focuses inside the new screen. `pop` restores the exact node that was focused on
the screen below, including per-rail memory. `Screen::on_back` may consume Back (close an inline
panel) before the navigator pops. Returning false lets the pop happen.

Custom widget: subclass `View`, override `measure_content`, `layout_content`, `draw`, and
`on_action`. Call `set_focusable(true)` if OK should land on it. Return true from `on_action` only
for the actions you consume. Unhandled arrows move focus geometrically.

```cpp
bool on_action(Ui &ui, const input::ActionEvent &e) override {
    if (e.action == input::Action::Confirm && e.phase == input::Phase::Press) {
        ui.toast("ok");
        return true;
    }
    return false;
}
```

Ignore `Phase::Repeat` unless the control is a seek or a slider. `Phase::Release` is for
held-key behaviour.

Pointer (Magic Remote) is already wired by the shell: hover focuses, left click is Confirm on the
hit view, wheel goes to the vertical scroller under the pointer (a horizontal rail passes it on),
an arrow key returns control to the D-pad. Hover does not auto-scroll a list. Do not add a second
pointer path.

Theme colours are `ui.theme()`. Override `AppConfig::theme` before `run()` if the app needs a
different palette. `Ui::draw` clears to `theme().background` unless `set_clear_color` is set.
`render::kTransparent` (`Color{0,0,0,0}`) punches through to the hardware video plane.

Animation helpers: `Smoothed` (critically damped spring) and `Tween` in `wnk/ui/anim.hpp`. Advance
them from `View::update`. The focus ring is already animated by `Ui`.

### Text in other languages

Write UI strings in English through the catalog: `ctx.i18n().tr("Search by name")`,
`tr("Hello {name}", {{"name", n}})`, and for counts `trn("{n} show", "{n} shows", count)` (`{n}` is
formatted for the locale). The English text is the key, so English needs no file. Translations go
in `<app>/i18n/<tag>.json` (`fr.json`, `zh-Hant.json`, `pt-BR.json`); stage the directory with
`EXTRA_DIRS` and next to the host binary, as Example C does:

```json
{ "Search by name": "Rechercher par nom",
  "{n} show": { "one": "{n} série", "other": "{n} séries" } }
```

Plural keys are CLDR categories (`zero one two few many other`). The locale is the TV's UI language
(`com.webos.settingsservice` `localeInfo`), `LANG` on the host, or `{"wnkLocale":"ja-JP"}` in the
launch parameters to try a language without changing the TV's. `ctx.formats_locale()` and
`i18n::format_number` format numbers. The shell's own text ("Press Back again to exit") goes through
the same catalog.

Fonts: the bundled Atkinson Hyperlegible covers Latin. Any character it lacks comes, per character,
from `AppConfig::fallback_fonts` (files you bundle), else from a face the shell finds under
`/usr/share/fonts` the first time the character is drawn, preferring the locale's forms of Han
characters (JP, KR, SC, TC). The search runs on a worker; the text appears on the next frame.

## Data loading, HTTP, storage

### Threading pattern

Hold a `Lifetime` on the screen or delegate. Pass `weak()` into `run_then_post` or `HttpClient::send`.

```cpp
class HomeScreen : public Screen {
    async::Lifetime life_;
};

// from on_start, on the main thread:
net::HttpClient client(ctx.http(), ctx.pool(), ctx.dispatcher());
net::HttpRequest req;
req.url = "https://example.test/catalogue.json";
client.send(req, {}, life_.weak(), [this](net::HttpResponse res) {
    if (!res.ok()) {
        WNK_LOGW(kCat, "catalogue %s", res.detail.c_str());
        return;
    }
    // parse and rebuild the collection here; this callback is on the main thread
});
```

If `ctx.http()` is null, log `Unsupported` and show an error state. Do not fall back to a
hand-rolled socket.

`HttpClient` retries idempotent requests (GET/HEAD/OPTIONS/PUT/DELETE, plus HTTP 502/503/504 and
connect/timeout). TLS verification is always on. Only `http` and `https`. Bodies over
`max_response_bytes` (default 8 MiB) fail with `TooLarge`. Non-idempotent methods are not retried.

`HttpTransport::perform` blocks. Call it on a worker, or use `HttpClient`. For a large media body
use `HttpStream` / `FfmpegSource`, not `perform`.

### Key/value store

`ctx.store()` is `Db8Store` on webOS and a file store on the host. Same contract
(`tests/unit/test_storage.cpp`):

- Keys: 1..64 of `[a-z0-9._-]`, starting with a letter or digit.
- Values: opaque bytes, default max 64 KiB. Serialise JSON yourself.
- `put` / `remove` take an expected revision. `kAnyRevision` skips the check. `kMustNotExist`
  creates only. `Status::Conflict` means retry from a fresh `get`.

Calls block (a Luna round trip on the TV). Use them from `on_start` only for a handful of keys, or
from a worker via `run_then_post`. Never from `draw` or `on_frame`.

There is **no durable app-writable filesystem** in the Developer Mode jail (measured on
OLED65C1PTB, webOS 6.5.3). `/tmp/<app id>` is tmpfs. Do not "save settings to a file" on webOS.
DB8 survived relaunch, reinstall, and reboot on that TV. The store is not secret: anything that
can read the app's DB8 kind can read values.

### Secrets

```cpp
#include "wnk/storage/secret.hpp"

storage::SecretStore secrets(*ctx.store(), storage::luna_keymanager3(),
                             ctx.launch().app_id + ".secrets.v1");
if (secrets.protection() != storage::Protection::Keymanager3) {
    // webOS 6.5.3 has no keymanager3; the envelope would be plain-v1.
}
secrets.put("token", token);  // blocks; call from a worker or startup
```

Permissions: `database.operation` and `securitykey.operation`. A sealed value is never overwritten
with a plain envelope. Reading a sealed value while keymanager3 is down returns `Locked`.

### Sign-in and sessions

`services/auth` keeps who is signed in and sends requests with their credentials. The app owns the
login flow (pick a user, a code shown on screen, a paired phone) and the provider-specific parts;
the session owns storage, refresh, retry and sign-out.

```cpp
#include "wnk/auth/session.hpp"

class ApiAuth final : public auth::Authenticator {
 public:
    void authorize(net::HttpRequest *req, const auth::Credentials &c) const override {
        req->headers.emplace_back("Authorization", "Bearer " + c.access_token);
    }
    // Optional: refresh() (worker thread) when the provider has refresh tokens, revoke() for sign-out.
};

// on_start, with AppConfig::storage = true and http = true:
secrets_ = std::make_unique<storage::SecretStore>(*ctx.store(), storage::luna_keymanager3(),
                                                  ctx.launch().app_id + ".secrets.v1");
http_ = std::make_unique<net::HttpClient>(ctx.http(), ctx.pool(), ctx.dispatcher());
session_ = std::make_unique<auth::Session>(std::make_shared<auth::CredentialStore>(*secrets_, *ctx.store()),
                                           std::make_shared<ApiAuth>(), ctx.pool(), ctx.dispatcher());
session_->set_listener([this](auth::SessionState, auth::SessionState to, const std::string &why) {
    if (to == auth::SessionState::SignedOut) show_login(why);
});
session_->restore([this](auth::SessionState s) { s == auth::SessionState::SignedIn ? show_home() : show_login({}); });

// After the app's own login request succeeded:
session_->sign_in(creds, /*remember=*/true);
// Every request that needs the user:
session_->send(*http_, req, cancel_.token(), life_.weak(), [this](net::HttpResponse r) { ... });
```

- `send` adds the credentials, refreshes a token that expires within a minute first, and on a 401
  refreshes once and retries. Concurrent 401s share that one refresh.
- A refresh the provider rejects signs out (state `SignedOut` with the reason) and removes the stored
  copy. A refresh that cannot reach the provider keeps the credentials; the requests get their 401.
- Without a refresh token a 401 signs out (`SessionOptions::sign_out_on_401`, on by default).
- `sign_out` forgets the credentials at once, then revokes and removes them on a worker.
- `CredentialStore::accounts()` lists saved accounts without unsealing them (an account picker).
- Destroy the session before the `SecretStore` and the key/value store it uses.
- Log with `auth::describe(creds, now)`; never format a token or an `Authorization` header.

### WebSocket

`WebSocketClient` runs one I/O thread. `on_open` / `on_message` / `on_close` / `on_error` run on
the main thread. `send_text` is safe from any thread. Destroying the client closes and joins.
Give it `ctx.dispatcher()`. Set `options.reconnect.enabled` for a session that should retry.
Extensions (permessage-deflate) are not implemented.

### Images without a widget

Prefer `ImageView`. Direct use is `image::ImageService::request` (already created as
`ctx.images()`) plus the texture cache the shell owns. Do not upload textures off the main thread.
`prefetch` warms the next row.

### JSON

`#include <nlohmann/json.hpp>` with the `SYSTEM` include of `third_party/` shown above. Parse
failures use the non-throwing `parse(..., false)` form unless you want exceptions on a worker.

### Luna

`platform::luna_call_sync(uri, json_payload, timeout_ms, public_bus)` blocks. Worker or startup
only. On host builds every call returns `Unsupported`. Do not add `libhelpers` to `DT_NEEDED`.

## Media playback

Follow `applications/examples/player/main.cpp`. The controller owns the source and the backend and
runs a playback thread. Public methods are main-thread calls. `stop()` and the destructor join
that thread (bounded by `PlaybackOptions::stop_deadline`).

```cpp
#include "wnk/media/adaptive_source.hpp"
#include "wnk/media/auto_backend.hpp"
#include "wnk/media/controller.hpp"
#include "wnk/media/device_caps.hpp"
#include "wnk/media/ffmpeg_source.hpp"
#include "wnk/media/ndl_backend.hpp"
#include "wnk/media/starfish_backend.hpp"
#include "wnk/media/subtitle_view.hpp"

std::unique_ptr<MediaBackend> make_backend(const DeviceMediaCaps &device) {
    StarfishOptions so;
    so.dolby_vision = device.dolby_vision == true;  // unknown is not yes
    so.atmos = device.dolby_atmos == true;
    auto ndl = std::make_unique<NdlBackend>();
    auto starfish = std::make_unique<StarfishBackend>(so);
    return std::make_unique<AutoBackend>(std::move(starfish), std::move(ndl));
}

std::unique_ptr<MediaSource> make_source(const std::string &uri,
                                         std::shared_ptr<net::HttpTransport> http,
                                         const DeviceMediaCaps &device) {
    if (AdaptiveSource::handles(uri)) {
        AdaptiveSourceOptions o;
        o.http = http;
        if (device.hevc == false) o.video_codecs = {CodecId::H264};
        if (device.uhd_panel != true) o.max_height = 1080;
        o.dolby_vision = device.dolby_vision == true;
        return std::make_unique<AdaptiveSource>(o);
    }
    return std::make_unique<FfmpegSource>(FfmpegSourceOptions{http, {}, true});
}
```

Wire-up that matches Example D:

1. Build the screen with a full-frame `SubtitleView` under a bottom `Column` of controls.
2. Create `PlaybackController(source, backend, ctx.dispatcher(), options)`.
3. `set_state_callback` updates labels. On `PlayState::Ready`, call
   `set_video_area({0, 0, 1920, 1080})` and pick the default subtitle track.
4. `on_show`: `ui.set_clear_color(kTransparent)`, `set_video_area` again, `open(uri)` once.
5. `on_hide`: clear colour back to `nullopt`.
6. `on_back`: `stop()`, return false so the navigator pops.
7. Delegate `on_lifecycle`: on `Suspended`, `pause_and_release()` and, on webOS,
   `NdlBackend::set_app_foreground(false)`. On `Resuming`, `set_app_foreground(true)`. When the
   transition into `Running` comes from `Resuming`, resume playback if it was playing.
8. Delegate `on_stop`: `stop()` if a player screen is up.
9. `on_frame`: read `snapshot()` for position; call `progress->set_value(position / duration)`.
   `position_estimated` is true when the backend does not report a clock.

Transport controls use actions, not key codes: `Confirm` toggles pause, `Left`/`Right` seek by a
fixed step (`seek_relative`), `Rewind`/`FastForward` if you want the transport keys,
`Up` opens the track menu (a `Dialog` with a `Grid` of buttons per kind: audio, then Off and the
subtitle tracks), `Down` cycles subtitle tracks (`set_subtitle_track`).

`set_audio_track` reloads the decoder (the codec may differ, and `AutoBackend` may choose the other
backend for it) and restarts at the keyframe at or before the position, like a seek: on DASH/HLS
that can be up to one segment earlier. The state goes `Seeking` → `Playing`; a paused player stays
paused and loads the new track on play. A track the source refuses is logged and the current one
keeps playing.

`AutoBackend` keeps Starfish when it can take the video codec and the audio (as-is, or AC-3
re-encode). NDL is the fallback (AV1, VP9, and audio the source cannot decode). On the measured
TV, Starfish plays 24 fps without the NDL judder and can pause for real. NDL is fed with a 1 s
lead because it was measured accepting a whole clip without backpressure.

Source choice:

| URI | Source |
|---|---|
| `file://` or a local path, MP4/MKV/TS/WebM | `FfmpegSource` |
| `http(s)` file (MP4, MKV, MPEG-TS) | `FfmpegSource` with `options.http = ctx.http()` |
| `.mpd` / `.m3u8`, VOD or live | `AdaptiveSource` |

Encrypted content plays when the app has the keys (ClearKey, `wnk/media/clearkey.hpp`): MPEG Common
Encryption ('cenc', 'cbcs', 'cens', 'cbc1') in MP4 and DASH. Give keys as
`AdaptiveSourceOptions::keys` / `FfmpegSourceOptions::decryption_keys` (`parse_content_keys("kid:key,...")`
also reads a JWK set). Key IDs the manifest names without a key are asked from
`AdaptiveSourceOptions::license_url`, else from the manifest's ClearKey licence URL. Keys are
secrets: never log them. Without keys `AdaptiveSource` refuses an encrypted presentation.
HLS `EXT-X-KEY` needs nothing from the app: METHOD=AES-128 segments (MPEG-TS or fMP4) are decrypted as
they download, with the key from its URI (sent with `headers` and `license_headers`) and rotating
keys followed per segment; SAMPLE-AES / SAMPLE-AES-CTR in fMP4 go through the CENC path with the
URI's key. SAMPLE-AES in MPEG-TS and FairPlay (`skd://`) are refused with the reason. TTML (`stpp`) is
listed and not decoded. Audio bitrate switching is not implemented; video bitrate switching is
(`adaptive_bitrate`, default on).

A static MPD may have several Periods (chapters, inserted clips). Each rendition continues into the
matching one of the next Period (same AdaptationSet and Representation ids, else same type, language
and codec at the closest bandwidth); the source opens it at the Period's end and clips packets to each
Period, and seeks land in the Period holding the target. Tracks, subtitles and bitrate choice come from
the first Period; a rendition with no match in the next Period ends there. A live (dynamic) MPD plays
its first Period. WebM renditions (VP9, Opus) play too, indexed by the file's Cues.

Subtitles the FFmpeg source can decode (SubRip, ASS/SSA, WebVTT, MP4 timed text, PGS, DVD, DVB)
become cues. ASS styling is stripped to plain text. Manifest subtitles: HLS WebVTT, DASH
`text/vtt` and `wvtt`. External subtitle files are not loaded.

Host builds have no NDL or Starfish. `PacingBackend` is the no-output stand-in (Example D uses it
when neither backend loads). Playback tests on the host check the controller, not the picture.

## Input

Actions: `Up Down Left Right Confirm Back Exit Home PlayPause Play Pause Stop Rewind FastForward
ChannelUp ChannelDown Red Green Yellow Blue VolumeUp VolumeDown Mute`.

Host keyboard (so you can develop without a TV): arrows, Enter = Confirm, Escape/Backspace = Back,
F1–F4 = colour keys, Space = PlayPause, Home = Home, F12 = Exit.

The shell maps keys with `default_keymap()` (webOS table on device, host table on the desktop).
Do not install a parallel mapper.

`Action::Exit` quits unless `on_action` returns true. `Action::Back` with an empty navigator stack
is the shell's double-Back exit when `double_back_exit` is set.

Volume, channel, Home, and some transport keys are `Assumed` until a TV capture marks them
`Measured`. Handle them if the feature needs them, and do not document them as measured.

## Lifecycle

```
Created → Initialising → Running ⇄ Suspended → Resuming → Running
                              └──────── Quit ────────→ Stopping → Terminated
```

The shell drives this from SDL. Another app covering yours sends background events; coming back
sends foreground. Measured on OLED65C1PTB, webOS 6.5.3: the process stays alive, draws nothing
while suspended, and the first frame after return was 8 ms later. SAM close runs `on_stop` and
teardown.

While `Suspended`, do not keep a hardware decoder. `pause_and_release()` unloads it and seeks back
on resume.

Low-memory trimming of the image cache is done by the shell. Do not add another cache that ignores
it; if you add one, trim it from `on_lifecycle` when you see the same path the shell uses (read
`app/src/app.cpp` before hooking anything new).

A crash writes an async-signal-safe record that the next launch logs. `tools/symbolize-crash.py`
resolves it against the unstripped binary in the build tree. You do not implement this.

## Logging

```cpp
WNK_LOGI(kCat, "opened %zu items", n);
WNK_LOGW(kCat, "store: %s", storage::status_name(st));
```

One category string per app (`"tvui"`, `"player"`). The shell writes a 0640 log in the runtime
directory. On a Developer Mode TV, `tools/logs.sh <app id>` or Dev Manager can read
`/tmp/<app id>/events.log`.

Every message passes through `wnk::log::redact()` before any sink. It masks the value of every query
and fragment parameter of a URL (`https://h/m.m3u8?sig=<redacted>&t=<redacted>`; names, hosts and
paths stay), credential-like key/value pairs, `Authorization` headers and bearer tokens. Signed media
URLs carry their signature under names no list knows, so URLs are logged without their query values.

## Verification before you hand the app over

Run from the repository root. Do not claim a step you did not run.

```sh
tools/test.sh                  # host build + unit tests, including Example B if you did not break it
tools/build-webos.sh           # ARM build; ipk must pass check-package.py
tools/verify.sh build/webos-release/dist/<appid>_<version>_arm.ipk \
    --runtime-libs cmake/sdl-webos-runtime-libs.txt
```

`verify.sh` must report 7/7 for webOS 5.3.1 and newer against the firmware inventory.

If you touched memory ownership, also `tools/test.sh host-asan`. If you touched threads (pool,
HTTP, WebSocket, playback), also `tools/test.sh host-tsan`.

Host run of a UI app:

```sh
SDL_VIDEODRIVER=offscreen WNK_MYAPP_KEYS=Right,Return WNK_MYAPP_SHOT=/tmp/shot.bmp \
    build/host-debug/applications/<name>/wnk-<name>
```

Device, only when a Developer Mode TV is configured (`ARES_DEVICE`, `ares-setup-device`):

```sh
tools/install.sh build/webos-release/dist/<appid>_<version>_arm.ipk
tools/launch.sh <appid>
tools/logs.sh <appid>
```

Before submitting to LG's store, work through `docs/LG-APP-CHECKLIST.md`:
`tools/lifecycle-check.sh <appid>` checks start, relaunch, background, foreground and close from the
event log, and `tools/soak.sh <appid> --hours 4` reports memory growth and crashes over a long run.

Record the model, webOS release, and date in `docs/compatibility/records.json` if you add a device
result, then run `tools/check-compat.py`. That checker rejects serials and MACs.

C++ is C++17, `-Wall -Wextra`, warnings as failures in the project builds. Match the surrounding
style: 4-space indent, `wnk::` namespaces, header comments that say why.

## What the kit does not provide

Say these are missing. Do not emulate them with a fake.

| Gap | Consequence |
|---|---|
| No text selection, clipboard, or multi-line text area | `TextField` is one line with a cursor. LG's keyboard opening for it is host-tested, not yet seen on a TV. |
| No Widevine / PlayReady | Encrypted content plays only with ClearKey (the app has the keys). |
| No scale or rotation | Focus "zoom" is the ring, not a scaled card. |
| No complex-script shaping, no right-to-left layout | Arabic, Hebrew, and Indic runs will be wrong. CJK, Cyrillic and Greek are drawn (with a fallback face for CJK). |
| No date or time formatting | `i18n` formats numbers only. The TV's system fonts for CJK are not yet seen from a Developer Mode app: bundle a fallback face if the app must show CJK. |
| No audio-only media player | Music files play through `audio/` (WAV, Ogg decoded whole); there is no streaming audio player with seek and metadata. |
| No second window, no overlay window | One SDL window, one GLES context. |
| No durable filesystem on Developer Mode webOS | Persist with DB8. |
| `keymanager3` absent on webOS 6.5.3 | Secrets are plain envelopes there unless `protection()` says otherwise. |
| TTML subtitles, external subtitle files, ASS styling, audio-only bitrate switching, DASH sidx boxes that index other sidx boxes | Not implemented. |
| Home and Exit as background/close paths | Not fully measured. The shell already handles `SDL_QUIT` and background events. |
| Jellyfin/Plex/Emby clients | Not in the tree. A client would be an app on top of HTTP + this UI + this player, not a change to `media/`. |

Also out of bounds: rooting the TV, patching firmware, bundling LG libraries, and linking
`libcurl` or NDL as `DT_NEEDED`.

## When you are unsure

1. Read the header comment of the type you are about to call. The comments in `app.hpp`,
   `view.hpp`, `collection.hpp`, `navigation.hpp`, `http.hpp`, `kv.hpp`, `controller.hpp`, and
   `adaptive_source.hpp` are the contract.
2. Read the example that already does the feature (`tvui/main.cpp`, `player/main.cpp`) and follow
   it.
3. For platform behaviour (paths, Luna, keys, decoder), read `docs/research/` and
   `docs/MILESTONES.md` before guessing. `docs/architecture/PROPOSAL.md` is an early sketch; the
   headers are the implementation.
4. If a capability is `Unknown`, branch on unknown as "do not advertise it", the way
   `device.dolby_vision == true` does. Unknown is not false and not true.

## Handoff checklist

Before calling the app done:

- [ ] New code is under `wnk::app::run` + `Delegate`, not a private SDL loop.
- [ ] Every screen is reachable with the D-pad, and Back restores the previous focus.
- [ ] Pointer hover, click, and wheel still work because the shell path was left intact.
- [ ] Network, DB, and decode work is off the main thread; callbacks check `Lifetime`.
- [ ] `AppConfig::http` / `storage` match the permissions in `wnk_add_webos_package`.
- [ ] Fonts are bundled. Remote images use `http` + `ImageView`. webOS persistence uses `ctx.store()`.
- [ ] A player screen clears to transparent, sets the video area, stops on Back and on `on_stop`,
      and releases the decoder when the lifecycle enters `Suspended`.
- [ ] `expected-needed.txt` matches the binary. No new firmware library was linked by `DT_NEEDED`.
- [ ] `tools/test.sh` and `tools/build-webos.sh` were run, and `verify.sh` is 7/7 if an ipk exists.
- [ ] Device claims cite a record. Gaps from the table above are stated, not papered over.
