// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/ndl_backend.hpp"

#include "wnk/diagnostics/crash.hpp"
#include "wnk/log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include <dlfcn.h>

#include "ndl_abi.h"

namespace wnk::media {
namespace {

constexpr const char *kCat = "ndl";
constexpr const char *kLib = "libNDL_directmedia.so.1";
// After a splice the new content starts this far after the last PTS fed.
constexpr int64_t kSpliceGapUs = 200000;

struct Api {
    void *handle = nullptr;
    int (*init)(const char *, wnk_ndl_resource_released) = nullptr;
    int (*quit)() = nullptr;
    int (*set_app_state)(int) = nullptr;
    int (*load)(wnk_ndl_data_info *, wnk_ndl_load_callback) = nullptr;
    int (*unload)() = nullptr;
    int (*video_play)(void *, unsigned int, long long) = nullptr;
    int (*audio_play)(void *, unsigned int, long long) = nullptr;
    int (*set_area)(int, int, int, int) = nullptr;
    const char *(*get_error)() = nullptr;
    // Optional (webOS 5.3.1+ per the firmware inventory), for logs only.
    int (*render_length)(int *) = nullptr;
    std::string why;
};

std::mutex g_mutex;  // every NDL call
Api g_api;
bool g_tried = false;
bool g_initialized = false;
int g_backends = 0;
bool g_foreground = true;
std::atomic<NdlBackend *> g_active{nullptr};  // receives load-callback events

template <typename F>
bool sym(F &out, const char *name) {
    out = reinterpret_cast<F>(dlsym(g_api.handle, name));
    if (out == nullptr) g_api.why = std::string("missing ") + name;
    return out != nullptr;
}

// Lock held.
bool load_api() {
    if (g_tried) return g_api.handle != nullptr && g_api.why.empty();
    g_tried = true;
    g_api.handle = dlopen(kLib, RTLD_NOW | RTLD_LOCAL);
    if (g_api.handle == nullptr) {
        const char *e = dlerror();
        g_api.why = e != nullptr ? e : "dlopen failed";
        return false;
    }
    const bool ok = sym(g_api.init, "NDL_DirectMediaInit") && sym(g_api.quit, "NDL_DirectMediaQuit") &&
                    sym(g_api.set_app_state, "NDL_DirectMediaSetAppState") && sym(g_api.load, "NDL_DirectMediaLoad") &&
                    sym(g_api.unload, "NDL_DirectMediaUnload") && sym(g_api.video_play, "NDL_DirectVideoPlay") &&
                    sym(g_api.audio_play, "NDL_DirectAudioPlay") && sym(g_api.set_area, "NDL_DirectVideoSetArea") &&
                    sym(g_api.get_error, "NDL_DirectMediaGetError");
    if (!ok) {
        dlclose(g_api.handle);
        g_api.handle = nullptr;
        return false;
    }
    g_api.render_length = reinterpret_cast<int (*)(int *)>(dlsym(g_api.handle, "NDL_DirectVideoGetRenderBufferLength"));
    return true;
}

std::string last_error() {
    const char *e = g_api.get_error != nullptr ? g_api.get_error() : nullptr;
    return e != nullptr && e[0] != '\0' ? e : "no error text";
}

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// Lock held.
void quit_locked() {
    if (!g_initialized) return;
    const auto t = std::chrono::steady_clock::now();
    const int rc = g_api.quit();
    g_initialized = false;
    WNK_LOGI(kCat, "NDL_DirectMediaQuit -> %d (%.1f ms)", rc, ms_since(t));
}

// Signal context (crash handler) and atexit: release the decoder without taking the mutex, which
// the crashing thread may hold. Best effort by design.
void emergency_release() {
    if (g_initialized && g_api.quit != nullptr) {
        if (g_api.unload != nullptr) g_api.unload();
        g_api.quit();
        g_initialized = false;
    }
}

void at_exit_release() {
    std::lock_guard<std::mutex> lock(g_mutex);
    quit_locked();
}

}  // namespace

int64_t PtsSplice::map(int64_t pts_us) {
    if (pts_us == kNoTime) return kNoTime;
    if (pending_) {
        pending_ = false;
        shift_us_ = (max_out_us_ == kNoTime ? pts_us : max_out_us_ + gap_us_) - pts_us;
    }
    const int64_t out = pts_us + shift_us_;
    max_out_us_ = max_out_us_ == kNoTime ? out : std::max(max_out_us_, out);
    return out;
}

NdlBackend::NdlBackend(NdlOptions options) : options_(std::move(options)), splice_(kSpliceGapUs) {
    if (options_.app_id.empty()) {
        const char *id = std::getenv("APPID");
        options_.app_id = id != nullptr ? id : "";
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_backends;
}

NdlBackend::~NdlBackend() {
    std::lock_guard<std::mutex> lock(g_mutex);
    unload_locked();
    if (g_active.load() == this) g_active = nullptr;
    if (--g_backends == 0) quit_locked();
}

bool NdlBackend::available(std::string *why) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool ok = load_api();
    if (!ok && why != nullptr) *why = g_api.why;
    return ok;
}

void NdlBackend::set_app_foreground(bool foreground) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_foreground = foreground;
    if (g_initialized) {
        const int rc = g_api.set_app_state(foreground ? 0 : 1);
        WNK_LOGI(kCat, "NDL_DirectMediaSetAppState(%s) -> %d", foreground ? "FOREGROUND" : "BACKGROUND", rc);
    }
}

