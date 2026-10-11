// SPDX-License-Identifier: GPL-3.0-or-later
//
// Derived from PlxNative's src/starfish.c and rust-modules/media/src/player/engine.rs (Copyright the
// PlxNative authors, GPL-3.0-or-later): the mangled entry points, the in-place object, the sret
// Feed reply, the BUFFERSTREAM payload, the exported-window binding and the frame-rate envelope.
#include "wnk/media/starfish_backend.hpp"

#include "wnk/log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include <dlfcn.h>

namespace wnk::media {
namespace {

constexpr const char *kCat = "starfish";
constexpr size_t kObjectSize = 65536;  // generous: the class's real size is not published
constexpr int64_t kSpliceGapUs = 200000;

using EventCb = void (*)(int type, long long num, const char *str, void *ctx);

struct Api {
    void *handle = nullptr;
    void (*ctor)(void *self, const char *uid) = nullptr;
    void (*dtor)(void *self) = nullptr;
    int (*load)(void *self, const char *payload, EventCb cb, void *ctx) = nullptr;
    void (*feed)(void *sret, void *self, const char *payload) = nullptr;  // returns std::string via sret
    int (*play)(void *self) = nullptr;
    int (*pause)(void *self) = nullptr;
    int (*unload)(void *self) = nullptr;
    int (*push_eos)(void *self) = nullptr;
    void (*notify_foreground)(void *self) = nullptr;
    // SDL-webOS exported window (already loaded with the app's SDL).
    const char *(*create_window)(int type) = nullptr;
    int (*set_window)(const char *id, void *src, void *dst) = nullptr;  // SDL_bool, SDL_Rect*
    void (*destroy_window)(const char *id) = nullptr;
    std::string why;
    bool tried = false;
};

std::mutex g_api_mutex;
Api g_api;

template <typename F>
bool sym(void *handle, F &out, const char *name) {
    out = reinterpret_cast<F>(dlsym(handle, name));
    if (out == nullptr && g_api.why.empty()) g_api.why = std::string("missing ") + name;
    return out != nullptr;
}

const Api &api() {
    std::lock_guard<std::mutex> lock(g_api_mutex);
    if (g_api.tried) return g_api;
    g_api.tried = true;
    void *h = dlopen("libplayerAPIs.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (h == nullptr) {
        const char *e = dlerror();
        g_api.why = e != nullptr ? e : "dlopen libplayerAPIs.so.1 failed";
        return g_api;
    }
    bool ok = sym(h, g_api.ctor, "_ZN17StarfishMediaAPIsC1EPKc");
    ok = sym(h, g_api.dtor, "_ZN17StarfishMediaAPIsD1Ev") && ok;
    ok = sym(h, g_api.load, "_ZN17StarfishMediaAPIs4LoadEPKcPFvixS1_PvES2_") && ok;
    ok = sym(h, g_api.feed, "_ZN17StarfishMediaAPIs4FeedB5cxx11EPKc") && ok;
    ok = sym(h, g_api.play, "_ZN17StarfishMediaAPIs4PlayEv") && ok;
    ok = sym(h, g_api.pause, "_ZN17StarfishMediaAPIs5PauseEv") && ok;
    ok = sym(h, g_api.unload, "_ZN17StarfishMediaAPIs6UnloadEv") && ok;
    ok = sym(h, g_api.push_eos, "_ZN17StarfishMediaAPIs7pushEOSEv") && ok;
    ok = sym(h, g_api.notify_foreground, "_ZN17StarfishMediaAPIs16notifyForegroundEv") && ok;
    ok = sym(RTLD_DEFAULT, g_api.create_window, "SDL_webOSCreateExportedWindow") && ok;
    ok = sym(RTLD_DEFAULT, g_api.set_window, "SDL_webOSSetExportedWindow") && ok;
    ok = sym(RTLD_DEFAULT, g_api.destroy_window, "SDL_webOSDestroyExportedWindow") && ok;
    if (!ok) {
        dlclose(h);
        return g_api;
    }
    g_api.handle = h;
    return g_api;
}

bool usable() { return api().handle != nullptr; }

// Callbacks carry an epoch, never a pointer: a late event after the backend is gone finds nothing.
std::mutex g_registry_mutex;
std::map<unsigned, StarfishBackend *> g_registry;
unsigned g_next_epoch = 0;

void event_trampoline(int type, long long num, const char *str, void *ctx) {
    const auto epoch = static_cast<unsigned>(reinterpret_cast<uintptr_t>(ctx));
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    const auto it = g_registry.find(epoch);
    if (it != g_registry.end()) it->second->on_event(type, num, str);
}

struct SdlRect {
    int x, y, w, h;  // SDL_Rect
};

std::string json_escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out;
}

}  // namespace

