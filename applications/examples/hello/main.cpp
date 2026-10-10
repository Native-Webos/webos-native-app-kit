// SPDX-License-Identifier: GPL-3.0-or-later
//
// Example A (Milestone 2): the smallest complete native app on the kit.
//
// What it demonstrates and records:
//   * process setup, launch parameters and a safe log file in the runtime directory;
//   * a system probe (release, SoC, ids, environment, which storage locations are writable);
//   * SDL window + OpenGL ES 2 context, GL capability strings, panel resolution;
//   * every key event logged with scancode/keycode/repeat, which is the key-capture experiment
//     from docs/research/PACKAGING-AND-FIRMWARE.md E.3 for the SDL build we actually ship;
//   * lifecycle events (background/foreground, focus, close) and an orderly shutdown.
//
// Quit: the Exit key, Back twice within 1.5 s (a single Back only flashes the screen so it can be
// captured), window close, or SIGTERM/SIGINT (SDL turns those into SDL_QUIT).
#include "wnk/log.hpp"
#include "wnk/platform/probe.hpp"
#include "wnk/platform/process.hpp"

#include <SDL.h>
#include <SDL_opengles2.h>

#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace {

constexpr const char *kFallbackAppId = "org.wnk.examples.hello";
constexpr const char *kCat = "hello";
constexpr Uint32 kDoubleBackMs = 1500;

// Scancodes of the webOS-specific remote keys in webosbrew SDL-webOS 2.x (include/SDL_scancode.h).
// These numbers are NOT valid for the TV's system SDL or for SDL3 (docs/research/REMOTE-INPUT.md).
constexpr int kWebOSBack = 482;
constexpr int kWebOSExit = 505;
#if defined(WNK_SDL_WEBOS) && defined(SDL_WEBOS_SCANCODE_BACK)
static_assert(SDL_WEBOS_SCANCODE_BACK == kWebOSBack, "SDL-webOS Back scancode changed");
#endif

bool is_back(const SDL_Keysym &k) {
    return k.scancode == kWebOSBack || k.scancode == SDL_SCANCODE_ESCAPE || k.scancode == SDL_SCANCODE_AC_BACK;
}

bool is_exit(const SDL_Keysym &k) { return k.scancode == kWebOSExit; }

const char *app_event_name(Uint32 type) {
    switch (type) {
        case SDL_APP_TERMINATING: return "APP_TERMINATING";
        case SDL_APP_LOWMEMORY: return "APP_LOWMEMORY";
        case SDL_APP_WILLENTERBACKGROUND: return "APP_WILLENTERBACKGROUND";
        case SDL_APP_DIDENTERBACKGROUND: return "APP_DIDENTERBACKGROUND";
        case SDL_APP_WILLENTERFOREGROUND: return "APP_WILLENTERFOREGROUND";
        case SDL_APP_DIDENTERFOREGROUND: return "APP_DIDENTERFOREGROUND";
        default: return nullptr;
    }
}

const char *window_event_name(Uint8 e) {
    switch (e) {
        case SDL_WINDOWEVENT_SHOWN: return "SHOWN";
        case SDL_WINDOWEVENT_HIDDEN: return "HIDDEN";
        case SDL_WINDOWEVENT_EXPOSED: return "EXPOSED";
        case SDL_WINDOWEVENT_MOVED: return "MOVED";
        case SDL_WINDOWEVENT_RESIZED: return "RESIZED";
        case SDL_WINDOWEVENT_SIZE_CHANGED: return "SIZE_CHANGED";
        case SDL_WINDOWEVENT_MINIMIZED: return "MINIMIZED";
        case SDL_WINDOWEVENT_MAXIMIZED: return "MAXIMIZED";
        case SDL_WINDOWEVENT_RESTORED: return "RESTORED";
        case SDL_WINDOWEVENT_ENTER: return "ENTER";
        case SDL_WINDOWEVENT_LEAVE: return "LEAVE";
        case SDL_WINDOWEVENT_FOCUS_GAINED: return "FOCUS_GAINED";
        case SDL_WINDOWEVENT_FOCUS_LOST: return "FOCUS_LOST";
        case SDL_WINDOWEVENT_CLOSE: return "CLOSE";
        default: return "OTHER";
    }
}