BackendCaps NdlBackend::caps() const {
    BackendCaps c;
    c.video_codecs = {CodecId::H264, CodecId::Hevc, CodecId::Vp9, CodecId::Av1};
    // PCM only: MP3 passed through loaded fine but played silently on an OLED65C1PTB (webOS 6.5.3,
    // mono 48 kHz), so sources decode MP3 to PCM like everything else.
    c.audio_codecs = {CodecId::PcmS16};
    const bool hold = !options_.reload_to_pause_and_seek;
    c.pause = hold;
    c.pause_drains = hold;
    c.seek_splices = hold;
    c.splice_gap_us = hold ? kSpliceGapUs : 0;
    c.position = false;
    return c;
}

void NdlBackend::set_event_callback(EventFn fn) {
    std::lock_guard<std::mutex> lock(events_mutex_);
    events_ = std::move(fn);
}

void NdlBackend::emit(BackendEvent e, const std::string &detail) {
    EventFn fn;
    {
        std::lock_guard<std::mutex> lock(events_mutex_);
        fn = events_;
    }
    if (fn) fn(e, detail);
}

int NdlBackend::pcm_rate_enum(int hertz) {
    switch (hertz) {
        case 48000: return 1;
        case 44100: return 2;
        case 32000: return 3;
        case 24000: return 4;
        case 22050: return 5;
        case 16000: return 6;
        case 12000: return 7;
        case 8000: return 8;
        default: return 0;
    }
}

long long NdlBackend::convert_pts(int64_t pts_us, PtsUnit unit) {
    if (pts_us == kNoTime) return 0;
    return unit == PtsUnit::Milliseconds ? pts_us / 1000 : pts_us;
}

// NDL calls these on its own thread. They must not take g_mutex: NDL may call back from inside a
// call we are making while holding it.
void NdlBackend::load_callback(int type, long long num, const char *str) {
    WNK_LOGI(kCat, "load callback type=0x%x num=%lld str=%s", type, num, str != nullptr ? str : "");
    NdlBackend *b = g_active.load();
    if (b == nullptr) return;
    // Event ids shared with Starfish PF_EVENT_T (webosbrew samples): 0x16 load completed,
    // 0x1a playing, 0x17 unload completed, 0x1c end of stream.
    switch (type) {
        case 0x16: b->emit(BackendEvent::Loaded, ""); break;
        case 0x1a: b->emit(BackendEvent::Playing, ""); break;
        case 0x1c: b->emit(BackendEvent::EndOfStream, ""); break;
        default: break;
    }
}

