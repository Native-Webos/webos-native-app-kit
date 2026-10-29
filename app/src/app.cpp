// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/app/app.hpp"

#include "wnk/app/overlay.hpp"
#include "wnk/audio/sdl_output.hpp"
#include "wnk/diagnostics/crash.hpp"
#include "wnk/image/textures.hpp"
#include "wnk/input/sdl_input.hpp"
#include "wnk/log.hpp"
#include "wnk/platform/luna.hpp"
#include "wnk/render/gles2_renderer.hpp"
#include "wnk/ui/image_source.hpp"
#include "wnk/ui/widgets.hpp"

#include <SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <mutex>
#include <map>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wnk::app {

bool Delegate::on_action(Context &, const input::ActionEvent &) { return false; }
void Delegate::on_lifecycle(Context &, const Lifecycle::Transition &) {}
void Delegate::on_relaunch(Context &, const std::string &) {}
bool Delegate::on_gamepad(Context &, const input::GamepadEvent &) { return false; }
void Delegate::on_network(Context &, const platform::NetworkStatus &) {}
bool Delegate::on_key(Context &, const input::KeyEvent &) { return false; }
void Delegate::on_frame(Context &, float) {}
void Delegate::on_stop(Context &) {}

namespace {

constexpr const char *kCat = "app";
constexpr Uint32 kDoubleBackMs = 1500;

// "version" from appinfo.json next to the executable (what SAM installed), or empty.
std::string app_version(const std::string &app_dir) {
    std::ifstream in(app_dir + "/appinfo.json");
    if (!in) return {};
    const auto j = nlohmann::json::parse(in, nullptr, false);
    return j.is_object() && j.contains("version") && j["version"].is_string() ? j["version"].get<std::string>() : "";
}

// {"wnkCrashTest":"segv"|"abort"} in the launch params crashes on purpose a few seconds after start,
// to prove the crash record works on a device. Returns "" when absent.
std::string crash_test_kind(const std::string &params) {
    if (params.empty()) return {};
    const auto j = nlohmann::json::parse(params, nullptr, false);
    if (!j.is_object() || !j.contains("wnkCrashTest") || !j["wnkCrashTest"].is_string()) return {};
    const std::string k = j["wnkCrashTest"];
    return k == "segv" || k == "abort" ? k : "";
}

__attribute__((noinline)) void crash_now(const std::string &kind) {
    if (kind == "abort") std::abort();
    // A write into an inaccessible page: a real SIGSEGV with a known fault address.
    void *page = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) std::abort();
    WNK_LOGW("crash", "crash test: writing to %p", page);
    if (auto *l = log::default_logger()) l->flush();
    *static_cast<volatile int *>(page) = 1;
}

double ms_since(Uint64 t) {
    return static_cast<double>(SDL_GetPerformanceCounter() - t) * 1000.0 /
           static_cast<double>(SDL_GetPerformanceFrequency());
}

// Ends the process if shutdown overruns its deadline: SAM's SIGTERM→SIGKILL interval is unknown,
// and a hung teardown must not leave the app half-closed on screen.
class Watchdog {
 public:
    ~Watchdog() { cancel(); }
    void arm(int deadline_ms, const char *const *step) {
        step_ = step;
        thread_ = std::thread([this, deadline_ms] {
            std::unique_lock<std::mutex> lock(mutex_);
            if (cv_.wait_for(lock, std::chrono::milliseconds(deadline_ms), [this] { return done_; })) return;
            WNK_LOGE(kCat, "shutdown watchdog: still in step '%s' after %d ms, exiting", *step_, deadline_ms);
            if (auto *l = log::default_logger()) l->flush();
            _exit(2);
        });
    }
    void cancel() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

 private:
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    const char *const *step_ = nullptr;
};

// Scripted input for host tests: <prefix>_KEYS is a comma-separated list, one step every 6 loop
// iterations after the 20th: SDL scancode names, move@X:Y / click@X:Y (logical units), wheel@N,
// bg / fg (the SDL app background/foreground event pairs SDL-webOS sends), text@STRING (typed text as
// SDL_TEXTINPUT, what LG's keyboard sends; no commas), kbclose (the viewer closes the keyboard),
// relaunch (<prefix>_RELAUNCH, a lifecycle message as the application manager sends it, arrives as
// SDL_webOSTakeRelaunchMessage would return it), pad (attach an SDL virtual game controller), pad+NAME /
// pad-NAME (press / release a button by SDL name: a, b, dpright, start...), padx=V / pady=V (left stick,
// -1..1), pad~ (unplug it), wait@MS (the next step comes MS of wall-clock time later, not a frame count:
// sanitizer builds draw frames several times slower). <prefix>_SHOT saves the
// last drawn frame as BMP. The app quits when the script ends.
class Script {
 public:
    explicit Script(const std::string &prefix) {
        if (const char *keys = SDL_getenv((prefix + "_KEYS").c_str())) parse(keys);
        if (const char *shot = SDL_getenv((prefix + "_SHOT").c_str())) shot_ = shot;
        if (const char *m = SDL_getenv((prefix + "_RELAUNCH").c_str())) relaunch_message_ = m;
    }
    ~Script() {
        if (pad_ != nullptr) SDL_JoystickClose(pad_);
        if (pad_index_ >= 0) SDL_JoystickDetachVirtual(pad_index_);
    }
    Script(const Script &) = delete;
    Script &operator=(const Script &) = delete;
    // The relaunch message a "relaunch" step released, once.
    std::string take_relaunch() { return std::exchange(relaunch_due_, std::string()); }
    bool active() const { return !steps_.empty() || !shot_.empty(); }
    // Returns true when the script has finished (the app should quit).
    bool tick(uint64_t iteration, render::Gles2Renderer &r, SDL_Window *window) {
        if (!active()) return false;
        const uint64_t first = 20, gap = 6;
        if (iteration >= first + next_ * gap + held_ && next_ < steps_.size()) {
            if (SDL_GetTicks64() < hold_until_) {
                ++held_;  // later steps move back by the frames spent waiting
            } else {
                if (steps_[next_].kind == Step::Relaunch) relaunch_due_ = relaunch_message_;
                if (steps_[next_].kind == Step::Wait) hold_until_ = SDL_GetTicks64() + static_cast<Uint64>(steps_[next_].x);
                run(steps_[next_++], r, window);
            }
        }
        return next_ >= steps_.size() && iteration >= first + steps_.size() * gap + held_ + 60;
    }
    void save(render::Gles2Renderer &r, int dw, int dh) const {
        if (shot_.empty()) return;
        std::vector<uint8_t> px = r.read_pixels(0, 0, dw, dh);
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(px.data(), dw, dh, 32, dw * 4, SDL_PIXELFORMAT_ABGR8888);
        if (s != nullptr && SDL_SaveBMP(s, shot_.c_str()) == 0) {
            WNK_LOGI("script", "screenshot saved to %s (%dx%d)", shot_.c_str(), dw, dh);
        } else {
            WNK_LOGE("script", "screenshot failed: %s", SDL_GetError());
        }
        if (s != nullptr) SDL_FreeSurface(s);
    }

