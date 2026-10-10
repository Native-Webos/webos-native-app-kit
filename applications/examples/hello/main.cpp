// SPDX-License-Identifier: GPL-3.0-or-later
//
// Example A: the smallest complete native app on the kit (Milestones 2-3).
//
// It demonstrates and records:
//   * process setup, launch parameters, a safe log file and a device/storage probe;
//   * the GLES2 renderer on a 1920x1080 logical canvas: rounded tiles, borders, text, clipping;
//   * remote input through the semantic action layer, and focus navigation through FocusManager:
//     a tile grid (directional focus with an animated ring), a modal dialog that traps focus and
//     restores it on close, and "press Back again to exit" at the root;
//   * an on-screen key log that shows every raw key with its mapped action and the provenance of the
//     mapping, so the remote's real codes can be captured on a TV (also written to the event log);
//   * lifecycle events and an orderly shutdown.
//
// Keys: arrows / OK to navigate and select; Back closes the dialog or (twice) quits; Exit quits.
// On the host: Escape/Backspace = Back, F12 = Exit, F1-F4 = colour keys.
//
// Scripted smoke test (host diagnostics; SAM launches cannot pass environment variables):
//   WNK_HELLO_KEYS="Right,Right,Down,Return"  SDL scancode names injected as key presses, 6 frames apart
//   WNK_HELLO_SHOT=/path/out.bmp              save the frame after the script, then quit
#include "wnk/focus/focus.hpp"
#include "wnk/input/input.hpp"
#include "wnk/input/sdl_input.hpp"
#include "wnk/log.hpp"
#include "wnk/platform/luna.hpp"
#include "wnk/platform/probe.hpp"
#include "wnk/platform/process.hpp"
#include "wnk/render/gles2_renderer.hpp"

#include <SDL.h>

#include <dlfcn.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace {

using wnk::Rect;
using wnk::focus::Direction;
using wnk::focus::FocusManager;
using wnk::focus::NodeId;
using wnk::input::Action;
using wnk::input::Phase;
using wnk::render::Color;
using wnk::render::TextStyle;

constexpr const char *kFallbackAppId = "org.wnk.examples.hello";
constexpr const char *kCat = "hello";
constexpr Uint32 kDoubleBackMs = 1500;

namespace theme {
constexpr Color kBackground = Color::hex(0x0E1420FF);
constexpr Color kPanel = Color::hex(0x172033FF);
constexpr Color kTile = Color::hex(0x24324DFF);
constexpr Color kTileFocused = Color::hex(0x3A5BA0FF);
constexpr Color kRing = Color::hex(0xFFD54AFF);
constexpr Color kText = Color::hex(0xF2F5FAFF);
constexpr Color kMuted = Color::hex(0x9AA7BDFF);
constexpr Color kAccent = Color::hex(0x7CC4FFFF);
constexpr Color kScrim = Color::hex(0x000000B0);
constexpr Color kDialog = Color::hex(0x1E2A42FF);
}  // namespace theme

// Focus scopes and node ids.
constexpr wnk::focus::ScopeId kGridScope = 1;
constexpr wnk::focus::ScopeId kDialogScope = 2;
constexpr NodeId kTileBase = 100;
constexpr NodeId kDialogClose = 200;
constexpr NodeId kDialogExit = 201;
constexpr int kCols = 4, kRows = 3;

const char *kTileLabels[kCols * kRows] = {"Movies", "Shows", "Music", "Photos", "Live", "Search",
                                          "Library", "Downloads", "Settings", "Network", "About", "Help"};

Rect tile_rect(int index) {
    const int c = index % kCols, r = index / kCols;
    return {80.0f + c * 300.0f, 220.0f + r * 220.0f, 270, 190};
}