const char *StarfishBackend::codec_string(CodecId c) {
    switch (c) {
        case CodecId::H264: return "H264";
        case CodecId::Hevc: return "H265";
        case CodecId::Aac: return "AAC";
        case CodecId::Ac3: return "AC3";
        case CodecId::Eac3: return "AC3 PLUS";  // LG's name; "EAC3" is not in its vocabulary
        default: return nullptr;
    }
}

int StarfishBackend::frame_rate_class(double fps) {
    for (int c : {24, 25, 30, 50, 60}) {
        if (fps <= c + 0.01) return c;
    }
    return static_cast<int>(std::ceil(fps));
}

void StarfishBackend::frame_rate_fraction(double fps, int64_t *num, int64_t *den) {
    struct Known {
        double fps;
        int64_t num, den;
    };
    static constexpr Known kKnown[] = {{23.976, 24000, 1001}, {29.97, 30000, 1001}, {59.94, 60000, 1001}};
    for (const Known &k : kKnown) {
        if (std::abs(fps - k.fps) < 0.005) {
            *num = k.num;
            *den = k.den;
            return;
        }
    }
    if (std::abs(fps - std::round(fps)) < 0.001) {
        *num = static_cast<int64_t>(std::round(fps));
        *den = 1;
        return;
    }
    *num = static_cast<int64_t>(std::llround(fps * 1000));
    *den = 1000;
}