 private:
    struct Step {
        enum Kind { Key, Move, Click, Wheel, Background, Foreground, Text, KeyboardClosed, Relaunch, Pad, Wait } kind = Key;
        SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
        float x = 0, y = 0;
        std::string text;
    };
    void parse(const std::string &list) {
        size_t start = 0;
        while (start <= list.size()) {
            const size_t comma = list.find(',', start);
            const std::string name = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            Step st;
            if (std::sscanf(name.c_str(), "move@%f:%f", &st.x, &st.y) == 2) {
                st.kind = Step::Move;
            } else if (std::sscanf(name.c_str(), "click@%f:%f", &st.x, &st.y) == 2) {
                st.kind = Step::Click;
            } else if (std::sscanf(name.c_str(), "wait@%f", &st.x) == 1) {
                st.kind = Step::Wait;
            } else if (std::sscanf(name.c_str(), "wheel@%f", &st.y) == 1) {
                st.kind = Step::Wheel;
            } else if (name.rfind("text@", 0) == 0) {
                st.kind = Step::Text;
                st.text = name.substr(5);
            } else if (name == "kbclose") {
                st.kind = Step::KeyboardClosed;
            } else if (name == "relaunch") {
                st.kind = Step::Relaunch;
            } else if (name.rfind("pad", 0) == 0) {
                st.kind = Step::Pad;
                st.text = name.substr(3);
            } else if (name == "bg") {
                st.kind = Step::Background;
            } else if (name == "fg") {
                st.kind = Step::Foreground;
            } else {
                st.key = SDL_GetScancodeFromName(name.c_str());
                if (st.key == SDL_SCANCODE_UNKNOWN) {
                    if (!name.empty()) WNK_LOGW("script", "unknown step '%s'", name.c_str());
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                    continue;
                }
            }
            steps_.push_back(st);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    static void push_type(Uint32 type) {
        SDL_Event ev{};
        ev.type = type;
        SDL_PushEvent(&ev);
    }
    // SDL's virtual joystick, typed as a game controller so SDL maps its buttons and axes one to one.
    void pad(const std::string &cmd) {
        if (cmd.empty()) {
            SDL_VirtualJoystickDesc desc{};
            desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
            desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
            desc.naxes = SDL_CONTROLLER_AXIS_MAX;
            desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
            desc.name = "wnk virtual pad";
            pad_index_ = SDL_JoystickAttachVirtualEx(&desc);
            pad_ = pad_index_ >= 0 ? SDL_JoystickOpen(pad_index_) : nullptr;
            if (pad_ == nullptr) WNK_LOGW("script", "virtual pad: %s", SDL_GetError());
            return;
        }
        if (pad_ == nullptr) return;
        if (cmd == "~") {
            SDL_JoystickClose(pad_);
            pad_ = nullptr;
            SDL_JoystickDetachVirtual(pad_index_);
            pad_index_ = -1;
        } else if (cmd[0] == '+' || cmd[0] == '-') {
            const SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(cmd.c_str() + 1);
            if (b != SDL_CONTROLLER_BUTTON_INVALID) SDL_JoystickSetVirtualButton(pad_, b, cmd[0] == '+' ? 1 : 0);
        } else if (cmd.size() > 2 && cmd[1] == '=') {
            const auto v = static_cast<Sint16>(std::clamp(std::atof(cmd.c_str() + 2), -1.0, 1.0) * 32767);
            SDL_JoystickSetVirtualAxis(pad_, cmd[0] == 'x' ? SDL_CONTROLLER_AXIS_LEFTX : SDL_CONTROLLER_AXIS_LEFTY, v);
        }
    }

    void run(const Step &st, render::Gles2Renderer &r, SDL_Window *window) {
        // Logical units → drawable pixels → window units: the reverse of the input path.
        const Point px = r.mapping().to_pixels({st.x, st.y});
        int ww = 0, wh = 0, dw = 0, dh = 0;
        SDL_GetWindowSize(window, &ww, &wh);
        SDL_GL_GetDrawableSize(window, &dw, &dh);
        const int wx = static_cast<int>(px.x * ww / std::max(1, dw));
        const int wy = static_cast<int>(px.y * wh / std::max(1, dh));
        SDL_Event ev{};
        switch (st.kind) {
            case Step::Key:
                for (bool down : {true, false}) {
                    ev = SDL_Event{};
                    ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
                    ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
                    ev.key.keysym.scancode = st.key;
                    ev.key.keysym.sym = SDL_GetKeyFromScancode(st.key);
                    ev.key.timestamp = SDL_GetTicks();
                    SDL_PushEvent(&ev);
                }
                break;
            case Step::Click:
            case Step::Move:
                ev.type = SDL_MOUSEMOTION;
                ev.motion.x = wx;
                ev.motion.y = wy;
                ev.motion.timestamp = SDL_GetTicks();
                SDL_PushEvent(&ev);
                if (st.kind == Step::Move) break;
                for (bool down : {true, false}) {
                    ev = SDL_Event{};
                    ev.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
                    ev.button.button = SDL_BUTTON_LEFT;
                    ev.button.state = down ? SDL_PRESSED : SDL_RELEASED;
                    ev.button.x = wx;
                    ev.button.y = wy;
                    ev.button.timestamp = SDL_GetTicks();
                    SDL_PushEvent(&ev);
                }
                break;
            case Step::Wheel:
                ev.type = SDL_MOUSEWHEEL;
                ev.wheel.y = static_cast<Sint32>(st.y);
                ev.wheel.timestamp = SDL_GetTicks();
                SDL_PushEvent(&ev);
                break;
            case Step::Text:
                ev.type = SDL_TEXTINPUT;
                SDL_strlcpy(ev.text.text, st.text.c_str(), sizeof ev.text.text);
                ev.text.timestamp = SDL_GetTicks();
                SDL_PushEvent(&ev);
                break;
            case Step::Relaunch:
            case Step::Wait: break;  // tick() hands the message to the shell / holds the next step
            case Step::Pad: pad(st.text); break;
            case Step::KeyboardClosed:
                // What SDL-webOS does when the panel closes: text events off (SDL_IsTextInputActive false).
                SDL_EventState(SDL_TEXTINPUT, SDL_DISABLE);
                SDL_EventState(SDL_TEXTEDITING, SDL_DISABLE);
                break;
            case Step::Background:
                push_type(SDL_APP_WILLENTERBACKGROUND);
                push_type(SDL_APP_DIDENTERBACKGROUND);
                break;
            case Step::Foreground:
                push_type(SDL_APP_WILLENTERFOREGROUND);
                push_type(SDL_APP_DIDENTERFOREGROUND);
                break;
        }
    }
    std::vector<Step> steps_;
    size_t next_ = 0;
    uint64_t held_ = 0;      // frames spent in wait@ steps
    Uint64 hold_until_ = 0;  // SDL_GetTicks64() before which the next step waits
    std::string shot_;
    std::string relaunch_message_, relaunch_due_;
    SDL_Joystick *pad_ = nullptr;
    int pad_index_ = -1;
};

class Shell final : public Context {
 public:
    Shell(const AppConfig &config, platform::LaunchInfo launch, std::string runtime)
        : config_(config), launch_(std::move(launch)), runtime_(std::move(runtime)),
          mapper_(input::default_keymap()) {}

    // ---- Context ----
    ui::Ui &ui() override { return *ui_; }
    render::Renderer &renderer() override { return *renderer_; }
    async::WorkerPool &pool() override { return *pool_; }
    async::MainDispatcher &dispatcher() override { return *dispatcher_; }
    image::ImageService *images() override { return images_.get(); }
    std::shared_ptr<net::HttpTransport> http() override { return http_; }
    storage::KeyValueStore *store() override { return store_.get(); }
    audio::Mixer *audio() override { return audio_out_ ? mixer_.get() : nullptr; }
    const platform::LaunchInfo &launch() override { return launch_; }
    const i18n::Catalog &i18n() const override { return catalog_; }
    const i18n::Locale &formats_locale() const override { return formats_; }
    const std::string &runtime_dir() override { return runtime_; }
    State state() const override { return lifecycle_.state(); }
    const diagnostics::FrameMetrics &frame_metrics() const override { return metrics_; }
    diagnostics::Capabilities &capabilities() override { return caps_; }
    const diagnostics::DeviceInfo &device() const override { return device_; }
    WakeLock keep_screen_on(std::string reason) override { return wake_locks_->acquire(std::move(reason)); }
    void set_metrics_overlay(bool visible) override {
        if (visible != overlay_) WNK_LOGI(kCat, "metrics overlay %s", visible ? "on" : "off");
        overlay_ = visible;
    }
    bool metrics_overlay() const override { return overlay_; }
    const platform::NetworkStatus &network() const override { return network_; }
    void launch_app(const std::string &app_id, const std::string &params_json, std::function<void(bool)> done) override {
        system_call("launch " + app_id, [app_id, params_json] { return platform::launch_app(app_id, params_json); }, std::move(done));
    }
    void open_url(const std::string &url, std::function<void(bool)> done) override {
        system_call("open url", [url] { return platform::open_url(url); }, std::move(done));
    }
    void system_toast(const std::string &message, std::function<void(bool)> done) override {
        system_call("toast", [id = launch_.app_id, message] { return platform::system_toast(id, message); }, std::move(done));
    }
    void quit(const char *reason) override {
        if (!quit_requested_) WNK_LOGI(kCat, "quit requested: %s", reason);
        quit_requested_ = true;
    }

    int run(const DelegateFactory &factory) {
        setup_crash_record();
        lifecycle_.set_listener([this](const Lifecycle::Transition &t) {
            WNK_LOGI("lifecycle", "%s -> %s (%s)", state_name(t.from), state_name(t.to), event_name(t.event));
            diagnostics::set_crash_state(state_name(t.to));
            if (audio_out_) audio_out_->pause(t.to == State::Suspended || t.to == State::Stopping || t.to == State::Terminated);
            if (t.to == State::Suspended) {
                note_capability("lifecycle.background", diagnostics::Support::Supported,
                                std::string("background event received (") + event_name(t.event) + ")");
            }
            if (delegate_ && ui_) delegate_->on_lifecycle(*this, t);
        });
        lifecycle_.dispatch(Event::Start, SDL_GetTicks());
        int code = start(factory);
        if (code == 0) loop();
        shutdown();
        return code;
    }

 private:
    void setup_crash_record() {
        device_ = diagnostics::DeviceInfo::detect(platform::kIsWebOS);
        WNK_LOGI("device", "%s", device_.summary().c_str());
        if (runtime_.empty()) {
            WNK_LOGW("crash", "no runtime directory: crash records disabled");
            return;
        }
        const std::string path = runtime_ + "/crash.log";
        const auto prev = diagnostics::take_previous_crash(path);
        if (prev.found) {
            WNK_LOGW("crash", "previous run crashed: %s (record kept as %s.prev)", prev.summary.c_str(), path.c_str());
            std::istringstream lines(prev.text);
            int n = 0;
            for (std::string line; std::getline(lines, line) && n < 120; ++n) WNK_LOGW("crash", "| %s", line.c_str());
        }
        diagnostics::CrashConfig cc;
        cc.path = path;
        cc.app_id = launch_.app_id;
        cc.version = app_version(launch_.app_dir);
        cc.build_id = diagnostics::own_build_id();
        std::string err;
        if (diagnostics::install_crash_handler(cc, &err)) {
            WNK_LOGI("crash", "handler installed: %s, version %s, build id %s", path.c_str(),
                     cc.version.empty() ? "?" : cc.version.c_str(), cc.build_id.empty() ? "?" : cc.build_id.c_str());
            caps_.set("diagnostics.crash_record", diagnostics::Support::Supported, path);
        } else {
            WNK_LOGW("crash", "handler not installed: %s", err.c_str());
            caps_.set("diagnostics.crash_record", diagnostics::Support::Unsupported, err);
        }
        crash_test_ = crash_test_kind(launch_.params_json);
        if (!launch_.params_json.empty()) {
            const auto j = nlohmann::json::parse(launch_.params_json, nullptr, false);
            if (j.is_object() && j.contains("wnkMetrics") && j["wnkMetrics"].is_boolean() && j["wnkMetrics"].get<bool>()) {
                overlay_ = true;
            }
        }
        if (const char *m = SDL_getenv((config_.script_env_prefix + "_METRICS").c_str()); m != nullptr && m[0] == '1') {
            overlay_ = true;
        }
        if (!crash_test_.empty()) WNK_LOGW("crash", "crash test requested: %s after 180 frames", crash_test_.c_str());
    }

    int start(const DelegateFactory &factory) {
        // SDL_Init(0) first: SDL-webOS registers the app with SAM here. Access-policy hints must be
        // set before the window exists (moonlight-tv src/app/app.c).
        if (SDL_Init(0) != 0) return fail("SDL_Init(0)");
        sdl_ = true;
        SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");
        SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_EXIT", "true");
        SDL_SetHint("SDL_WEBOS_CURSOR_SLEEP_TIME", std::to_string(config_.cursor_sleep_ms).c_str());
        // SDL disables the screensaver at video init unless told otherwise; SDL-webOS then refuses every
        // screensaver request from the TV. The wake locks decide instead.
        SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return fail("SDL video init");
        if (config_.gamepads) {
            // Nintendo pads over Bluetooth: SDL's own Switch driver (HIDAPI) fails on the TV reading the
            // stick calibration ("Couldn't load stick calibration", Pro Controller, OLED65C1PTB); the
            // kernel's driver works, so leave those to it, as moonlight-tv does (src/app/app.c).
            SDL_SetHintWithPriority("SDL_WEBOS_HIDAPI_IGNORE_BLUETOOTH_DEVICES", "0x057e/0x0000", SDL_HINT_DEFAULT);
            if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
                WNK_LOGW("gamepad", "no game controller support: %s", SDL_GetError());
            } else {
                log_input_devices();
                const std::string db = launch_.app_dir + "/" + config_.gamepad_mappings;
                if (!config_.gamepad_mappings.empty() && access(db.c_str(), R_OK) == 0) {
                    const int n = SDL_GameControllerAddMappingsFromFile(db.c_str());
                    WNK_LOGI("gamepad", "%d mappings from %s", n, db.c_str());
                }
            }
        }
        wake_locks_ = std::make_unique<WakeLocks>(config_.allow_screensaver, [](bool block, const std::string &why) {
            if (block) {
                SDL_DisableScreenSaver();
            } else {
                SDL_EnableScreenSaver();
            }
            WNK_LOGI("screensaver", "%s (%s)", block ? "blocked" : "allowed", why.c_str());
        });
        SDL_DisplayMode mode{};
        SDL_GetCurrentDisplayMode(0, &mode);
        WNK_LOGI(kCat, "video driver %s, display mode %dx%d @%d Hz", SDL_GetCurrentVideoDriver(), mode.w, mode.h,
                 mode.refresh_rate);

        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
        Uint32 flags = SDL_WINDOW_OPENGL;
        int w = 1280, h = 720;
        if constexpr (platform::kIsWebOS) {
            // Fullscreen at the display mode size (webosbrew samples sdl_shell.c).
            flags |= SDL_WINDOW_FULLSCREEN;
            w = mode.w > 0 ? mode.w : 1920;
            h = mode.h > 0 ? mode.h : 1080;
        } else {
            flags |= SDL_WINDOW_RESIZABLE;
        }
        window_ = SDL_CreateWindow(config_.title.c_str(), SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h, flags);
        if (window_ == nullptr) return fail("SDL_CreateWindow");
        gl_ = SDL_GL_CreateContext(window_);
        if (gl_ == nullptr) return fail("SDL_GL_CreateContext");
        if (SDL_GL_SetSwapInterval(1) != 0) WNK_LOGW("gl", "vsync unavailable: %s", SDL_GetError());

        std::string err;
        renderer_ = render::Gles2Renderer::create(&err);
        if (!renderer_) return fail(("renderer: " + err).c_str());
        const auto gpu = renderer_->gpu_info();
        WNK_LOGI("gl", "%s / %s / %s", gpu.vendor.c_str(), gpu.renderer.c_str(), gpu.version.c_str());
        ui::Theme theme = config_.theme;
        theme.regular = renderer_->load_font_file(launch_.app_dir + "/" + config_.font_regular, &err);
        theme.bold = renderer_->load_font_file(launch_.app_dir + "/" + config_.font_bold, &err);
        if (theme.regular == render::kNoFont || theme.bold == render::kNoFont) return fail(("fonts: " + err).c_str());
        for (const auto &f : config_.fallback_fonts) {
            const std::string path = !f.empty() && f[0] == '/' ? f : launch_.app_dir + "/" + f;
            const render::FontId id = renderer_->load_font_file(path, &err);
            if (id == render::kNoFont) {
                WNK_LOGW("fonts", "fallback font not loaded: %s", err.c_str());
                continue;
            }
            renderer_->add_fallback_font(id);
            WNK_LOGI("fonts", "fallback font %s", path.c_str());
        }

        load_locale();
        dispatcher_ = std::make_unique<async::MainDispatcher>();
        // A result posted from a worker wakes an idle loop at once (one pending wake event at a time).
        wake_event_ = SDL_RegisterEvents(1);
        if (wake_event_ != static_cast<Uint32>(-1)) {
            dispatcher_->set_wake([this] {
                if (wake_pending_.exchange(true)) return;
                SDL_Event e{};
                e.type = wake_event_;
                SDL_PushEvent(&e);
            });
        }
        pool_ = std::make_unique<async::WorkerPool>(config_.worker_threads);
        // The shell's own short jobs (font search, Luna requests, speech) get a thread of their own: on
        // the app's pool they queued behind poster downloads, and Japanese text stayed blank for 2 s.
        shell_pool_ = std::make_unique<async::WorkerPool>(1);
        if (config_.http) {
            http_ = net::make_curl_transport({}, &err);
            if (!http_) WNK_LOGW("net", "no HTTP transport: %s", err.c_str());
        }
        if (config_.storage) {
            store_ = storage::open_default_store(launch_.app_id, &err);
            if (!store_) WNK_LOGW("storage", "key/value store unavailable: %s", err.c_str());
        }
        if (config_.audio) {
            mixer_ = std::make_unique<audio::Mixer>();
            audio_out_ = audio::SdlOutput::open(*mixer_, &err);
            if (audio_out_) {
                WNK_LOGI("audio", "output %s", audio_out_->describe().c_str());
            } else {
                WNK_LOGW("audio", "no audio output: %s", err.c_str());
            }
        }
        ui_ = std::make_unique<ui::Ui>(*renderer_, theme);
        if (config_.images) {
            image::ImageServiceConfig ic;
            ic.memory_bytes = config_.image_memory_bytes;
            images_ = std::make_unique<image::ImageService>(http_, *pool_, *dispatcher_, ic);
            textures_ = std::make_unique<image::ImageTextures>(*renderer_, config_.texture_bytes);
            image_source_ = std::make_unique<ui::ServiceImageSource>(*images_, *textures_);
            ui_->set_image_source(image_source_.get());
        }
        // Kit SDL patch (cmake/patches/sdl-webos-relaunch-message.patch); absent in other SDL builds.
        take_relaunch_ = reinterpret_cast<char *(*)()>(dlsym(RTLD_DEFAULT, "SDL_webOSTakeRelaunchMessage"));
        probe_capabilities(mode, gpu);
        watch_network();
        watch_audio_guidance();
        delegate_ = factory();
        if (!delegate_) return fail("no delegate");
        delegate_->on_start(*this);
        lifecycle_.dispatch(Event::Started, SDL_GetTicks());
        return 0;
    }

    // Facts the running app can establish itself. Everything else stays Unknown until seen.
    void probe_capabilities(const SDL_DisplayMode &mode, const render::GpuInfo &gpu) {
        using diagnostics::Support;
        caps_.set("display.gles2", Support::Supported, gpu.renderer + " / " + gpu.version);
        caps_.set("display.fragment_highp", gpu.fragment_highp ? Support::Supported : Support::Unsupported, "");
        caps_.set("display.max_texture_size", Support::Supported, std::to_string(gpu.max_texture_size));
        caps_.set("display.mode", Support::Supported,
                  std::to_string(mode.w) + "x" + std::to_string(mode.h) + " @" + std::to_string(mode.refresh_rate) + " Hz");
        // SDL-webOS extensions, looked up at runtime so another SDL build still links and runs.
        using PanelFn = SDL_bool (*)(int *, int *);
        using RateFn = SDL_bool (*)(int *);
        auto panel = reinterpret_cast<PanelFn>(dlsym(RTLD_DEFAULT, "SDL_webOSGetPanelResolution"));
        auto rate = reinterpret_cast<RateFn>(dlsym(RTLD_DEFAULT, "SDL_webOSGetRefreshRate"));
        int pw = 0, ph = 0, hz = 0;
        if (panel != nullptr && panel(&pw, &ph) == SDL_TRUE) {
            caps_.set("display.panel", Support::Supported, std::to_string(pw) + "x" + std::to_string(ph));
        } else {
            caps_.set("display.panel", Support::Unknown, panel == nullptr ? "SDL_webOSGetPanelResolution not exported" : "query failed");
        }
        if (rate != nullptr && rate(&hz) == SDL_TRUE) {
            caps_.set("display.refresh_rate", Support::Supported, std::to_string(hz) + " Hz");
        } else {
            caps_.set("display.refresh_rate", Support::Unknown, rate == nullptr ? "SDL_webOSGetRefreshRate not exported" : "query failed");
        }
        if (config_.storage) {
            caps_.set("storage.kv", store_ ? Support::Supported : Support::Unsupported, store_ ? store_->backend_name() : "open failed");
        } else {
            caps_.set("storage.kv", Support::Unknown, "not requested by the app", "not-probed");
        }
        if (config_.http) {
            caps_.set("net.http", http_ ? Support::Supported : Support::Unsupported, http_ ? http_->describe() : "libcurl unavailable");
        } else {
            caps_.set("net.http", Support::Unknown, "not requested by the app", "not-probed");
        }
        if (config_.audio) {
            caps_.set("audio.output", audio_out_ ? Support::Supported : Support::Unsupported,
                      audio_out_ ? audio_out_->describe() : "device did not open");
        } else {
            caps_.set("audio.output", Support::Unknown, "not requested by the app", "not-probed");
        }
        caps_.set("images.decode", config_.images ? Support::Supported : Support::Unknown, config_.images ? "PNG, JPEG, GIF, BMP, WebP, SVG" : "not requested");
        caps_.set("input.pointer", Support::Unknown, "no pointer event yet");
        caps_.set("lifecycle.background", Support::Unknown, "no background event yet");
        caps_.set("lifecycle.relaunch_params", take_relaunch_ != nullptr ? Support::Supported : Support::Unsupported,
                  take_relaunch_ != nullptr ? "SDL_webOSTakeRelaunchMessage" : "SDL build without the kit's relaunch patch");
        caps_.set("media.playback", Support::Unknown, "not probed by the shell (wnk_media checks per file)", "not-probed");
        for (const auto &c : caps_.all()) {
            WNK_LOGI("caps", "%-26s %-11s %s", c.id.c_str(), diagnostics::support_name(c.support), c.detail.c_str());
        }
        write_capabilities();
    }

    void note_capability(const char *id, diagnostics::Support s, const std::string &detail) {
        if (caps_.set(id, s, detail)) {
            WNK_LOGI("caps", "%s now %s: %s", id, diagnostics::support_name(s), detail.c_str());
        }
    }

    void write_capabilities() {
        if (runtime_.empty()) return;
        const std::string path = runtime_ + "/capabilities.json";
        {
            std::ofstream out(path, std::ios::trunc);
            if (!out) return;
            out << diagnostics::capability_report_json(device_, caps_);
        }
        chmod(path.c_str(), 0640);  // readable by Dev Manager (app group), like the event log
    }

    int fail(const char *what) {
        WNK_LOGE(kCat, "%s failed: %s", what, SDL_GetError());
        quit_requested_ = true;
        return 1;
    }

    void loop() {
        Script script(config_.script_env_prefix);
        const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
        Uint64 prev = SDL_GetPerformanceCounter();
        uint64_t iteration = 0, frames = 0, drawn = 0;
        int dw = 0, dh = 0;
        uint64_t presented_fp = 0;
        Uint32 last_present_ms = 0, last_change_ms = 0;
        bool idle = false;           // the last drawn frame looked like the one on screen: not presented
        bool prev_presented = false;  // for frame metrics: intervals only between consecutive presents
        while (!quit_requested_) {
            const Uint64 t_events = SDL_GetPerformanceCounter();
            SDL_Event ev;
            size_t work = 0;
            // Backgrounded: draw nothing, wake for events (and worker results) a few times a second.
            // Idle: wait for input or a worker result, drawing again every idle_probe_ms to notice what
            // changes with time alone; for half a second after the last change, every frame.
            if (!lifecycle_.renders()) {
                if (SDL_WaitEventTimeout(&ev, script.active() ? 16 : 250)) on_event(ev);
            } else if (idle) {
                const bool settling = SDL_GetTicks() - last_change_ms < 500;
                if (SDL_WaitEventTimeout(&ev, script.active() || settling ? 16 : config_.idle_probe_ms)) {
                    on_event(ev);
                    ++work;
                }
            }
            while (!quit_requested_ && SDL_PollEvent(&ev)) {
                on_event(ev);
                ++work;
            }
            if (quit_requested_) break;
            const float events_ms = static_cast<float>(ms_since(t_events));

            work += dispatcher_->drain(32, std::chrono::milliseconds(4));
            heartbeat(frames, drawn);
            sync_text_input();
            for (const auto &a : pad_nav_.tick(SDL_GetTicks())) {
                handle_action(a);
                ++work;
            }
            if (char *m = take_relaunch_ != nullptr ? take_relaunch_() : nullptr) {
                on_relaunch_message(m);
                SDL_free(m);
                ++work;
            }
            const bool script_done = script.tick(iteration++, *renderer_, window_);
            if (std::string m = script.take_relaunch(); !m.empty()) {
                on_relaunch_message(m.c_str());
                ++work;
            }
            if (script_done && !lifecycle_.renders()) {
                quit("script finished");
                break;
            }
            if (!lifecycle_.renders()) {
                prev = SDL_GetPerformanceCounter();  // no catch-up jump in animations after resuming
                continue;
            }

            const Uint64 now = SDL_GetPerformanceCounter();
            float dt = static_cast<float>((now - prev) / freq);
            prev = now;
            // Woken from idle by input or a result: animations it starts begin with one frame's step.
            if (idle && work > 0) dt = std::min(dt, 1.0f / 60.0f);
            ui_->update(std::min(dt, 0.1f));
            delegate_->on_frame(*this, std::min(dt, 0.1f));
            log_focus_change();
            const float update_ms = static_cast<float>(ms_since(now));

            const Uint64 t_draw = SDL_GetPerformanceCounter();
            SDL_GL_GetDrawableSize(window_, &dw, &dh);
            renderer_->begin_frame(dw, dh);
            ui_->draw();
            if (overlay_) draw_overlay();
            const float draw_ms = static_cast<float>(ms_since(t_draw));
            const Uint64 t_finish = SDL_GetPerformanceCounter();
            renderer_->end_frame();
            if (textures_) textures_->end_frame();
            find_missing_glyphs();
            const float finish_ms = static_cast<float>(ms_since(t_finish));
            if (script_done) {
                script.save(*renderer_, dw, dh);  // before the swap, while the frame is still readable
                quit("script finished");
            }
            if (++drawn == 180 && !crash_test_.empty()) {
                WNK_LOGW("crash", "crash test: triggering %s", crash_test_.c_str());
                if (auto *l = log::default_logger()) l->flush();
                crash_now(crash_test_);
            }
            const uint64_t fp = renderer_->frame_fingerprint();
            const Uint32 ticks = SDL_GetTicks();
            const bool changed = fp != presented_fp;
            const bool present = !config_.redraw_on_change || frames == 0 || changed ||
                                 lifecycle_.state() == State::Resuming || ticks - last_present_ms >= static_cast<Uint32>(config_.keepalive_ms);
            idle = !present;
            if (!present) {
                prev_presented = false;
                continue;
            }
            if (changed) last_change_ms = ticks;
            presented_fp = fp;
            last_present_ms = ticks;
            const Uint64 t_swap = SDL_GetPerformanceCounter();
            SDL_GL_SwapWindow(window_);
            const float swap_ms = static_cast<float>(ms_since(t_swap));
            if (++frames == 1) WNK_LOGI(kCat, "first frame: drawable %dx%d", dw, dh);
            if (lifecycle_.state() == State::Resuming) lifecycle_.dispatch(Event::FramePresented, SDL_GetTicks());

            if (frames > 1 && prev_presented) metrics_.add({dt * 1000.0f, events_ms, update_ms, draw_ms, finish_ms, swap_ms});
            prev_presented = true;
            if (frames % 300 == 0) log_perf(frames, drawn);

            // If the swap did not block on vsync, sleep out the rest of a 60 Hz frame.
            const double frame_ms = ms_since(now);
            if (frame_ms < 4.0) SDL_Delay(static_cast<Uint32>(1000.0 / 60.0 - frame_ms));
        }
        WNK_LOGI(kCat, "main loop ended after %llu frames presented, %llu drawn", static_cast<unsigned long long>(frames),
                 static_cast<unsigned long long>(drawn));
    }

    void on_event(const SDL_Event &ev) {
        switch (ev.type) {
            case SDL_QUIT: quit("SDL_QUIT"); break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                // SDL-webOS announces the pointer appearing/hiding as pseudo-keys.
                if (auto p = input::pointer_event_from_sdl(ev, window_)) on_pointer(*p);
                on_key(ev);
                break;
            case SDL_MOUSEMOTION:
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
            case SDL_MOUSEWHEEL:
                if (auto p = input::pointer_event_from_sdl(ev, window_)) on_pointer(*p);
                break;
            case SDL_JOYDEVICEADDED:
                if (config_.gamepads) log_joysticks("joystick added");
                break;
            case SDL_CONTROLLERDEVICEADDED:
            case SDL_CONTROLLERDEVICEREMOVED:
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP:
            case SDL_CONTROLLERAXISMOTION:
                on_pad(ev);
                break;
            case SDL_TEXTINPUT:
                if (lifecycle_.renders()) ui_->handle_text(ev.text.text);
                break;
            case SDL_TEXTEDITING:
                if (lifecycle_.renders()) ui_->handle_composition(ev.edit.text);
                break;
            case SDL_WINDOWEVENT:
                on_window(ev);
                break;
            case SDL_APP_WILLENTERBACKGROUND:
            case SDL_APP_DIDENTERBACKGROUND:
                WNK_LOGI("lifecycle", "SDL %s", ev.type == SDL_APP_WILLENTERBACKGROUND ? "WILLENTERBACKGROUND"
                                                                                     : "DIDENTERBACKGROUND");
                background();
                break;
            case SDL_APP_WILLENTERFOREGROUND:
            case SDL_APP_DIDENTERFOREGROUND:
                WNK_LOGI("lifecycle", "SDL %s", ev.type == SDL_APP_WILLENTERFOREGROUND ? "WILLENTERFOREGROUND"
                                                                                     : "DIDENTERFOREGROUND");
                lifecycle_.dispatch(Event::Foreground, SDL_GetTicks());
                break;
            case SDL_APP_TERMINATING: quit("SDL_APP_TERMINATING"); break;
            case SDL_APP_LOWMEMORY:
                WNK_LOGW("lifecycle", "SDL_APP_LOWMEMORY: trimming the image memory cache");
                if (images_) images_->memory().trim(config_.image_memory_bytes / 4);
                break;
            default:
                if (ev.type == wake_event_) wake_pending_ = false;  // the loop drains the dispatcher next
                break;
        }
    }

    void on_window(const SDL_Event &ev) {
        if (auto p = input::pointer_event_from_sdl(ev, window_)) on_pointer(*p);
        switch (ev.window.event) {
            case SDL_WINDOWEVENT_FOCUS_LOST:
                WNK_LOGI("window", "focus lost");
                release_keys();
                break;
            case SDL_WINDOWEVENT_FOCUS_GAINED: WNK_LOGI("window", "focus gained"); break;
            case SDL_WINDOWEVENT_MINIMIZED:
            case SDL_WINDOWEVENT_HIDDEN:
                WNK_LOGI("window", "%s", ev.window.event == SDL_WINDOWEVENT_MINIMIZED ? "minimized" : "hidden");
                background();
                break;
            case SDL_WINDOWEVENT_RESTORED:
            case SDL_WINDOWEVENT_SHOWN:
                WNK_LOGI("window", "%s", ev.window.event == SDL_WINDOWEVENT_RESTORED ? "restored" : "shown");
                lifecycle_.dispatch(Event::Foreground, SDL_GetTicks());
                break;
            case SDL_WINDOWEVENT_CLOSE: quit("window close"); break;
            default: break;
        }
    }

    void background() {
        release_keys();
        for (const auto &a : pad_nav_.release_all(SDL_GetTicks())) handle_action(a);
        if (ui_ && ui_->text_input_view() != nullptr) ui_->text_input_closed();  // the keyboard goes with the app
        lifecycle_.dispatch(Event::Background, SDL_GetTicks());
    }

    void release_keys() {
        for (const auto &a : mapper_.release_all(SDL_GetTicks())) handle_action(a);
    }

    void on_key(const SDL_Event &ev) {
        auto key = input::key_event_from_sdl(ev);
        if (!key) return;
        if (lifecycle_.renders() && delegate_->on_key(*this, *key)) return;
        if (key->down) {
            const auto *b = mapper_.keymap().find(key->scancode, key->keycode);
            WNK_LOGD("key", "down sc=%d kc=0x%x repeat=%d action=%s pointer=%d", key->scancode, key->keycode,
                     key->repeat ? 1 : 0, b != nullptr ? input::action_name(b->action) : "unmapped",
                     ui_->pointer_active() ? 1 : 0);
        }
        // While a view edits text, editing keys go to it and printable keys arrive as SDL_TEXTINPUT, so
        // neither becomes an action (Backspace is not Back, Space does not pause). Key-ups still reach
        // the mapper: it ignores ups whose down it never saw, and ends a press that began before.
        if (key->down && lifecycle_.renders() && ui_->text_input_view() != nullptr) {
            if (auto edit = input::edit_key_from(*key)) {
                // Holding OK must not submit again and again; erasing and moving repeat.
                if (!(key->repeat && *edit == input::EditKey::Enter)) ui_->handle_edit_key(*edit);
                return;
            }
            if (input::key_types_text(*key)) return;
        }
        if (auto a = mapper_.map(*key)) handle_action(*a);
    }

    // The UI language and its catalog. Launch parameters win (testing a language without changing the
    // TV's), then the TV's settings, then the host's environment.
    void load_locale() {
        std::string source;
        i18n::Locale ui = i18n::Locale::parse("en"), formats = ui;
        const auto params = nlohmann::json::parse(launch_.params_json.empty() ? "{}" : launch_.params_json, nullptr, false);
        if (params.is_object() && params.contains("wnkLocale") && params["wnkLocale"].is_string()) {
            ui = formats = i18n::Locale::parse(params["wnkLocale"].get<std::string>());
            source = "launch parameters";
        } else if constexpr (platform::kIsWebOS) {
            const auto reply = platform::luna_call_sync("luna://com.webos.settingsservice/getSystemSettings",
                                                        R"({"keys":["localeInfo"]})", 600);
            if (reply.status == platform::LunaStatus::Ok && i18n::parse_locale_info(reply.payload, &ui, &formats)) {
                source = "settingsservice";
                caps_.set("i18n.system_locale", diagnostics::Support::Supported, ui.tag());
            } else {
                source = std::string("default (settingsservice: ") +
                         (reply.status == platform::LunaStatus::Ok ? "no localeInfo" : platform::luna_status_name(reply.status)) + ")";
                caps_.set("i18n.system_locale", diagnostics::Support::Unsupported, source);
            }
        } else {
            for (const char *var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
                const char *v = SDL_getenv(var);
                if (v != nullptr && v[0] != '\0') {
                    ui = formats = i18n::Locale::parse(v);
                    source = var;
                    break;
                }
            }
            if (source.empty()) source = "default";
        }
        catalog_ = i18n::Catalog(ui);
        formats_ = formats;
        std::string err;
        const int files = catalog_.load_dir(launch_.app_dir + "/" + config_.translations_dir, &err);
        if (!err.empty()) WNK_LOGW("i18n", "%s", err.c_str());
        font_preferences_ = i18n::font_preferences(ui);
        WNK_LOGI("i18n", "locale %s (formats %s) from %s; %d catalog file(s), %zu entries", ui.tag().c_str(),
                 formats.tag().c_str(), source.c_str(), files, catalog_.size());
    }

    // Characters no loaded font has: search the system font directories on a worker, then add what
    // was found to the renderer's fallback chain and lay the UI out again. One search at a time;
    // characters reported meanwhile wait for the next.
    void find_missing_glyphs() {
        for (char32_t cp : renderer_->take_missing_codepoints()) glyphs_wanted_.push_back(cp);
        if (glyphs_wanted_.empty() || font_search_running_) return;
        std::vector<char32_t> wanted = std::exchange(glyphs_wanted_, {});
        if (!config_.system_font_fallback) {
            for (char32_t cp : wanted) WNK_LOGW("fonts", "no font has U+%04X", static_cast<unsigned>(cp));
            return;
        }
        font_search_running_ = true;
        auto finder = font_finder_;
        const auto dirs = config_.system_font_dirs;
        const auto preferred = font_preferences_;
        const Uint64 t0 = SDL_GetPerformanceCounter();
        struct Found {
            std::shared_ptr<render::FontFinder> finder;
            std::vector<std::string> paths;
        };
        async::run_then_post<Found>(
            *shell_pool_, *dispatcher_, life_.weak(), {},
            [finder, dirs, preferred, wanted](const async::CancellationToken &) {
                auto f = finder ? finder : std::make_shared<render::FontFinder>(dirs);
                return Found{f, f->find(wanted, preferred)};
            },
            [this, wanted, t0](Found found) {
                font_search_running_ = false;
                const bool first = !font_finder_;
                font_finder_ = found.finder;
                if (first) WNK_LOGI("fonts", "%zu font files under the system font directories", font_finder_->files().size());
                std::string err;
                for (const auto &path : found.paths) {
                    if (!loaded_fallbacks_.insert(path).second) continue;
                    const render::FontId id = renderer_->load_font_file(path, &err);
                    if (id == render::kNoFont) {
                        WNK_LOGW("fonts", "%s", err.c_str());
                        continue;
                    }
                    renderer_->add_fallback_font(id);
                    WNK_LOGI("fonts", "system fallback font %s (search %.0f ms)", path.c_str(), ms_since(t0));
                    caps_.set("text.system_fallback_fonts", diagnostics::Support::Supported, path);
                }
                // Anything still missing stays .notdef; the renderer reports each character once.
                for (char32_t cp : wanted) {
                    bool covered = false;
                    for (const auto &path : found.paths) covered = covered || covered_by(path, cp);
                    if (!covered) WNK_LOGW("fonts", "no font has U+%04X", static_cast<unsigned>(cp));
                }
                if (!found.paths.empty()) ui_->invalidate_layout();
            });
    }

    bool covered_by(const std::string &path, char32_t cp) {
        auto it = path_faces_.find(path);
        if (it == path_faces_.end()) it = path_faces_.emplace(path, render::FontFace::from_file(path)).first;
        return it->second && it->second->covers(cp);
    }

    // What the app can see of game controllers: its groups, which input event files it may open, and
    // every joystick SDL found (a pad without a controller mapping is a joystick but not a controller).
    void log_input_devices() {
        gid_t groups[64];
        const int n = getgroups(64, groups);
        std::string gs;
        for (int i = 0; i < n; ++i) gs += (gs.empty() ? "" : ",") + std::to_string(groups[i]);
        int readable = 0, total = 0;
        for (int i = 0; i < 64; ++i) {
            const std::string path = "/dev/input/event" + std::to_string(i);
            if (access(path.c_str(), F_OK) != 0) continue;
            ++total;
            if (access(path.c_str(), R_OK) == 0) ++readable;
        }
        WNK_LOGI("gamepad", "uid %u gid %u groups [%s]; /dev/input event files readable %d of %d", getuid(), getgid(),
                 gs.c_str(), readable, total);
        log_joysticks("at start");
    }

    void log_joysticks(const char *when) {
        const int count = SDL_NumJoysticks();
        WNK_LOGI("gamepad", "%s: %d joystick(s)", when, count);
        for (int i = 0; i < count; ++i) {
            char guid[40];
            SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof guid);
            const char *name = SDL_JoystickNameForIndex(i);
            WNK_LOGI("gamepad", "  joystick %d: %s, guid %s, %s", i, name != nullptr ? name : "?", guid,
                     SDL_IsGameController(i) ? "game controller" : "NO controller mapping");
        }
    }

