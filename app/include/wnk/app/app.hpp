// SPDX-License-Identifier: GPL-3.0-or-later
//
// The app shell: everything a native webOS app needs around its UI, in the order the research
// established (docs/research/APPLICATION-LIFECYCLE.md):
//
//   process env, logs → SDL_Init(0) (SAM registration) → webOS hints → video, window, GLES2 context →
//   renderer, fonts → worker pool, dispatcher, storage, images → Ui → delegate.on_start → main loop
//
// A relaunch (the app launched again while running) reaches the delegate as on_relaunch with the new
// parameters. SDL-webOS drops them, so the kit's SDL build keeps them (cmake/patches/).
//
// The main loop polls SDL, maps keys to actions and pointer events to the Ui, drives the lifecycle
// state machine (Running ⇄ Suspended), stops drawing while backgrounded, and records frame timing.
// Shutdown is a fixed sequence of timed steps under a watchdog, because how long SAM waits between
// SIGTERM and SIGKILL is unknown:
//
//   on_stop (save state) → dispatcher.close → pool.shutdown(deadline) → delegate and UI destroyed →
//   image service, textures → renderer (GL objects) → GL context, window → SDL_Quit → log flush
//
// Apps implement Delegate and call run() from main(). Everything here runs on the main thread except
// the pool's workers and the shutdown watchdog.
#pragma once

#include "wnk/app/lifecycle.hpp"
#include "wnk/app/wake_lock.hpp"
#include "wnk/async/async.hpp"
#include "wnk/diagnostics/capabilities.hpp"
#include "wnk/diagnostics/metrics.hpp"
#include "wnk/i18n/i18n.hpp"
#include "wnk/image/image.hpp"
#include "wnk/input/input.hpp"
#include "wnk/net/http.hpp"
#include "wnk/platform/process.hpp"
#include "wnk/render/renderer.hpp"
#include "wnk/storage/kv.hpp"
#include "wnk/ui/ui.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wnk::app {

struct AppConfig {
    std::string fallback_app_id;  // used when $APPID is not set (host runs)
    std::string title = "wnk app";
    size_t worker_threads = 2;
    bool http = false;        // create the libcurl transport (dlopen) for HTTP(S) and image URLs
    bool images = true;       // image service + texture cache + ui::ImageSource
    size_t image_memory_bytes = 48u << 20;
    size_t texture_bytes = 64u << 20;
    bool storage = false;     // open the platform key/value store (DB8 on the TV; needs database.operation)
    bool double_back_exit = true;  // unhandled Back at the root: "Press Back again to exit"
    int cursor_sleep_ms = 5000;
    // Let the TV's screensaver start while the app is idle in the foreground (wake_lock.hpp). Hold a
    // Context::keep_screen_on() lock while video plays. false blocks it for the whole run.
    bool allow_screensaver = true;
    int shutdown_deadline_ms = 3000;  // the watchdog ends the process if teardown takes longer
    // Present a frame only when it differs from the one on screen: on the TV the compositor's work is
    // most of a frame's cost, and a still screen needs none of it. A still screen is drawn again every
    // idle_probe_ms to notice what changes with time alone (a caret, a playback clock) and presented
    // every keepalive_ms regardless. Input and worker results wake it at once. false: 60 fps always.
    bool redraw_on_change = true;
    int idle_probe_ms = 100;
    int keepalive_ms = 2000;
    ui::Theme theme;          // fonts are filled in by the shell
    std::string font_regular = "fonts/AtkinsonHyperlegible-Regular.ttf";  // relative to the app dir
    std::string font_bold = "fonts/AtkinsonHyperlegible-Bold.ttf";
    // Fonts for characters the two above lack, tried in order per character (relative to the app
    // directory, or absolute). Bundle one for scripts the app must always show (OFL faces only, and
    // record them in THIRD-PARTY-NOTICES.md).
    std::vector<std::string> fallback_fonts;
    // When a character is in no font above, look for a face that has it under these directories (on
    // a worker, the first time it is drawn; the text shows on the next frame once found).
    bool system_font_fallback = true;
    std::vector<std::string> system_font_dirs = {"/usr/share/fonts"};
    // Translations: <translations_dir>/<tag>.json under the app directory (i18n.hpp). The locale is the
    // TV's UI language (settingsservice localeInfo), LANG on the host, or {"wnkLocale":"fr-FR"} in the
    // launch parameters.
    std::string translations_dir = "i18n";
    // Scripted input for host tests: <prefix>_KEYS and <prefix>_SHOT (see Script in app.cpp);
    // <prefix>_METRICS=1 shows the metrics overlay from the start.
    std::string script_env_prefix = "WNK";
    // Action that toggles the metrics overlay anywhere in the app. None by default: the kit does not
    // give keys side effects the app did not ask for. Launch params {"wnkMetrics":true} also show it.
    input::Action metrics_toggle = input::Action::Unknown;
};