void NdlBackend::resource_released(const char *type) {
    WNK_LOGW(kCat, "resource released: %s", type != nullptr ? type : "?");
    if (NdlBackend *b = g_active.load()) b->emit(BackendEvent::ResourceLost, type != nullptr ? type : "");
}

bool NdlBackend::load(const StreamFormat &format, std::string *error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return load_locked(format, error);
}

bool NdlBackend::load_locked(const StreamFormat &format, std::string *error) {
    if (!load_api()) {
        *error = "NDL unavailable: " + g_api.why;
        return false;
    }
    if (loaded_) unload_locked();
    if (!g_initialized) {
        const auto t = std::chrono::steady_clock::now();
        const int rc = g_api.init(options_.app_id.c_str(), &NdlBackend::resource_released);
        WNK_LOGI(kCat, "NDL_DirectMediaInit(%s) -> %d (%.1f ms)", options_.app_id.c_str(), rc, ms_since(t));
        if (rc != 0) {
            *error = "NDL_DirectMediaInit failed: " + last_error();
            return false;
        }
        g_initialized = true;
        static bool hooks = false;
        if (!hooks) {
            hooks = true;
            std::atexit(&at_exit_release);
            diagnostics::add_crash_cleanup(&emergency_release);
        }
        const int st = g_api.set_app_state(g_foreground ? 0 : 1);
        WNK_LOGI(kCat, "NDL_DirectMediaSetAppState(%s) -> %d", g_foreground ? "FOREGROUND" : "BACKGROUND", st);
    }

    wnk_ndl_data_info info{};
    if (format.video) {
        switch (format.video->codec) {
            case CodecId::H264: info.video.type = WNK_NDL_VIDEO_H264; break;
            case CodecId::Hevc: info.video.type = WNK_NDL_VIDEO_H265; break;
            case CodecId::Vp9: info.video.type = WNK_NDL_VIDEO_VP9; break;
            case CodecId::Av1: info.video.type = WNK_NDL_VIDEO_AV1; break;
            default: *error = std::string("video codec ") + codec_name(format.video->codec) + " not supported"; return false;
        }
        info.video.width = format.video->width;
        info.video.height = format.video->height;
    }
    if (format.audio) {
        if (format.audio->codec == CodecId::PcmS16) {
            const int rate = pcm_rate_enum(format.audio->sample_rate);
            if (rate == 0) {
                *error = "PCM sample rate " + std::to_string(format.audio->sample_rate) + " Hz has no NDL value";
                return false;
            }
            info.audio.pcm.type = WNK_NDL_AUDIO_PCM;
            info.audio.pcm.format = "S16LE";
            info.audio.pcm.layout = "interleaved";
            info.audio.pcm.channel_mode = "stereo";
            info.audio.pcm.sample_rate = rate;
        } else if (format.audio->codec == CodecId::Mp3) {
            info.audio.type = WNK_NDL_AUDIO_MP3;
        } else {
            *error = std::string("audio codec ") + codec_name(format.audio->codec) + " not supported";
            return false;
        }
    }
    g_active = this;
    const auto t = std::chrono::steady_clock::now();
    const int rc = g_api.load(&info, &NdlBackend::load_callback);
    WNK_LOGI(kCat, "NDL_DirectMediaLoad(video %s %dx%d, audio %s %d Hz) -> %d (%.1f ms)",
             format.video ? codec_name(format.video->codec) : "-", info.video.width, info.video.height,
             format.audio ? codec_name(format.audio->codec) : "-", format.audio ? format.audio->sample_rate : 0, rc,
             ms_since(t));
    if (rc != 0) {
        *error = "NDL_DirectMediaLoad failed: " + last_error();
        return false;
    }
    loaded_ = true;
    format_ = format;
    video_fed_ = audio_fed_ = 0;
    splice_.reset();
    const Rect a = area_set_ ? area_ : Rect{0, 0, 1920, 1080};
    const int ar = g_api.set_area(static_cast<int>(a.x), static_cast<int>(a.y), static_cast<int>(a.w), static_cast<int>(a.h));
    WNK_LOGI(kCat, "NDL_DirectVideoSetArea(%.0f, %.0f, %.0f, %.0f) -> %d", a.x, a.y, a.w, a.h, ar);
    return true;
}