    void on_pad(const SDL_Event &ev) {
        auto e = gamepads_.on_sdl(ev);
        if (!e) {
            if (ev.type == SDL_CONTROLLERDEVICEADDED) {
                WNK_LOGW("gamepad", "controller %d not opened: %s", ev.cdevice.which, SDL_GetError());
            }
            return;
        }
        if (e->type == input::GamepadEvent::Type::Added) {
            WNK_LOGI("gamepad", "pad %d connected: %s (%zu connected)", e->id, e->name.c_str(), gamepads_.connected());
            note_capability("input.gamepad", diagnostics::Support::Supported, e->name);
        } else if (e->type == input::GamepadEvent::Type::Removed) {
            WNK_LOGI("gamepad", "pad %d disconnected (%zu connected)", e->id, gamepads_.connected());
        } else if (!lifecycle_.renders()) {
            return;  // backgrounded: like keys, pad input is ignored
        }
        if (delegate_->on_gamepad(*this, *e)) return;
        for (const auto &a : pad_nav_.on_event(*e)) handle_action(a);
    }

    // One Luna request on a worker; the outcome is logged and handed to `done` on the main thread.
    void system_call(std::string what, std::function<platform::LunaReply()> call, std::function<void(bool)> done) {
        async::run_then_post<std::pair<bool, std::string>>(
            *shell_pool_, *dispatcher_, life_.weak(), {},
            [call = std::move(call)](const async::CancellationToken &) {
                const platform::LunaReply r = call();
                std::string why = r.detail;
                const bool ok = r.status == platform::LunaStatus::Ok && platform::reply_ok(r.payload, &why);
                if (r.status != platform::LunaStatus::Ok) why = std::string(platform::luna_status_name(r.status)) + ": " + r.detail;
                return std::make_pair(ok, why);
            },
            [what = std::move(what), done = std::move(done)](std::pair<bool, std::string> r) {
                if (r.first) {
                    WNK_LOGI("system", "%s: accepted", what.c_str());
                } else {
                    WNK_LOGW("system", "%s: %s", what.c_str(), r.second.c_str());
                }
                if (done) done(r.first);
            });
    }