void log_webos_display_info() {
    using PanelFn = SDL_bool (*)(int *, int *);
    using RateFn = SDL_bool (*)(int *);
    auto panel = reinterpret_cast<PanelFn>(dlsym(RTLD_DEFAULT, "SDL_webOSGetPanelResolution"));
    auto rate = reinterpret_cast<RateFn>(dlsym(RTLD_DEFAULT, "SDL_webOSGetRefreshRate"));
    if (panel != nullptr) {
        int w = 0, h = 0;
        SDL_bool ok = panel(&w, &h);
        WNK_LOGI("display", "SDL_webOSGetPanelResolution ok=%d %dx%d", ok ? 1 : 0, w, h);
    } else {
        WNK_LOGI("display", "SDL_webOSGetPanelResolution: not exported by the loaded SDL");
    }
    if (rate != nullptr) {
        int hz = 0;
        SDL_bool ok = rate(&hz);
        WNK_LOGI("display", "SDL_webOSGetRefreshRate ok=%d %d", ok ? 1 : 0, hz);
    } else {
        WNK_LOGI("display", "SDL_webOSGetRefreshRate: not exported by the loaded SDL");
    }
}

void log_probe(const wnk::platform::LaunchInfo &launch, const std::string &runtime) {
    for (const auto &[key, value] : wnk::platform::collect_system_facts()) {
        WNK_LOGI("probe", "%s=%s", key.c_str(), value.c_str());
    }
    for (const auto &m : wnk::platform::writable_mounts()) {
        WNK_LOGI("probe", "rw mount %s on %s type %s (%s)", m.device.c_str(), m.mount_point.c_str(),
                 m.fs_type.c_str(), m.options.c_str());
    }
    for (const auto &c : wnk::platform::extended_storage_candidates(launch.app_id, launch.app_dir, runtime)) {
        auto r = wnk::platform::probe_writable(c);
        WNK_LOGI("probe", "storage %-7s %-60s writable=%d (%s) class=%s [%s]", c.purpose.c_str(),
                 c.directory.c_str(), r.writable ? 1 : 0, r.detail.c_str(),
                 wnk::platform::durability_name(c.durability), c.evidence.c_str());
    }
}

void log_gpu(const wnk::render::GpuInfo &g) {
    WNK_LOGI("gl", "GL_VENDOR=%s", g.vendor.c_str());
    WNK_LOGI("gl", "GL_RENDERER=%s", g.renderer.c_str());
    WNK_LOGI("gl", "GL_VERSION=%s", g.version.c_str());
    WNK_LOGI("gl", "GL_SHADING_LANGUAGE_VERSION=%s", g.shading_language.c_str());
    WNK_LOGI("gl", "GL_MAX_TEXTURE_SIZE=%d fragment_highp=%d", g.max_texture_size, g.fragment_highp ? 1 : 0);
    for (size_t pos = 0; pos < g.extensions.size(); pos += 400) {
        WNK_LOGI("gl", "GL_EXTENSIONS[%zu]=%s", pos / 400, g.extensions.substr(pos, 400).c_str());
    }
}