std::string StarfishBackend::build_load_payload(const StarfishLoad &l) {
    const char *video = codec_string(l.video);
    const char *audio = codec_string(l.audio);
    const bool has_audio = audio != nullptr;
    // The sink the pipeline allocates for (PlxNative's envelope): H.264 that fits FHD declares
    // 1920x1080 (webOS 10.3.1 refuses H.264 declared as 4K), everything else 3840x2160. H.264
    // declares its own rate class; HEVC stays at 60, the value its Dolby Vision runs were verified at.
    const bool fhd = l.video == CodecId::H264 && l.width > 0 && l.height > 0 && l.width <= 1920 && l.height <= 1088;
    const int max_w = fhd ? 1920 : 3840, max_h = fhd ? 1080 : 2160;
    const int max_fps = l.video == CodecId::H264 && l.frame_rate > 0 ? frame_rate_class(l.frame_rate) : 60;
    std::string codec = std::string("{\"video\":\"") + (video != nullptr ? video : "H264") + "\"";
    if (has_audio) codec += std::string(",\"audio\":\"") + audio + "\"";
    codec += "}";
    std::string es = "{\"pauseAtDecodeTime\":false,\"ptsToDecode\":0,\"seperatedPTS\":true";
    if (l.frame_rate > 0) {
        int64_t num = 0, den = 1;
        frame_rate_fraction(l.frame_rate, &num, &den);
        es += ",\"videoFpsValue\":" + std::to_string(num) + ",\"videoFpsScale\":" + std::to_string(den);
    }
    es += "}";
    const std::string audio_buffer = has_audio ? "1048576" : "32768";
    std::string p = "{\"args\":[{\"mediaTransportType\":\"BUFFERSTREAM\",\"option\":{";
    p += "\"appId\":\"" + json_escape(l.app_id) + "\"";
    if (!l.window_id.empty()) p += ",\"windowId\":\"" + json_escape(l.window_id) + "\"";
    // The codec stays "H265" for Dolby Vision: libpf maps it to video/x-h265 and adds the DV caps
    // when DolbyHdrInfo is present; profileId must be a JSON integer (a string reads as -1).
    std::string extra;
    if (l.dolby_vision_profile >= 0 && l.video == CodecId::Hevc) {
        extra += ",\"DolbyHdrInfo\":{\"trackType\":\"single\",\"encryptionType\":\"clear\",\"profileId\":" +
                 std::to_string(l.dolby_vision_profile) + "}";
    }
    if (l.atmos && l.audio == CodecId::Eac3) extra += ",\"immersive\":\"ATMOS\"";
    p += ",\"externalStreamingInfo\":{\"contents\":{\"codec\":" + codec + ",\"esInfo\":" + es +
         ",\"format\":\"RAW\",\"provider\":\"wnk\"" + extra + "},\"streamQualityInfo\":true,\"streamQualityInfoNonFlushable\":true,"
         "\"audioSync\":true,\"restartStreaming\":false,\"bufferingCtrInfo\":{\"bufferMaxLevel\":0,\"bufferMinLevel\":0,"
         "\"preBufferByte\":0,\"qBufferLevelAudio\":0,\"qBufferLevelVideo\":0,"
         "\"srcBufferLevelAudio\":{\"minimum\":1,\"maximum\":" + audio_buffer +
         "},\"srcBufferLevelVideo\":{\"minimum\":1,\"maximum\":8388608}}}";
    p += std::string(",\"needAudio\":") + (has_audio ? "true" : "false");
    p += std::string(",\"queryPosition\":false,\"lowDelayMode\":") + (has_audio ? "false" : "true");
    p += ",\"transmission\":{\"contentsType\":\"LIVE\"}";
    p += ",\"adaptiveStreaming\":{\"audioOnly\":false";
    if (l.frame_rate > 0) p += ",\"adaptiveResolution\":true";
    p += ",\"maxWidth\":" + std::to_string(max_w) + ",\"maxHeight\":" + std::to_string(max_h) +
         ",\"maxFrameRate\":" + std::to_string(max_fps) + "}}}]}";
    return p;
}

bool StarfishBackend::available(std::string *why) {
    const bool ok = usable();
    if (!ok && why != nullptr) *why = api().why;
    return ok;
}

StarfishBackend::StarfishBackend(StarfishOptions options) : options_(std::move(options)), splice_(kSpliceGapUs) {
    if (options_.app_id.empty()) {
        const char *id = std::getenv("APPID");
        options_.app_id = id != nullptr ? id : "";
    }
    if (!usable()) return;
    // Type 0 is the video window (LG's header spells the constant EXPORED).
    if (const char *id = api().create_window(0)) {
        window_id_ = id;  // SDL's string has no documented lifetime: copy it
        WNK_LOGI(kCat, "exported window %s", window_id_.c_str());
    } else {
        WNK_LOGW(kCat, "SDL_webOSCreateExportedWindow failed: the video will not be visible");
    }
}

StarfishBackend::~StarfishBackend() {
    unload();
    if (!window_id_.empty() && usable()) api().destroy_window(window_id_.c_str());
}

BackendCaps StarfishBackend::caps() const {
    BackendCaps c;
    c.video_codecs = {CodecId::H264, CodecId::Hevc};
    c.audio_codecs = {CodecId::Aac, CodecId::Ac3, CodecId::Eac3};
    c.aac_adts = true;
    c.adaptive_resolution = true;  // Load declares adaptiveResolution and the 4K/FHD envelope
    c.pause = true;
    c.position = true;
    c.seek_splices = true;
    c.splice_gap_us = kSpliceGapUs;
    return c;
}

void StarfishBackend::set_event_callback(EventFn fn) {
    std::lock_guard<std::mutex> lock(events_mutex_);
    events_ = std::move(fn);
}