    // Audio Guidance: follow the TV's setting, or {"wnkSpeak":true} in the launch parameters.
    void watch_audio_guidance() {
        const auto params = nlohmann::json::parse(launch_.params_json.empty() ? "{}" : launch_.params_json, nullptr, false);
        if (params.is_object() && params.value("wnkSpeak", false)) {
            set_speech(true, "launch parameters");
            return;
        }
        if constexpr (!platform::kIsWebOS) return;
        std::string err;
        guidance_sub_ = platform::subscribe_audio_guidance(
            [this, d = dispatcher_.get(), weak = life_.weak()](bool on) {
                d->post([this, weak, on] {
                    if (weak.alive()) set_speech(on, "Audio Guidance setting");
                });
            },
            &err);
        if (!guidance_sub_) caps_.set("accessibility.audio_guidance", diagnostics::Support::Unsupported, err);
    }

    void set_speech(bool on, const char *why) {
        if (!speech_known_) {
            speech_known_ = true;
            WNK_LOGI("speech", "%s: %s", why, on ? "on" : "off");
            note_capability("accessibility.audio_guidance", diagnostics::Support::Supported, on ? "on" : "off");
        }
        if (on == speech_) return;
        speech_ = on;
        WNK_LOGI("speech", "focus is %s (%s)", on ? "spoken" : "not spoken", why);
        note_capability("accessibility.audio_guidance", diagnostics::Support::Supported, on ? "on" : "off");
        if (on && ui_ && ui_->focused() != nullptr) say(ui_->focused()->accessible_text(), dynamic_cast<ui::TextField *>(ui_->focused()) == nullptr);
    }