// Key presses injected through SDL's event queue, so they take the same path as real keys.
class Script {
 public:
    Script() {
        if (const char *keys = SDL_getenv("WNK_HELLO_KEYS")) {
            std::string list = keys;
            size_t start = 0;
            while (start <= list.size()) {
                const size_t comma = list.find(',', start);
                const std::string name = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                const SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
                if (sc != SDL_SCANCODE_UNKNOWN) {
                    keys_.push_back(sc);
                } else if (!name.empty()) {
                    WNK_LOGW("script", "unknown key name '%s'", name.c_str());
                }
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        if (const char *shot = SDL_getenv("WNK_HELLO_SHOT")) shot_ = shot;
    }
    bool active() const { return !keys_.empty() || !shot_.empty(); }
    // Returns true when the script has finished and the app should quit.
    bool step(Uint64 frame, wnk::render::Gles2Renderer &r, int dw, int dh) {
        if (!active()) return false;
        const Uint64 first = 20, gap = 6;
        if (frame >= first && next_ < keys_.size() && (frame - first) % gap == 0) {
            push(keys_[next_], true);
            push(keys_[next_], false);
            ++next_;
        }
        if (next_ < keys_.size() || frame < first + keys_.size() * gap + 40) return false;
        if (!shot_.empty()) save(r, dw, dh);
        return true;
    }

 private:
    static void push(SDL_Scancode sc, bool down) {
        SDL_Event ev{};
        ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
        ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
        ev.key.keysym.scancode = sc;
        ev.key.keysym.sym = SDL_GetKeyFromScancode(sc);
        ev.key.timestamp = SDL_GetTicks();
        SDL_PushEvent(&ev);
    }
    void save(wnk::render::Gles2Renderer &r, int dw, int dh) {
        std::vector<uint8_t> px = r.read_pixels(0, 0, dw, dh);
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(px.data(), dw, dh, 32, dw * 4, SDL_PIXELFORMAT_ABGR8888);
        if (s != nullptr && SDL_SaveBMP(s, shot_.c_str()) == 0) {
            WNK_LOGI("script", "screenshot saved to %s (%dx%d)", shot_.c_str(), dw, dh);
        } else {
            WNK_LOGE("script", "screenshot failed: %s", SDL_GetError());
        }
        if (s != nullptr) SDL_FreeSurface(s);
    }
    std::vector<SDL_Scancode> keys_;
    size_t next_ = 0;
    std::string shot_;
};

// Experiment: can the app itself persist data in DB8 (webOS's built-in JSON store) over Luna?
// Writes one record of its own kind with a launch counter; a value that survives relaunch and reboot
// shows DB8 works for app data on this firmware. Returns a one-line summary for the screen.
std::string db8_experiment(const std::string &app_id) {
    if (!wnk::platform::kIsWebOS) return "DB8: not on webOS";
    const std::string kind = app_id + ".wnk:1";
    // DB8 rejected a 33-character _id ("db: Invalid _id length", OLED65C1PTB webOS 6.5.3); PlxNative's ids
    // are at most 15 characters. Keep record ids short.
    const std::string id = "wnkhello.state";
    for (const char *service : {"luna://com.palm.db", "luna://com.webos.service.db"}) {
        auto call = [&](const char *method, const std::string &payload) {
            auto r = wnk::platform::luna_call_sync(std::string(service) + "/" + method, payload, 3000);
            WNK_LOGI("db8", "%s/%s %s -> %s %s", service, method, payload.c_str(),
                     wnk::platform::luna_status_name(r.status), r.status == wnk::platform::LunaStatus::Ok
                                                                       ? r.payload.c_str()
                                                                       : r.detail.c_str());
            return r;
        };
        auto ok = [](const wnk::platform::LunaReply &r) {
            return r.status == wnk::platform::LunaStatus::Ok &&
                   r.payload.find("\"returnValue\":true") != std::string::npos;
        };
        auto kind_reply = call("putKind", "{\"id\":\"" + kind + "\",\"owner\":\"" + app_id + "\",\"indexes\":[]}");
        if (!ok(kind_reply)) continue;
        auto got = call("get", "{\"ids\":[\"" + id + "\"]}");
        long previous = 0;
        std::string rev_field;  // updating an existing object requires its current _rev (optimistic concurrency)
        const size_t pos = got.payload.find("\"counter\":");
        if (ok(got) && pos != std::string::npos) previous = std::strtol(got.payload.c_str() + pos + 10, nullptr, 10);
        const size_t rev = got.payload.find("\"_rev\":");
        if (ok(got) && rev != std::string::npos) {
            rev_field = ",\"_rev\":" + std::to_string(std::strtoll(got.payload.c_str() + rev + 7, nullptr, 10));
        }
        const long next = previous + 1;
        auto put = call("put", "{\"objects\":[{\"_id\":\"" + id + "\",\"_kind\":\"" + kind + "\"" + rev_field + ",\"" +
                                   "counter\":" + std::to_string(next) + ",\"written\":" +
                                   std::to_string(static_cast<long long>(time(nullptr))) + "}]}");
        char line[160];
        std::snprintf(line, sizeof line, "DB8 via %s: launch #%ld (previous value %ld), write %s", service + 7, next,
                      previous, ok(put) ? "ok" : "FAILED");
        WNK_LOGI("db8", "%s", line);
        return line;
    }
    return "DB8: putKind refused on both service names (see log)";
}

struct KeyLogLine {
    std::string text;
    Color color;
};

class Demo {
 public:
    Demo(wnk::render::Renderer &r, wnk::render::FontId regular, wnk::render::FontId bold, std::string storage_note)
        : r_(r), regular_(regular), bold_(bold), mapper_(wnk::input::default_keymap()),
          storage_note_(std::move(storage_note)) {
        fm_.add_scope({kGridScope, wnk::focus::kRootScope, false, true});
        fm_.add_scope({kDialogScope, wnk::focus::kRootScope, true, true});
        for (int i = 0; i < kCols * kRows; ++i) {
            wnk::focus::NodeSpec n;
            n.id = kTileBase + static_cast<NodeId>(i);
            n.scope = kGridScope;
            n.rect = tile_rect(i);
            fm_.upsert(n);
        }
        fm_.set_listener([this](NodeId from, NodeId to) { WNK_LOGD("focus", "%u -> %u", from, to); });
        fm_.enter_scope(kGridScope);
        ring_ = target_ring();
    }

    bool running() const { return running_; }

    void on_key(const SDL_Event &ev) {
        auto key = wnk::input::key_event_from_sdl(ev);
        if (!key) return;
        record_raw(*key);
        if (auto action = mapper_.map(*key)) handle(*action);
    }

    void on_focus_lost(Uint32 now) {
        for (const auto &a : mapper_.release_all(now)) handle(a);
    }

    void update(float dt) {
        // Ease the focus ring towards the focused node (exponential smoothing, frame-rate independent).
        const Rect target = target_ring();
        const float k = 1.0f - std::exp(-dt * 18.0f);
        ring_.x += (target.x - ring_.x) * k;
        ring_.y += (target.y - ring_.y) * k;
        ring_.w += (target.w - ring_.w) * k;
        ring_.h += (target.h - ring_.h) * k;
        dialog_t_ += ((dialog_open_ ? 1.0f : 0.0f) - dialog_t_) * (1.0f - std::exp(-dt * 14.0f));
    }

    void draw(Uint32 now, float fps) {
        r_.clear(theme::kBackground);
        TextStyle title{bold_, 64, theme::kText};
        TextStyle small{regular_, 26, theme::kMuted};
        r_.draw_text(title, {80, 130}, "webOS Native App Kit");
        r_.draw_text(small, {82, 175}, "Arrows/OK to move and select  ·  Back closes / exits  ·  Exit quits");

        for (int i = 0; i < kCols * kRows; ++i) {
            const Rect rc = tile_rect(i);
            const bool focused = fm_.focused() == kTileBase + static_cast<NodeId>(i);
            r_.fill_rect(rc, focused ? theme::kTileFocused : theme::kTile, 18);
            TextStyle label{focused ? bold_ : regular_, 38, theme::kText};
            const float w = r_.measure_text(label, kTileLabels[i]);
            r_.draw_text(label, {rc.cx() - w * 0.5f, rc.cy() + 13}, kTileLabels[i]);
        }
        if (!dialog_open_ || dialog_t_ < 0.99f) r_.stroke_rect(ring_.inset(-8), theme::kRing, 6, 24);

        draw_key_panel();
        draw_status(fps);
        if (dialog_t_ > 0.01f) draw_dialog();
        if (now < toast_until_) draw_toast(now);
    }

 private:
    Rect target_ring() const {
        const auto *n = fm_.node(fm_.focused());
        return n != nullptr ? n->rect : Rect{};
    }

    void record_raw(const wnk::input::KeyEvent &k) {
        if (!k.down) return;
        const auto *b = mapper_.keymap().find(k.scancode, k.keycode);
        char line[160];
        std::snprintf(line, sizeof line, "%s  sc=%d  kc=%d%s  %s", b ? wnk::input::action_name(b->action) : "unmapped",
                      k.scancode, k.keycode, k.repeat ? "  (repeat)" : "",
                      b ? wnk::input::provenance_name(b->provenance) : "");
        WNK_LOGI("key", "%s name='%s'", line, SDL_GetScancodeName(static_cast<SDL_Scancode>(k.scancode)));
        if (k.repeat && !key_log_.empty() && key_log_.front().text.find("(repeat)") != std::string::npos) {
            key_log_.front().text = line;  // collapse repeats into one line
            return;
        }
        key_log_.push_front({line, b ? theme::kText : theme::kAccent});
        while (key_log_.size() > 14) key_log_.pop_back();
    }

    void handle(const wnk::input::ActionEvent &a) {
        WNK_LOGD("action", "%s %s held=%u", wnk::input::action_name(a.action),
                 a.phase == Phase::Press ? "press" : a.phase == Phase::Repeat ? "repeat" : "release", a.held_ms);
        if (a.phase == Phase::Release) return;
        switch (a.action) {
            case Action::Up: fm_.move(Direction::Up); break;
            case Action::Down: fm_.move(Direction::Down); break;
            case Action::Left: fm_.move(Direction::Left); break;
            case Action::Right: fm_.move(Direction::Right); break;
            case Action::Confirm:
                if (a.phase == Phase::Press) confirm();
                break;
            case Action::Back:
                if (a.phase == Phase::Press) back(a.source.time_ms);
                break;
            case Action::Exit:
                WNK_LOGI(kCat, "Exit key: quitting");
                running_ = false;
                break;
            default:
                break;  // other actions are shown in the key log only
        }
    }

    void confirm() {
        const NodeId f = fm_.focused();
        if (f >= kTileBase && f < kTileBase + kCols * kRows) {
            selected_ = static_cast<int>(f - kTileBase);
            open_dialog();
        } else if (f == kDialogClose) {
            close_dialog();
        } else if (f == kDialogExit) {
            WNK_LOGI(kCat, "Exit chosen in dialog: quitting");
            running_ = false;
        }
    }

    void back(Uint32 now) {
        if (dialog_open_) {
            close_dialog();
            return;
        }
        if (last_back_ != 0 && now - last_back_ <= kDoubleBackMs) {
            WNK_LOGI(kCat, "Back twice at root: quitting");
            running_ = false;
            return;
        }
        last_back_ = now;
        toast_until_ = SDL_GetTicks() + kDoubleBackMs;
    }

    Rect dialog_rect() const { return {560, 340, 800, 400}; }
    Rect dialog_button(int i) const {
        const Rect d = dialog_rect();
        return {d.x + 80 + i * 340.0f, d.bottom() - 140, 300, 90};
    }

    void open_dialog() {
        dialog_open_ = true;
        wnk::focus::NodeSpec close;
        close.id = kDialogClose;
        close.scope = kDialogScope;
        close.rect = dialog_button(0);
        wnk::focus::NodeSpec exit = close;
        exit.id = kDialogExit;
        exit.rect = dialog_button(1);
        fm_.upsert(close);
        fm_.upsert(exit);
        fm_.push_modal(kDialogScope);
        WNK_LOGI(kCat, "dialog opened for tile '%s'", kTileLabels[selected_]);
    }

    void close_dialog() {
        fm_.pop_modal();
        fm_.remove(kDialogClose);
        fm_.remove(kDialogExit);
        dialog_open_ = false;
        WNK_LOGI(kCat, "dialog closed, focus restored to %u", fm_.focused());
    }

    void draw_key_panel() {
        const Rect panel{1300, 220, 540, 630};
        r_.fill_rect(panel, theme::kPanel, 16);
        r_.draw_text({bold_, 30, theme::kText}, {panel.x + 28, panel.y + 50}, "Key log");
        r_.draw_text({regular_, 20, theme::kMuted}, {panel.x + 150, panel.y + 50}, "action  scancode  keycode  source");
        r_.push_clip(panel.inset(12));
        float y = panel.y + 95;
        for (const auto &l : key_log_) {
            r_.draw_text({regular_, 24, l.color}, {panel.x + 28, y}, l.text);
            y += 37;
        }
        if (key_log_.empty()) r_.draw_text({regular_, 24, theme::kMuted}, {panel.x + 28, y}, "Press any key on the remote");
        r_.pop_clip();
    }

    void draw_status(float fps) {
        const auto s = r_.stats();
        char line[200];
        std::snprintf(line, sizeof line, "%.0f fps   draw calls %u   quads %u   glyphs %u   atlas resets %u   focus %u",
                      fps, s.draw_calls, s.quads, s.glyphs_cached, s.atlas_resets, fm_.focused());
        r_.draw_text({regular_, 22, theme::kMuted}, {80, 1020}, line);
        r_.draw_text({regular_, 22, theme::kAccent}, {80, 1055}, storage_note_);
    }

    void draw_dialog() {
        const float t = dialog_t_;
        r_.fill_rect({0, 0, 1920, 1080}, theme::kScrim.with_alpha(theme::kScrim.a * t));
        Rect d = dialog_rect();
        d.y += (1.0f - t) * 40;  // slide up while fading in
        r_.push_translate(0, d.y - dialog_rect().y);
        const Rect base = dialog_rect();
        r_.fill_rect(base, theme::kDialog.with_alpha(t), 24);
        TextStyle title{bold_, 46, theme::kText.with_alpha(t)};
        std::string text = std::string("You selected ") + kTileLabels[selected_];
        r_.draw_text(title, {base.x + 80, base.y + 110}, text);
        r_.draw_text({regular_, 28, theme::kMuted.with_alpha(t)}, {base.x + 80, base.y + 165},
                     "Focus is trapped here. Back closes the dialog.");
        const char *labels[2] = {"Close", "Exit app"};
        for (int i = 0; i < 2; ++i) {
            const Rect b = dialog_button(i);
            const bool focused = fm_.focused() == kDialogClose + static_cast<NodeId>(i);
            r_.fill_rect(b, (focused ? theme::kTileFocused : theme::kTile).with_alpha(t), 14);
            if (focused) r_.stroke_rect(b.inset(-6), theme::kRing.with_alpha(t), 5, 20);
            TextStyle s{focused ? bold_ : regular_, 34, theme::kText.with_alpha(t)};
            const float w = r_.measure_text(s, labels[i]);
            r_.draw_text(s, {b.cx() - w * 0.5f, b.cy() + 12}, labels[i]);
        }
        r_.pop_translate();
    }

    void draw_toast(Uint32 now) {
        const float remaining = static_cast<float>(toast_until_ - now) / kDoubleBackMs;
        const float a = std::min(1.0f, remaining * 4.0f);
        TextStyle s{bold_, 32, theme::kText.with_alpha(a)};
        const char *msg = "Press Back again to exit";
        const float w = r_.measure_text(s, msg);
        const Rect box{960 - w * 0.5f - 40, 900, w + 80, 70};
        r_.fill_rect(box, Color::hex(0x000000CC).with_alpha(0.8f * a), 35);
        r_.draw_text(s, {box.x + 40, box.y + 47}, msg);
    }

    wnk::render::Renderer &r_;
    wnk::render::FontId regular_, bold_;
    wnk::input::ActionMapper mapper_;
    std::string storage_note_;
    FocusManager fm_;
    Rect ring_;
    std::deque<KeyLogLine> key_log_;
    bool running_ = true;
    bool dialog_open_ = false;
    float dialog_t_ = 0;
    int selected_ = 0;
    Uint32 last_back_ = 0;
    Uint32 toast_until_ = 0;
};

}  // namespace

int main(int argc, char **argv) {
    wnk::platform::prepare_process_environment();
    const auto launch = wnk::platform::read_launch_info(argc, argv, kFallbackAppId);
    const std::string runtime = wnk::platform::runtime_dir(launch.app_id);

    wnk::log::Logger logger(wnk::log::Level::Debug);
    logger.add_sink(std::make_unique<wnk::log::StderrSink>());
    std::string log_path;
    if (!runtime.empty()) {
        log_path = runtime + "/events.log";
        int fd = wnk::log::open_log_file(log_path, wnk::log::OpenMode::Truncate);
        if (fd >= 0) logger.add_sink(std::make_unique<wnk::log::FileSink>(fd));
    }
    wnk::log::set_default_logger(&logger);

    WNK_LOGI(kCat, "start app_id=%s platform=%s log=%s", launch.app_id.c_str(),
             wnk::platform::kIsWebOS ? "webos" : "host", log_path.empty() ? "<stderr only>" : log_path.c_str());
    WNK_LOGI(kCat, "exe=%s app_dir=%s", launch.exe_path.c_str(), launch.app_dir.c_str());
    WNK_LOGI(kCat, "launch params: %s", launch.params_json.empty() ? "<none>" : launch.params_json.c_str());
    log_probe(launch, runtime);

    // SDL_Init(0) first: SDL-webOS registers the app with the application manager here.
    // Access-policy hints must be set before the window exists (moonlight-tv src/app/app.c).
    if (SDL_Init(0) != 0) {
        WNK_LOGE(kCat, "SDL_Init(0) failed: %s", SDL_GetError());
        wnk::log::set_default_logger(nullptr);
        return 1;
    }
    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");
    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_EXIT", "true");
    SDL_SetHint("SDL_WEBOS_CURSOR_SLEEP_TIME", "5000");

    SDL_version compiled, linked;
    SDL_VERSION(&compiled);
    SDL_GetVersion(&linked);
    WNK_LOGI("sdl", "compiled %d.%d.%d, linked %d.%d.%d, revision %s", compiled.major, compiled.minor,
             compiled.patch, linked.major, linked.minor, linked.patch, SDL_GetRevision());
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        WNK_LOGE(kCat, "SDL video init failed: %s", SDL_GetError());
        SDL_Quit();
        wnk::log::set_default_logger(nullptr);
        return 1;
    }
    WNK_LOGI("sdl", "video driver: %s", SDL_GetCurrentVideoDriver());
    SDL_DisplayMode mode{};
    if (SDL_GetCurrentDisplayMode(0, &mode) == 0) {
        WNK_LOGI("display", "current mode %dx%d @%dHz format=%s", mode.w, mode.h, mode.refresh_rate,
                 SDL_GetPixelFormatName(mode.format));
    }
    log_webos_display_info();

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    // webOS: SDL_WINDOW_FULLSCREEN at the display mode size (webosbrew samples sdl_shell.c).
    Uint32 flags = SDL_WINDOW_OPENGL;
    int win_w = 1280, win_h = 720;
    if constexpr (wnk::platform::kIsWebOS) {
        flags |= SDL_WINDOW_FULLSCREEN;
        win_w = mode.w > 0 ? mode.w : 1920;
        win_h = mode.h > 0 ? mode.h : 1080;
    } else {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    SDL_Window *window = SDL_CreateWindow("wnk hello", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, win_w,
                                          win_h, flags);
    if (window == nullptr) {
        WNK_LOGE(kCat, "SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        wnk::log::set_default_logger(nullptr);
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (gl == nullptr) {
        WNK_LOGE(kCat, "SDL_GL_CreateContext failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        wnk::log::set_default_logger(nullptr);
        return 1;
    }
    if (SDL_GL_SetSwapInterval(1) != 0) WNK_LOGW("gl", "vsync unavailable: %s", SDL_GetError());

    int exit_code = 0;
    {
        std::string err;
        auto renderer = wnk::render::Gles2Renderer::create(&err);
        if (!renderer) {
            WNK_LOGE("gl", "renderer init failed: %s", err.c_str());
            exit_code = 1;
        } else {
            log_gpu(renderer->gpu_info());
            const std::string font_dir = launch.app_dir + "/fonts";
            auto regular = renderer->load_font_file(font_dir + "/AtkinsonHyperlegible-Regular.ttf", &err);
            auto bold = renderer->load_font_file(font_dir + "/AtkinsonHyperlegible-Bold.ttf", &err);
            if (regular == wnk::render::kNoFont || bold == wnk::render::kNoFont) {
                WNK_LOGE("text", "font load failed: %s", err.c_str());
                exit_code = 1;
            } else {
                Demo demo(*renderer, regular, bold, db8_experiment(launch.app_id));
                Script script;
                const double perf_freq = static_cast<double>(SDL_GetPerformanceFrequency());
                Uint64 prev = SDL_GetPerformanceCounter();
                Uint64 frames = 0;
                float fps = 0, fps_acc = 0;
                int fps_frames = 0;
                bool pacing_logged = false;
                bool quit = false;
                while (!quit && demo.running()) {
                    SDL_Event ev;
                    while (SDL_PollEvent(&ev)) {
                        switch (ev.type) {
                            case SDL_QUIT:
                                WNK_LOGI(kCat, "SDL_QUIT");
                                quit = true;
                                break;
                            case SDL_KEYDOWN:
                            case SDL_KEYUP:
                                demo.on_key(ev);
                                break;
                            case SDL_WINDOWEVENT:
                                WNK_LOGI("window", "event %d %d %d", ev.window.event, ev.window.data1, ev.window.data2);
                                if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) demo.on_focus_lost(SDL_GetTicks());
                                if (ev.window.event == SDL_WINDOWEVENT_CLOSE) quit = true;
                                break;
                            case SDL_APP_WILLENTERBACKGROUND:
                                WNK_LOGI("lifecycle", "APP_WILLENTERBACKGROUND");
                                demo.on_focus_lost(SDL_GetTicks());
                                break;
                            case SDL_APP_DIDENTERBACKGROUND: WNK_LOGI("lifecycle", "APP_DIDENTERBACKGROUND"); break;
                            case SDL_APP_WILLENTERFOREGROUND: WNK_LOGI("lifecycle", "APP_WILLENTERFOREGROUND"); break;
                            case SDL_APP_DIDENTERFOREGROUND: WNK_LOGI("lifecycle", "APP_DIDENTERFOREGROUND"); break;
                            case SDL_APP_TERMINATING: WNK_LOGI("lifecycle", "APP_TERMINATING"); break;
                            case SDL_APP_LOWMEMORY: WNK_LOGW("lifecycle", "APP_LOWMEMORY"); break;
                            default: break;
                        }
                    }
                    if (quit) break;

                    const Uint64 now_counter = SDL_GetPerformanceCounter();
                    const float dt = static_cast<float>((now_counter - prev) / perf_freq);
                    prev = now_counter;
                    fps_acc += dt;
                    if (++fps_frames >= 30) {
                        fps = fps_frames / fps_acc;
                        fps_acc = 0;
                        fps_frames = 0;
                    }

                    int dw = 0, dh = 0;
                    SDL_GL_GetDrawableSize(window, &dw, &dh);
                    renderer->begin_frame(dw, dh);
                    demo.update(std::min(dt, 0.1f));
                    demo.draw(SDL_GetTicks(), fps);
                    renderer->end_frame();
                    if (script.step(frames, *renderer, dw, dh)) {
                        WNK_LOGI(kCat, "script finished");
                        quit = true;
                    }
                    SDL_GL_SwapWindow(window);
                    if (++frames == 1) {
                        WNK_LOGI(kCat, "first frame presented: drawable %dx%d scale %.3f", dw, dh,
                                 renderer->mapping().scale);
                    }

                    // If the swap did not block on vsync, sleep out the rest of a 60 Hz frame.
                    const double frame_ms = (SDL_GetPerformanceCounter() - now_counter) * 1000.0 / perf_freq;
                    if (frame_ms < 4.0) {
                        if (!pacing_logged) {
                            WNK_LOGI(kCat, "swap returned after %.2f ms: vsync not blocking, pacing to 60 Hz", frame_ms);
                            pacing_logged = true;
                        }
                        SDL_Delay(static_cast<Uint32>(1000.0 / 60.0 - frame_ms));
                    }
                }
                WNK_LOGI(kCat, "shutting down after %llu frames", static_cast<unsigned long long>(frames));
            }
        }
        // The renderer (GL objects) is destroyed here, while the context is still current.
    }

    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    WNK_LOGI(kCat, "shutdown complete (exit %d)", exit_code);
    logger.flush();
    wnk::log::set_default_logger(nullptr);
    return exit_code;
}