// webOS-only SDL entry points are resolved at runtime, so the same code runs against any SDL and
// the log says whether the loaded SDL has them.
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

void log_gl_info() {
    auto str = [](GLenum name) {
        const GLubyte *s = glGetString(name);
        return s != nullptr ? reinterpret_cast<const char *>(s) : "<null>";
    };
    WNK_LOGI("gl", "GL_VENDOR=%s", str(GL_VENDOR));
    WNK_LOGI("gl", "GL_RENDERER=%s", str(GL_RENDERER));
    WNK_LOGI("gl", "GL_VERSION=%s", str(GL_VERSION));
    WNK_LOGI("gl", "GL_SHADING_LANGUAGE_VERSION=%s", str(GL_SHADING_LANGUAGE_VERSION));
    GLint max_tex = 0, max_rb = 0, max_units = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_rb);
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &max_units);
    WNK_LOGI("gl", "GL_MAX_TEXTURE_SIZE=%d GL_MAX_RENDERBUFFER_SIZE=%d GL_MAX_TEXTURE_IMAGE_UNITS=%d", max_tex,
             max_rb, max_units);
    GLint range[2] = {0, 0}, precision = 0;
    glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range, &precision);
    WNK_LOGI("gl", "fragment highp float: range=[%d,%d] precision=%d", range[0], range[1], precision);
    // Extensions can be long; log them in chunks so no single line is truncated.
    std::string ext = str(GL_EXTENSIONS);
    for (size_t pos = 0; pos < ext.size(); pos += 400) {
        WNK_LOGI("gl", "GL_EXTENSIONS[%zu]=%s", pos / 400, ext.substr(pos, 400).c_str());
    }
}

void log_probe(const wnk::platform::LaunchInfo &launch, const std::string &runtime) {
    for (const auto &[key, value] : wnk::platform::collect_system_facts()) {
        WNK_LOGI("probe", "%s=%s", key.c_str(), value.c_str());
    }
    for (const auto &c : wnk::platform::storage_candidates(launch.app_id, launch.app_dir, runtime)) {
        auto r = wnk::platform::probe_writable(c);
        WNK_LOGI("probe", "storage %-7s %-60s writable=%d (%s) class=%s [%s]", c.purpose.c_str(),
                 c.directory.c_str(), r.writable ? 1 : 0, r.detail.c_str(),
                 wnk::platform::durability_name(c.durability), c.evidence.c_str());
    }
}

struct Color {
    float r, g, b;
};