    // The newest text wins: a request still queued when focus has moved on is dropped.
    // `loggable` is false for what the viewer typed (a user name, a search): it is spoken, not logged.
    void say(const std::string &text, bool loggable) {
        if (text.empty()) return;
        const uint64_t seq = ++speech_seq_;
        WNK_LOGI("speech", "say \"%s\"", loggable ? text.c_str() : "(text field)");
        auto latest = speech_latest_;
        latest->store(seq);
        const std::string language = catalog_.locale().tag();
        shell_pool_->submit([text, language, seq, latest](const async::CancellationToken &) {
            if (latest->load() != seq) return;
            const auto r = platform::speak(text, language, true);
            std::string why;
            if (r.status != platform::LunaStatus::Ok) {
                WNK_LOGW("speech", "tts: %s: %s", platform::luna_status_name(r.status), r.detail.c_str());
            } else if (!platform::reply_ok(r.payload, &why)) {
                WNK_LOGW("speech", "tts refused: %s", why.c_str());
            } else {
                WNK_LOGD("speech", "tts accepted");
            }
        });
    }

    // Network status: the connection manager's subscription on the TV, the interfaces on the host.
    void watch_network() {
        if constexpr (!platform::kIsWebOS) {
            set_network(platform::host_network_status());
            return;
        }
        std::string err;
        network_sub_ = platform::subscribe_network_status(
            [this, d = dispatcher_.get(), weak = life_.weak()](const platform::NetworkStatus &s) {
                d->post([this, weak, s] {
                    if (weak.alive()) set_network(s);
                });
            },
            &err);
        if (!network_sub_) {
            WNK_LOGW("network", "no connection status: %s", err.c_str());
            caps_.set("network.status", diagnostics::Support::Unsupported, err);
        }
    }