// What the delegate can reach. Valid from on_start until on_stop returns.
class Context {
 public:
    virtual ~Context() = default;
    virtual ui::Ui &ui() = 0;
    virtual render::Renderer &renderer() = 0;
    virtual async::WorkerPool &pool() = 0;
    virtual async::MainDispatcher &dispatcher() = 0;
    virtual image::ImageService *images() = 0;              // nullptr unless AppConfig::images
    virtual std::shared_ptr<net::HttpTransport> http() = 0;  // nullptr unless AppConfig::http and available
    virtual storage::KeyValueStore *store() = 0;             // nullptr unless AppConfig::storage and opened
    virtual const platform::LaunchInfo &launch() = 0;
    // The UI language's catalog: ctx.i18n().tr("Search by name"), trn("{n} show", "{n} shows", n).
    virtual const i18n::Catalog &i18n() const = 0;
    // The locale for numbers (the TV's formatting setting, which may differ from the UI language).
    virtual const i18n::Locale &formats_locale() const = 0;
    virtual const std::string &runtime_dir() = 0;            // empty if unavailable
    virtual State state() const = 0;
    virtual const diagnostics::FrameMetrics &frame_metrics() const = 0;
    // What this device was found to support, and which device it is (no identifiers).
    virtual diagnostics::Capabilities &capabilities() = 0;
    virtual const diagnostics::DeviceInfo &device() const = 0;
    // Blocks the screensaver until the returned lock is released or destroyed (main thread).
    virtual WakeLock keep_screen_on(std::string reason) = 0;
    virtual void set_metrics_overlay(bool visible) = 0;
    virtual bool metrics_overlay() const = 0;
    // Ends the main loop after the current iteration; the shutdown sequence then runs.
    virtual void quit(const char *reason) = 0;
};

class Delegate {
 public:
    virtual ~Delegate() = default;
    // Build the UI. The first frame is drawn after this returns.
    virtual void on_start(Context &ctx) = 0;
    // Actions the Ui did not consume. Return true to stop the shell's default handling
    // (Exit quits; Back at the root asks for a second Back).
    virtual bool on_action(Context &ctx, const input::ActionEvent &e);
    // After every lifecycle transition (Suspended: pause media and timers; Resuming: refresh).
    virtual void on_lifecycle(Context &ctx, const Lifecycle::Transition &t);
    // The app was launched again while it runs (a deep link, a launcher shortcut, voice search, a
    // second `ares-launch` with parameters). `params_json` is the new parameters object ("{}" when
    // there were none); Context::launch() already holds it. The window has been brought to the front.
    virtual void on_relaunch(Context &ctx, const std::string &params_json);
    // Once per drawn frame, after the Ui has been updated.
    virtual void on_frame(Context &ctx, float dt);
    // Shutdown has begun: save state. Workers and the dispatcher still run; the UI still exists.
    virtual void on_stop(Context &ctx);
};

using DelegateFactory = std::function<std::unique_ptr<Delegate>()>;

// Runs the app; returns the process exit code.
int run(int argc, char **argv, const AppConfig &config, const DelegateFactory &factory);

}  // namespace wnk::app
