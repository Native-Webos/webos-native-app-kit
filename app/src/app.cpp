// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/app/app.hpp"

#include "wnk/app/overlay.hpp"
#include "wnk/diagnostics/crash.hpp"
#include "wnk/image/textures.hpp"
#include "wnk/input/sdl_input.hpp"
#include "wnk/log.hpp"
#include "wnk/render/gles2_renderer.hpp"
#include "wnk/ui/image_source.hpp"

#include <SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <mutex>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wnk::app {

bool Delegate::on_action(Context &, const input::ActionEvent &) { return false; }
void Delegate::on_lifecycle(Context &, const Lifecycle::Transition &) {}
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
// bg / fg (the SDL app background/foreground event pairs SDL-webOS sends). <prefix>_SHOT saves the
// last drawn frame as BMP. The app quits when the script ends.
class Script {
 public:
    explicit Script(const std::string &prefix) {
        if (const char *keys = SDL_getenv((prefix + "_KEYS").c_str())) parse(keys);
        if (const char *shot = SDL_getenv((prefix + "_SHOT").c_str())) shot_ = shot;
    }
    bool active() const { return !steps_.empty() || !shot_.empty(); }
    // Returns true when the script has finished (the app should quit).
    bool tick(uint64_t iteration, render::Gles2Renderer &r, SDL_Window *window) {
        if (!active()) return false;
        const uint64_t first = 20, gap = 6;
        if (iteration >= first && next_ < steps_.size() && (iteration - first) % gap == 0) run(steps_[next_++], r, window);
        return next_ >= steps_.size() && iteration >= first + steps_.size() * gap + 60;
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
        enum Kind { Key, Move, Click, Wheel, Background, Foreground } kind = Key;
        SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
        float x = 0, y = 0;
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
            } else if (std::sscanf(name.c_str(), "wheel@%f", &st.y) == 1) {
                st.kind = Step::Wheel;
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
    static void run(const Step &st, render::Gles2Renderer &r, SDL_Window *window) {
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
    std::string shot_;
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
    const platform::LaunchInfo &launch() override { return launch_; }
    const std::string &runtime_dir() override { return runtime_; }
    State state() const override { return lifecycle_.state(); }
    const diagnostics::FrameMetrics &frame_metrics() const override { return metrics_; }
    diagnostics::Capabilities &capabilities() override { return caps_; }
    const diagnostics::DeviceInfo &device() const override { return device_; }
    void set_metrics_overlay(bool visible) override {
        if (visible != overlay_) WNK_LOGI(kCat, "metrics overlay %s", visible ? "on" : "off");
        overlay_ = visible;
    }
    bool metrics_overlay() const override { return overlay_; }
    void quit(const char *reason) override {
        if (!quit_requested_) WNK_LOGI(kCat, "quit requested: %s", reason);
        quit_requested_ = true;
    }

    int run(const DelegateFactory &factory) {
        setup_crash_record();
        lifecycle_.set_listener([this](const Lifecycle::Transition &t) {
            WNK_LOGI("lifecycle", "%s -> %s (%s)", state_name(t.from), state_name(t.to), event_name(t.event));
            diagnostics::set_crash_state(state_name(t.to));
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
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return fail("SDL video init");
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

        dispatcher_ = std::make_unique<async::MainDispatcher>();
        pool_ = std::make_unique<async::WorkerPool>(config_.worker_threads);
        if (config_.http) {
            http_ = net::make_curl_transport({}, &err);
            if (!http_) WNK_LOGW("net", "no HTTP transport: %s", err.c_str());
        }
        if (config_.storage) {
            store_ = storage::open_default_store(launch_.app_id, &err);
            if (!store_) WNK_LOGW("storage", "key/value store unavailable: %s", err.c_str());
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
        probe_capabilities(mode, gpu);
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
        caps_.set("images.decode", config_.images ? Support::Supported : Support::Unknown, config_.images ? "PNG, JPEG" : "not requested");
        caps_.set("input.pointer", Support::Unknown, "no pointer event yet");
        caps_.set("lifecycle.background", Support::Unknown, "no background event yet");
        caps_.set("media.playback", Support::Unknown, "not implemented in the kit yet", "not-implemented");
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
        uint64_t iteration = 0, frames = 0;
        int dw = 0, dh = 0;
        while (!quit_requested_) {
            const Uint64 t_events = SDL_GetPerformanceCounter();
            SDL_Event ev;
            // Backgrounded: draw nothing, wake for events (and worker results) a few times a second.
            if (!lifecycle_.renders() && SDL_WaitEventTimeout(&ev, script.active() ? 16 : 250)) on_event(ev);
            while (!quit_requested_ && SDL_PollEvent(&ev)) on_event(ev);
            if (quit_requested_) break;
            const float events_ms = static_cast<float>(ms_since(t_events));

            dispatcher_->drain(32, std::chrono::milliseconds(4));
            const bool script_done = script.tick(iteration++, *renderer_, window_);
            if (script_done && !lifecycle_.renders()) {
                quit("script finished");
                break;
            }
            if (!lifecycle_.renders()) {
                prev = SDL_GetPerformanceCounter();  // no catch-up jump in animations after resuming
                continue;
            }

            const Uint64 now = SDL_GetPerformanceCounter();
            const float dt = static_cast<float>((now - prev) / freq);
            prev = now;
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
            const float finish_ms = static_cast<float>(ms_since(t_finish));
            if (script_done) {
                script.save(*renderer_, dw, dh);  // before the swap, while the frame is still readable
                quit("script finished");
            }
            const Uint64 t_swap = SDL_GetPerformanceCounter();
            SDL_GL_SwapWindow(window_);
            const float swap_ms = static_cast<float>(ms_since(t_swap));
            if (++frames == 1) WNK_LOGI(kCat, "first frame: drawable %dx%d", dw, dh);
            if (lifecycle_.state() == State::Resuming) lifecycle_.dispatch(Event::FramePresented, SDL_GetTicks());

            if (frames > 1) metrics_.add({dt * 1000.0f, events_ms, update_ms, draw_ms, finish_ms, swap_ms});
            if (!crash_test_.empty() && frames == 180) {
                WNK_LOGW("crash", "crash test: triggering %s", crash_test_.c_str());
                if (auto *l = log::default_logger()) l->flush();
                crash_now(crash_test_);
            }
            if (frames % 300 == 0) log_perf(frames);

            // If the swap did not block on vsync, sleep out the rest of a 60 Hz frame.
            const double frame_ms = ms_since(now);
            if (frame_ms < 4.0) SDL_Delay(static_cast<Uint32>(1000.0 / 60.0 - frame_ms));
        }
        WNK_LOGI(kCat, "main loop ended after %llu frames", static_cast<unsigned long long>(frames));
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
            default: break;
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
        lifecycle_.dispatch(Event::Background, SDL_GetTicks());
    }

    void release_keys() {
        for (const auto &a : mapper_.release_all(SDL_GetTicks())) handle_action(a);
    }

    void on_key(const SDL_Event &ev) {
        auto key = input::key_event_from_sdl(ev);
        if (!key) return;
        if (key->down) {
            const auto *b = mapper_.keymap().find(key->scancode, key->keycode);
            WNK_LOGD("key", "down sc=%d kc=0x%x repeat=%d action=%s pointer=%d", key->scancode, key->keycode,
                     key->repeat ? 1 : 0, b != nullptr ? input::action_name(b->action) : "unmapped",
                     ui_->pointer_active() ? 1 : 0);
        }
        if (auto a = mapper_.map(*key)) handle_action(*a);
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
            ui_->toast("Press Back again to exit", kDoubleBackMs);
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
        if (f == nullptr) {
            WNK_LOGI("focus", "focus <none>");
        } else if (!f->name.empty()) {
            WNK_LOGI("focus", "focus %s", f->name.c_str());
        } else {
            WNK_LOGI("focus", "focus #%u", f->id());
        }
    }

    void log_perf(uint64_t frames) {
        const auto s = metrics_.summary(300);
        const auto rs = renderer_->stats();
        const auto us = ui_->stats();
        const auto mem = diagnostics::process_memory();
        WNK_LOGI("perf", "frames %llu: interval avg %.2f median %.2f p95 %.2f max %.2f ms, late %zu; max ms events %.2f "
                 "update %.2f draw %.2f finish %.2f swap %.2f; draw calls %u, quads %u, textures %u, views %zu, "
                 "rss %.1f MB",
                 static_cast<unsigned long long>(frames), s.interval_avg, s.interval_median, s.interval_p95, s.interval_max, s.late,
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
        if (dispatcher_) run_step("close dispatcher", [this] { dispatcher_->close(); });
        if (pool_) {
            run_step("join workers", [this] {
                const int budget = std::max(200, config_.shutdown_deadline_ms - 1000);
                if (!pool_->shutdown(std::chrono::milliseconds(budget))) WNK_LOGW("shutdown", "a worker ignored cancellation");
            });
        }
        run_step("destroy UI", [this] {
            delegate_.reset();
            ui_.reset();
            image_source_.reset();
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
        if (sdl_) run_step("SDL_Quit", [] { SDL_Quit(); });
        pool_.reset();
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
    SDL_Window *window_ = nullptr;
    SDL_GLContext gl_ = nullptr;
    std::unique_ptr<render::Gles2Renderer> renderer_;
    std::unique_ptr<async::MainDispatcher> dispatcher_;
    std::unique_ptr<async::WorkerPool> pool_;
    std::shared_ptr<net::HttpTransport> http_;
    std::unique_ptr<storage::KeyValueStore> store_;
    std::unique_ptr<image::ImageService> images_;
    std::unique_ptr<image::ImageTextures> textures_;
    std::unique_ptr<ui::ServiceImageSource> image_source_;
    std::unique_ptr<ui::Ui> ui_;
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