    void set_network(const platform::NetworkStatus &s) {
        if (s == network_) return;
        WNK_LOGI("network", "%s", s.summary().c_str());
        network_ = s;
        note_capability("network.status", diagnostics::Support::Supported, s.summary());
        if (delegate_ && ui_) delegate_->on_network(*this, s);
    }

    // {"event":"relaunch","parameters":{...},...} (registerApp, interface 2) or {"message":"relaunch",...}
    // (registerNativeApp, interface 1). The parameters replace the launch parameters.
    void on_relaunch_message(const char *message) {
        const auto j = nlohmann::json::parse(message, nullptr, false);
        nlohmann::json params = nlohmann::json::object();
        if (j.is_object() && j.contains("parameters") && j["parameters"].is_object()) params = j["parameters"];
        // Parameter values can be personal (a search, a link): log names only.
        auto names = [](const nlohmann::json &o) {
            std::string out;
            if (!o.is_object()) return out;
            for (auto it = o.begin(); it != o.end(); ++it) out += (out.empty() ? "" : ",") + it.key();
            return out;
        };
        WNK_LOGI("lifecycle", "relaunch: message fields [%s], parameters [%s]", names(j).c_str(), names(params).c_str());
        launch_.params_json = params.dump();
        if (delegate_ && ui_) delegate_->on_relaunch(*this, launch_.params_json);
    }