MediaBackend::Feed NdlBackend::feed(const Packet &p) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!loaded_) return Feed::Error;
    if (p.type != TrackType::Video && p.type != TrackType::Audio) return Feed::Accepted;  // not NDL's
    const bool video = p.type == TrackType::Video;
    if (video && !format_.video) return Feed::Accepted;  // not loaded with that stream: drop
    if (!video && !format_.audio) return Feed::Accepted;
    const int64_t before = splice_.shift_us();
    const int64_t out_us = splice_.map(p.pts_us);
    if (splice_.shift_us() != before) {
        WNK_LOGI(kCat, "after seek: content %lld us continues at %lld us", static_cast<long long>(p.pts_us),
                 static_cast<long long>(out_us));
    }
    const long long pts = convert_pts(out_us, options_.pts_unit);
    void *data = const_cast<uint8_t *>(p.data.data());  // NDL takes void*, does not write
    const auto size = static_cast<unsigned int>(p.data.size());
    const int rc = video ? g_api.video_play(data, size, pts) : g_api.audio_play(data, size, pts);
    if (rc != 0) return Feed::Full;  // NDL reports a full buffer as non-zero
    uint64_t &count = video ? video_fed_ : audio_fed_;
    if (++count == 1) {
        WNK_LOGI(kCat, "first %s packet accepted: pts %lld (%s), %u bytes", video ? "video" : "audio", pts,
                 options_.pts_unit == PtsUnit::Milliseconds ? "ms" : "us", size);
    }
    return Feed::Accepted;
}

bool NdlBackend::pause(bool paused) {
    if (options_.reload_to_pause_and_seek) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    int len = -1;
    const int rc = g_api.render_length != nullptr ? g_api.render_length(&len) : -2;
    WNK_LOGI(kCat, "%s by %s feeding (decoder stays loaded); render buffer %d (rc %d)", paused ? "pause" : "resume",
             paused ? "stopping" : "restarting", len, rc);
    return loaded_;
}

bool NdlBackend::flush(std::string *error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!loaded_) return true;
    if (!options_.reload_to_pause_and_seek) {
        int len = -1;
        if (g_api.render_length != nullptr) g_api.render_length(&len);
        WNK_LOGI(kCat, "splice seek: nothing flushed (render buffer %d); later packets continue the timeline", len);
        splice_.splice();
        return true;
    }
    const StreamFormat f = format_;
    unload_locked();
    return load_locked(f, error);
}

void NdlBackend::unload() {
    std::lock_guard<std::mutex> lock(g_mutex);
    unload_locked();
}

void NdlBackend::unload_locked() {
    if (!loaded_) return;
    const auto t = std::chrono::steady_clock::now();
    const int rc = g_api.unload();
    loaded_ = false;
    WNK_LOGI(kCat, "NDL_DirectMediaUnload -> %d (%.1f ms; fed %llu video, %llu audio)", rc, ms_since(t),
             static_cast<unsigned long long>(video_fed_), static_cast<unsigned long long>(audio_fed_));
}

void NdlBackend::set_area(const Rect &pixels) {
    std::lock_guard<std::mutex> lock(g_mutex);
    area_ = pixels;
    area_set_ = true;
    if (loaded_) {
        const int rc = g_api.set_area(static_cast<int>(pixels.x), static_cast<int>(pixels.y), static_cast<int>(pixels.w),
                                      static_cast<int>(pixels.h));
        WNK_LOGI(kCat, "NDL_DirectVideoSetArea(%.0f, %.0f, %.0f, %.0f) -> %d", pixels.x, pixels.y, pixels.w, pixels.h, rc);
    }
}

}  // namespace wnk::media