// A distinct colour per scancode, so a key press is visible without text rendering (M3).
Color color_for(int scancode) {
    unsigned h = static_cast<unsigned>(scancode) * 2654435761u;
    return {0.25f + ((h >> 8) & 0xff) / 512.0f, 0.25f + ((h >> 16) & 0xff) / 512.0f,
            0.25f + ((h >> 24) & 0xff) / 512.0f};
}

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
    // The access-policy hints must be set before the window exists (moonlight-tv src/app/app.c).
    // Hint names are plain strings, so they are harmless on an SDL that does not know them.
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
    } else {
        WNK_LOGW("display", "SDL_GetCurrentDisplayMode failed: %s", SDL_GetError());
    }
    log_webos_display_info();

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    // On webOS the window must be SDL_WINDOW_FULLSCREEN (not FULLSCREEN_DESKTOP) at the display mode
    // size (webosbrew samples sdl_shell.c). On the host a 1280x720 window is easier to work with.
    Uint32 flags = SDL_WINDOW_OPENGL;
    int win_w = 1280, win_h = 720;
    if constexpr (wnk::platform::kIsWebOS) {
        flags |= SDL_WINDOW_FULLSCREEN;
        if (mode.w > 0 && mode.h > 0) {
            win_w = mode.w;
            win_h = mode.h;
        } else {
            win_w = 1920;
            win_h = 1080;
        }
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
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(window, &dw, &dh);
    WNK_LOGI("display", "window %dx%d, drawable %dx%d", win_w, win_h, dw, dh);
    log_gl_info();
    logger.flush();

    bool running = true;
    Uint32 last_back_ms = 0;
    Color flash{0.0f, 0.0f, 0.0f};
    Uint32 flash_until = 0;
    const Color idle{0.08f, 0.10f, 0.16f};
    Uint64 frames = 0;
    constexpr double kTargetFrameMs = 1000.0 / 60.0;
    constexpr double kMinFrameMs = 4.0;  // a swap that returns faster than this did not wait for vsync
    const double perf_freq = static_cast<double>(SDL_GetPerformanceFrequency());
    Uint64 frame_start = SDL_GetPerformanceCounter();
    bool pacing_logged = false;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT:
                    WNK_LOGI(kCat, "SDL_QUIT");
                    running = false;
                    break;
                case SDL_KEYDOWN:
                case SDL_KEYUP: {
                    const SDL_Keysym &k = ev.key.keysym;
                    const bool down = ev.type == SDL_KEYDOWN;
                    WNK_LOGI("key", "%s scancode=%d keycode=%d (0x%x) name='%s' repeat=%d mod=0x%x",
                             down ? "down" : "up  ", static_cast<int>(k.scancode), static_cast<int>(k.sym),
                             static_cast<unsigned>(k.sym), SDL_GetScancodeName(k.scancode), ev.key.repeat,
                             static_cast<unsigned>(k.mod));
                    if (!down || ev.key.repeat) break;
                    flash = color_for(k.scancode);
                    flash_until = SDL_GetTicks() + 400;
                    if (is_exit(k)) {
                        WNK_LOGI(kCat, "Exit key: quitting");
                        running = false;
                    } else if (is_back(k)) {
                        const Uint32 now = SDL_GetTicks();
                        if (last_back_ms != 0 && now - last_back_ms <= kDoubleBackMs) {
                            WNK_LOGI(kCat, "Back twice: quitting");
                            running = false;
                        } else {
                            last_back_ms = now;
                            WNK_LOGI(kCat, "Back once: press again within %u ms to quit", kDoubleBackMs);
                        }
                    }
                    break;
                }
                case SDL_TEXTINPUT:
                    WNK_LOGI("key", "text '%s'", ev.text.text);
                    break;
                case SDL_MOUSEMOTION:
                    break;  // too chatty to log; buttons are enough to show pointer delivery
                case SDL_MOUSEBUTTONDOWN:
                    WNK_LOGI("pointer", "button %d down at %d,%d", ev.button.button, ev.button.x, ev.button.y);
                    break;
                case SDL_WINDOWEVENT:
                    WNK_LOGI("window", "%s %d %d", window_event_name(ev.window.event), ev.window.data1,
                             ev.window.data2);
                    if (ev.window.event == SDL_WINDOWEVENT_CLOSE) running = false;
                    break;
                default:
                    if (const char *name = app_event_name(ev.type)) WNK_LOGI("lifecycle", "%s", name);
                    break;
            }
        }

        const bool flashing = SDL_GetTicks() < flash_until;
        const Color c = flashing ? flash : idle;
        SDL_GL_GetDrawableSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(c.r, c.g, c.b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapWindow(window);
        ++frames;
        if (frames == 1) WNK_LOGI(kCat, "first frame presented");

        // Frame pacing fallback: if the swap did not block on vsync (unsupported, or a driver that
        // returns immediately), sleep out the rest of a 60 Hz frame instead of spinning a core.
        const Uint64 now = SDL_GetPerformanceCounter();
        const double frame_ms = static_cast<double>(now - frame_start) * 1000.0 / perf_freq;
        if (frame_ms < kMinFrameMs) {
            if (!pacing_logged) {
                WNK_LOGI(kCat, "swap returned after %.2f ms: vsync is not blocking, pacing to 60 Hz", frame_ms);
                pacing_logged = true;
            }
            SDL_Delay(static_cast<Uint32>(kTargetFrameMs - frame_ms));
        }
        frame_start = SDL_GetPerformanceCounter();
    }

    WNK_LOGI(kCat, "shutting down after %llu frames", static_cast<unsigned long long>(frames));
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    WNK_LOGI(kCat, "shutdown complete");
    logger.flush();
    wnk::log::set_default_logger(nullptr);
    return 0;
}