    // SDL text input follows the Ui: on while a view edits (on the TV this shows LG's keyboard), off
    // otherwise. The viewer can close LG's keyboard, which turns SDL text input off underneath us.
    void sync_text_input() {
        ui::View *v = lifecycle_.renders() ? ui_->text_input_view() : nullptr;
        if (v != nullptr && !text_input_on_) {
            const Rect r = v->screen_rect();
            const Point a = renderer_->mapping().to_pixels({r.x, r.y});
            const Point b = renderer_->mapping().to_pixels({r.x + r.w, r.y + r.h});
            int ww = 0, wh = 0, dw = 0, dh = 0;
            SDL_GetWindowSize(window_, &ww, &wh);
            SDL_GL_GetDrawableSize(window_, &dw, &dh);
            const float sx = static_cast<float>(ww) / std::max(1, dw), sy = static_cast<float>(wh) / std::max(1, dh);
            SDL_Rect rect{static_cast<int>(a.x * sx), static_cast<int>(a.y * sy), static_cast<int>((b.x - a.x) * sx),
                          static_cast<int>((b.y - a.y) * sy)};
            SDL_SetTextInputRect(&rect);
            SDL_StartTextInput();
            text_input_on_ = true;
            WNK_LOGI("text", "text input on for %s (screen keyboard %s)", v->name.empty() ? "a view" : v->name.c_str(),
                     SDL_HasScreenKeyboardSupport() ? "supported" : "not supported");
        } else if (v == nullptr && text_input_on_) {
            SDL_StopTextInput();
            text_input_on_ = false;
            WNK_LOGI("text", "text input off");
        } else if (v != nullptr && !SDL_IsTextInputActive()) {
            text_input_on_ = false;
            WNK_LOGI("text", "keyboard closed by the system: editing ends");
            ui_->text_input_closed();
        }
    }

    void draw_overlay() {
        OverlayData d;
        d.frames = &metrics_;
        d.render = renderer_->stats();
        d.ui = ui_->stats();
        if (images_) d.images = images_->stats();
        d.pool = pool_->stats();
        d.memory = diagnostics::process_memory();
        d.state = state_name(lifecycle_.state());
        draw_metrics_overlay(*renderer_, ui_->theme(), ui_->canvas(), d);
    }

    void handle_action(const input::ActionEvent &a) {
        if (!lifecycle_.renders() && a.phase != input::Phase::Release) return;  // backgrounded: ignore
        if (a.action == config_.metrics_toggle && a.action != input::Action::Unknown) {
            if (a.phase == input::Phase::Press) set_metrics_overlay(!overlay_);
            return;
        }
        if (ui_->handle(a)) return;
        if (delegate_->on_action(*this, a)) return;
        if (a.phase != input::Phase::Press) return;
        if (a.action == input::Action::Exit) {
            quit("Exit key");
        } else if (a.action == input::Action::Back && config_.double_back_exit) {
            const Uint32 now = a.source.time_ms;
            if (last_back_ != 0 && now - last_back_ <= kDoubleBackMs) {
                WNK_LOGI(kCat, "Back twice at root: quitting");
                quit("double Back");
                return;
            }
            last_back_ = now;
            WNK_LOGI(kCat, "Back at root: asking for a second Back");
            ui_->toast(catalog_.tr("Press Back again to exit"), kDoubleBackMs);
        }
    }

    void on_pointer(const input::PointerEvent &p) {
        using T = input::PointerEvent::Type;
        if (!lifecycle_.renders()) return;
        const bool hit = ui_->handle_pointer(p);
        if (p.type == T::Move || p.type == T::Press || p.type == T::Wheel) {
            note_capability("input.pointer", diagnostics::Support::Supported,
                            std::string("first event: ") + input::pointer_type_name(p.type));
        }
        if (p.type == T::Move) {
            // Motion arrives at the remote's report rate: log at most once a second.
            if (p.time_ms - last_move_log_ < 1000 && moves_logged_ > 0) return;
            last_move_log_ = p.time_ms;
            ++moves_logged_;
        }
        const Point l = ui_->pointer_position();
        WNK_LOGD("pointer", "%s px=(%.0f,%.0f) logical=(%.0f,%.0f) button=%d wheel=%.1f hit=%d",
                 input::pointer_type_name(p.type), p.x, p.y, l.x, l.y, p.button, p.wheel, hit ? 1 : 0);
    }

    void log_focus_change() {
        ui::View *f = ui_->focused();
        const ui::ViewId id = f != nullptr ? f->id() : 0;
        if (id == last_focus_) return;
        last_focus_ = id;
        if (speech_ && f != nullptr) say(f->accessible_text(), dynamic_cast<ui::TextField *>(f) == nullptr);
        if (f == nullptr) {
            WNK_LOGI("focus", "focus <none>");
        } else if (!f->name.empty()) {
            WNK_LOGI("focus", "focus %s", f->name.c_str());
        } else {
            WNK_LOGI("focus", "focus #%u", f->id());
        }
    }