void StarfishBackend::on_event(int type, long long num, const char *str) {
    if (type == 23) {  // unload completed: the destructor may run
        std::lock_guard<std::mutex> lock(unload_mutex_);
        unload_completed_ = true;
        unload_cv_.notify_all();
    }
    if (type == 0) {  // FRAMEREADY: the presented PTS, in ns
        presented_out_us_ = num / 1000;
        if (!seen_frame_.exchange(true)) WNK_LOGI(kCat, "first frame presented (pts %lld ns)", num);
        return;
    }
    const std::string s = str != nullptr ? str : "";
    if (events_logged_.fetch_add(1) < 40) WNK_LOGI(kCat, "event type=%d num=%lld %s", type, num, s.substr(0, 300).c_str());
    EventFn fn;
    {
        std::lock_guard<std::mutex> lock(events_mutex_);
        fn = events_;
    }
    if (!load_completed_ && (s.find("loadCompleted") != std::string::npos || s.find("\"loaded\"") != std::string::npos)) {
        load_completed_ = true;
        if (fn) fn(BackendEvent::Loaded, "");
    } else if (s.find("endOfStream") != std::string::npos) {
        if (fn) fn(BackendEvent::EndOfStream, "");
    } else if (type == 18 && !seen_frame_) {
        if (fn) fn(BackendEvent::Error, "pipeline refused the stream (" + std::to_string(num) + ")");
    }
}

bool StarfishBackend::load(const StreamFormat &format, std::string *error) {
    if (!usable()) {
        *error = "Starfish unavailable: " + api().why;
        return false;
    }
    if (!format.video) {
        *error = "the Starfish backend needs a video stream";
        return false;
    }
    unload();
    void *object = nullptr;
    if (posix_memalign(&object, 16, kObjectSize) != 0) {
        *error = "out of memory";
        return false;
    }
    std::memset(object, 0, kObjectSize);
    StarfishLoad l;
    l.app_id = options_.app_id;
    l.window_id = window_id_;
    l.video = format.video->codec;
    l.width = format.video->width;
    l.height = format.video->height;
    l.frame_rate = format.video->frame_rate;
    if (format.audio) l.audio = format.audio->codec;
    if (options_.dolby_vision) l.dolby_vision_profile = format.video->dolby_vision_profile;
    l.atmos = options_.atmos && format.audio && format.audio->atmos;
    const std::string payload = build_load_payload(l);
    WNK_LOGI(kCat, "Load %s", payload.c_str());
    unsigned epoch;
    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        epoch = ++g_next_epoch;
        g_registry[epoch] = this;
    }
    load_completed_ = false;
    seen_frame_ = false;
    {
        std::lock_guard<std::mutex> lock(unload_mutex_);
        unload_completed_ = false;
    }
    presented_out_us_ = kNoTime;
    // uid NULL: the pipeline registers on the pre-authorised media namespace (PlxNative).
    api().ctor(object, nullptr);
    api().notify_foreground(object);
    const auto t = std::chrono::steady_clock::now();
    const int ok = api().load(object, payload.c_str(), &event_trampoline, reinterpret_cast<void *>(static_cast<uintptr_t>(epoch)));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    WNK_LOGI(kCat, "Load -> %d (%.1f ms)", ok, ms);
    if ((ok & 0xFF) == 0) {
        // The object stays allocated (callbacks may still name it) and is never reused.
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        g_registry.erase(epoch);
        *error = "StarfishMediaAPIs::Load refused the stream";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        object_ = object;
        epoch_ = epoch;
        format_ = format;
        loaded_ = true;
        play_issued_ = false;
        paused_ = false;
        splice_.reset();
        replayed_ = false;
    }
    // Placing the window is an SDL call: the main thread does it through set_area().
    return true;
}