    // Once a minute, whatever the screen does (a still screen presents nothing and logs no perf lines):
    // memory and frame counts for long runs (tools/soak.sh).
    void heartbeat(uint64_t frames, uint64_t drawn) {
        const Uint32 now = SDL_GetTicks();
        if (now - last_heartbeat_ < 60000) return;
        last_heartbeat_ = now;
        const auto mem = diagnostics::process_memory();
        WNK_LOGI("heartbeat", "uptime %u s, %s, rss %.1f MB, presented %llu, drawn %llu, views %zu", now / 1000,
                 state_name(lifecycle_.state()), mem.rss_bytes / 1048576.0, static_cast<unsigned long long>(frames),
                 static_cast<unsigned long long>(drawn), ui_->stats().views);
    }

    void log_perf(uint64_t frames, uint64_t drawn) {
        const auto s = metrics_.summary(300);
        const auto rs = renderer_->stats();
        const auto us = ui_->stats();
        const auto mem = diagnostics::process_memory();
        WNK_LOGI("perf", "frames %llu presented of %llu drawn: interval avg %.2f median %.2f p95 %.2f max %.2f ms, late %zu; max ms events %.2f "
                 "update %.2f draw %.2f finish %.2f swap %.2f; draw calls %u, quads %u, textures %u, views %zu, "
                 "rss %.1f MB",
                 static_cast<unsigned long long>(frames), static_cast<unsigned long long>(drawn), s.interval_avg, s.interval_median, s.interval_p95, s.interval_max, s.late,
                 s.events_max, s.update_max, s.draw_max, s.finish_max, s.swap_max, rs.draw_calls, rs.quads,
                 rs.textures, us.views, mem.rss_bytes / 1048576.0);
    }

    // The fixed teardown order from the header comment, each step timed, under the watchdog.
    void shutdown() {
        const Uint64 t0 = SDL_GetPerformanceCounter();
        lifecycle_.dispatch(Event::Quit, SDL_GetTicks());
        const char *step = "start";
        Watchdog watchdog;
        watchdog.arm(config_.shutdown_deadline_ms, &step);
        auto run_step = [&step](const char *name, const std::function<void()> &fn) {
            step = name;
            const Uint64 t = SDL_GetPerformanceCounter();
            fn();
            WNK_LOGI("shutdown", "%s: %.1f ms", name, ms_since(t));
        };
        if (delegate_ && ui_) run_step("delegate on_stop", [this] { delegate_->on_stop(*this); });
        run_step("write capabilities", [this] { write_capabilities(); });
        if (network_sub_) network_sub_->close();
        if (guidance_sub_) guidance_sub_->close();
        if (dispatcher_) run_step("close dispatcher", [this] {
            dispatcher_->close();
            dispatcher_->set_wake(nullptr);
        });
        if (pool_) {
            run_step("join workers", [this] {
                const int budget = std::max(200, config_.shutdown_deadline_ms - 1000);
                if (!pool_->shutdown(std::chrono::milliseconds(budget))) WNK_LOGW("shutdown", "a worker ignored cancellation");
                if (!shell_pool_->shutdown(std::chrono::milliseconds(200))) WNK_LOGW("shutdown", "a shell worker ignored cancellation");
            });
        }
        run_step("destroy UI", [this] {
            delegate_.reset();
            ui_.reset();
            image_source_.reset();
        });
        if (audio_out_) run_step("close audio", [this] {
            const float peak = mixer_->peak();
            audio_out_.reset();  // stops the callback before the mixer goes
            WNK_LOGI("audio", "closed; peak level %.2f", static_cast<double>(peak));
        });
        run_step("release images", [this] {
            textures_.reset();
            images_.reset();
            http_.reset();
            store_.reset();
        });
        run_step("destroy renderer", [this] { renderer_.reset(); });  // GL objects, context still current
        run_step("destroy window", [this] {
            if (gl_ != nullptr) SDL_GL_DeleteContext(gl_);
            if (window_ != nullptr) SDL_DestroyWindow(window_);
            gl_ = nullptr;
            window_ = nullptr;
        });
        if (wake_locks_) wake_locks_->detach();  // locks still held elsewhere must not call SDL after this
        gamepads_.close_all();
        if (sdl_) run_step("SDL_Quit", [] { SDL_Quit(); });
        pool_.reset();
        shell_pool_.reset();
        dispatcher_.reset();
        watchdog.cancel();
        lifecycle_.dispatch(Event::Stopped, SDL_GetTicks());
        WNK_LOGI(kCat, "shutdown complete in %.1f ms", ms_since(t0));
    }

    const AppConfig &config_;
    platform::LaunchInfo launch_;
    std::string runtime_;
    input::ActionMapper mapper_;
    Lifecycle lifecycle_;
    diagnostics::FrameMetrics metrics_;
    diagnostics::Capabilities caps_;
    diagnostics::DeviceInfo device_;
    bool sdl_ = false;
    bool quit_requested_ = false;
    bool text_input_on_ = false;  // SDL text input started for a view
    Uint32 last_heartbeat_ = 0;
    Uint32 wake_event_ = static_cast<Uint32>(-1);
    char *(*take_relaunch_)() = nullptr;
    // System font fallback.
    std::shared_ptr<render::FontFinder> font_finder_;  // lists the font directories once
    std::vector<char32_t> glyphs_wanted_;
    bool font_search_running_ = false;
    std::vector<std::string> font_preferences_;          // file-name tags for the UI language
    std::set<std::string> loaded_fallbacks_;
    std::map<std::string, std::unique_ptr<render::FontFace>> path_faces_;
    platform::NetworkStatus network_;
    std::unique_ptr<platform::LunaSubscription> guidance_sub_;
    bool speech_ = false;
    bool speech_known_ = false;
    uint64_t speech_seq_ = 0;
    std::shared_ptr<std::atomic<uint64_t>> speech_latest_ = std::make_shared<std::atomic<uint64_t>>(0);
    std::unique_ptr<platform::LunaSubscription> network_sub_;
    i18n::Catalog catalog_;
    i18n::Locale formats_;
    async::Lifetime life_;  // results posted to the shell
    std::atomic<bool> wake_pending_{false};
    SDL_Window *window_ = nullptr;
    SDL_GLContext gl_ = nullptr;
    std::unique_ptr<render::Gles2Renderer> renderer_;
    std::unique_ptr<async::MainDispatcher> dispatcher_;
    std::unique_ptr<async::WorkerPool> pool_;
    std::unique_ptr<async::WorkerPool> shell_pool_;  // font search, Luna requests, speech
    std::shared_ptr<net::HttpTransport> http_;
    std::unique_ptr<storage::KeyValueStore> store_;
    std::unique_ptr<image::ImageService> images_;
    std::unique_ptr<image::ImageTextures> textures_;
    std::unique_ptr<ui::ServiceImageSource> image_source_;
    std::unique_ptr<ui::Ui> ui_;
    std::unique_ptr<WakeLocks> wake_locks_;
    input::SdlGamepads gamepads_;
    input::GamepadNavigator pad_nav_;
    std::unique_ptr<audio::Mixer> mixer_;
    std::unique_ptr<audio::SdlOutput> audio_out_;
    std::unique_ptr<Delegate> delegate_;
    Uint32 last_back_ = 0;
    Uint32 last_move_log_ = 0;
    int moves_logged_ = 0;
    ui::ViewId last_focus_ = 0;
    std::string crash_test_;
    bool overlay_ = false;
};

}  // namespace

int run(int argc, char **argv, const AppConfig &config, const DelegateFactory &factory) {
    platform::prepare_process_environment();
    platform::LaunchInfo launch = platform::read_launch_info(argc, argv, config.fallback_app_id);
    std::string runtime = platform::runtime_dir(launch.app_id);

    log::Logger logger(log::Level::Debug);
    logger.add_sink(std::make_unique<log::StderrSink>());
    if (!runtime.empty()) {
        const int fd = log::open_log_file(runtime + "/events.log", log::OpenMode::Truncate);
        if (fd >= 0) logger.add_sink(std::make_unique<log::FileSink>(fd));
    }
    log::set_default_logger(&logger);
    WNK_LOGI(kCat, "start app_id=%s platform=%s params=%s", launch.app_id.c_str(), platform::kIsWebOS ? "webos" : "host",
             launch.params_json.empty() ? "<none>" : launch.params_json.c_str());

    int code = 0;
    {
        Shell shell(config, std::move(launch), std::move(runtime));
        code = shell.run(factory);
    }
    WNK_LOGI(kCat, "exit %d", code);
    logger.flush();
    log::set_default_logger(nullptr);
    return code;
}

}  // namespace wnk::app