MediaBackend::Feed StarfishBackend::feed(const Packet &p) {
    if (p.type != TrackType::Video && p.type != TrackType::Audio) return Feed::Accepted;
    void *object;
    int64_t out_us;
    bool play = false, replay = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loaded_) return Feed::Error;
        if (p.type == TrackType::Audio && !format_.audio) return Feed::Accepted;
        object = object_;
        out_us = splice_.map(p.pts_us);
        // Play issued before loadCompleted is queued; it is issued once more after it (samples README).
        if (!paused_ && !play_issued_) play = play_issued_ = true;
        if (!paused_ && load_completed_ && !replayed_) replay = replayed_ = true;
    }
    if (play) WNK_LOGI(kCat, "Play -> %d", api().play(object));
    if (replay && !play) WNK_LOGI(kCat, "Play after loadCompleted -> %d", api().play(object));
    char json[160];
    std::snprintf(json, sizeof json, "{\"bufferAddr\":\"%p\",\"bufferSize\":%u,\"pts\":%lld,\"esData\":%d}",
                  static_cast<const void *>(p.data.data()), static_cast<unsigned>(p.data.size()),
                  static_cast<long long>(out_us == kNoTime ? 0 : out_us * 1000), p.type == TrackType::Video ? 1 : 2);
    alignas(8) unsigned char reply[32] = {};
    api().feed(reply, object, json);
    const char *s = *reinterpret_cast<const char *const *>(reply);
    if (s == nullptr) return Feed::Error;
    if (std::strstr(s, "BufferFull") != nullptr) return Feed::Full;
    if (std::strstr(s, "Ok") != nullptr) return Feed::Accepted;
    WNK_LOGW(kCat, "Feed reply \"%s\"", s);
    return Feed::Error;
}

bool StarfishBackend::flush(std::string *) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) return true;
    splice_.splice();  // later packets continue the timeline; nothing is dropped
    return true;
}

void StarfishBackend::unload() {
    void *object = nullptr;
    unsigned epoch = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loaded_) return;
        loaded_ = false;
        object = object_;
        epoch = epoch_;
        object_ = nullptr;
    }
    const auto t = std::chrono::steady_clock::now();
    const int rc = api().unload(object);
    // The destructor is what gives the decoders back (a Load after an Unload without it waited for
    // them forever on webOS 6.5.3), and PlxNative runs it only after UnloadCompleted (type 23).
    bool completed;
    {
        std::unique_lock<std::mutex> lock(unload_mutex_);
        completed = unload_cv_.wait_for(lock, std::chrono::seconds(3), [this] { return unload_completed_; });
    }
    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        g_registry.erase(epoch);  // later callbacks are dropped
    }
    if (completed) api().dtor(object);
    WNK_LOGI(kCat, "Unload -> %d, %s (%.1f ms)", rc, completed ? "destroyed" : "no UnloadCompleted: object kept",
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count());
    // The memory block itself is never freed or reused: a late callback carries only its address.
}

bool StarfishBackend::pause(bool paused) {
    void *object;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loaded_) return false;
        object = object_;
        paused_ = paused;
        if (!paused) play_issued_ = true;
    }
    const int rc = paused ? api().pause(object) : api().play(object);
    WNK_LOGI(kCat, "%s -> %d", paused ? "Pause" : "Play", rc);
    return true;
}

void StarfishBackend::end_of_stream() {
    void *object;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loaded_) return;
        object = object_;
    }
    WNK_LOGI(kCat, "pushEOS -> %d", api().push_eos(object));
}

int64_t StarfishBackend::position_us() const {
    const int64_t out = presented_out_us_.load();
    if (out == kNoTime) return kNoTime;
    std::lock_guard<std::mutex> lock(mutex_);
    return splice_.unmap(out);
}

void StarfishBackend::set_area(const Rect &pixels) {
    std::lock_guard<std::mutex> lock(mutex_);
    area_ = pixels;
    place_locked();
}

void StarfishBackend::place_locked() {
    if (window_id_.empty() || !usable()) return;
    // Source: the coded picture (or the area until the stream is known); destination: the area.
    SdlRect src{0, 0, format_.video ? format_.video->width : static_cast<int>(area_.w),
                format_.video ? format_.video->height : static_cast<int>(area_.h)};
    SdlRect dst{static_cast<int>(area_.x), static_cast<int>(area_.y), static_cast<int>(area_.w), static_cast<int>(area_.h)};
    const int rc = api().set_window(window_id_.c_str(), &src, &dst);
    WNK_LOGI(kCat, "place %dx%d -> %d,%d %dx%d rv=%d", src.w, src.h, dst.x, dst.y, dst.w, dst.h, rc);
}

}  // namespace wnk::media
