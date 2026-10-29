// SPDX-License-Identifier: GPL-3.0-or-later
//
// Derived from PlxNative's src/starfish.c and rust-modules/media/src/player/engine.rs (Copyright the
// PlxNative authors, GPL-3.0-or-later): the mangled entry points, the in-place object, the sret
// Feed reply, the BUFFERSTREAM payload, the exported-window binding and the frame-rate envelope.
// The setHdrInfo payload, ac3PlusInfo, the VP9/AV1 names and the getMaxVideoResolution envelope follow
// Kodi's xbmc/cores/VideoPlayer/MediaPipelineWebOS.cpp (Team Kodi, GPL-2.0-or-later), and so do the
// ACB video plane, the mediaSei/mediaVui names and the pre-webOS-5 event numbering. The copy-on-write
// Feed ABI and the video-info HDR10 rewrite follow PlxNative's src/starfish.c and
// rust-modules/media/src/player/vp9_hdr.rs.
#include "wnk/media/starfish_backend.hpp"

#include "wnk/log.hpp"
#include "wnk/platform/luna.hpp"
#include "wnk/platform/media_role.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <thread>
#include <type_traits>
#include <vector>

#include <dlfcn.h>
#include <unistd.h>

namespace wnk::media {
namespace {

constexpr const char *kCat = "starfish";
constexpr size_t kObjectSize = 65536;  // generous: the class's real size is not published
constexpr int64_t kSpliceGapUs = 200000;
// New content after a seek lands at least this far ahead of the decoder's clock (see PtsSplice).
constexpr int64_t kSpliceClockLeadUs = 600000;

// PF_EVENT_T as webOS 5+ numbers it (the NDK's StarfishMediaAPIs.h).
constexpr int kEvVideoInfo = 0x4, kEvAudioInfo = 0x7, kEvUnloadCompleted = 0x17, kEvPlaying = 0x1a,
              kEvPaused = 0x1b, kEvEndOfStream = 0x1c, kEvDropped = 0x30, kEvDisplayed = 0x31;
// AcbAPI.h: PLAYER_TYPE_MSE, SINK_TYPE_MAIN, APPSTATE_FOREGROUND, PLAYSTATE_*.
constexpr int kAcbPlayerMse = 10, kAcbSinkMain = 0, kAcbForeground = 1;
constexpr int kAcbUnloaded = 0, kAcbLoaded = 1, kAcbPlaying = 2, kAcbPaused = 3;

using EventCb = void (*)(int type, long long num, const char *str, void *ctx);

struct Api {
    void *handle = nullptr;
    void (*ctor)(void *self, const char *uid) = nullptr;
    void (*dtor)(void *self) = nullptr;
    int (*load)(void *self, const char *payload, EventCb cb, void *ctx) = nullptr;
    void (*feed)(void *sret, void *self, const char *payload) = nullptr;  // returns std::string via sret
    // webOS 3.x: Feed returns the pre-C++11 copy-on-write string, released with the firmware's own
    // ~basic_string (its rep is refcounted heap; the kit's static libstdc++ must not touch it).
    bool feed_cow = false;
    void (*cow_string_dtor)(void *str) = nullptr;  // null on a COW build: every reply leaks its rep
    const char *(*get_media_id)(void *self) = nullptr;  // for ACB's setMediaId
    int (*play)(void *self) = nullptr;
    int (*pause)(void *self) = nullptr;
    int (*unload)(void *self) = nullptr;
    int (*push_eos)(void *self) = nullptr;
    void (*notify_foreground)(void *self) = nullptr;
    int (*seek)(void *self, const char *millis) = nullptr;  // bool Seek(const char*), on every release
    // Optional (absent on webOS 1.x-3.x images): smp::util::getMaxVideoResolution. Its std::string
    // parameter is by value up to webOS 10 and const& from 11; both pass the string's address.
    bool (*max_video_resolution)(const std::string &codec, int32_t *w, int32_t *h, int32_t *fps) = nullptr;
    int (*set_hdr_info)(void *self, const char *payload) = nullptr;  // optional: webOS 5+
    // SDL-webOS exported window (already loaded with the app's SDL); optional, webOS 5+ only works.
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
    // Both Feed names are looked up, never linked: a strong reference to the cxx11 one killed the
    // process at the first Feed on 3.4.0 (PlxNative).
    g_api.feed = reinterpret_cast<decltype(g_api.feed)>(dlsym(h, "_ZN17StarfishMediaAPIs4FeedB5cxx11EPKc"));
    if (g_api.feed == nullptr) {
        g_api.feed = reinterpret_cast<decltype(g_api.feed)>(dlsym(h, "_ZN17StarfishMediaAPIs4FeedEPKc"));
        if (g_api.feed != nullptr) {
            g_api.feed_cow = true;
            // The firmware's libstdc++ (a dependency of libplayerAPIs), not RTLD_DEFAULT: the empty
            // rep a COW string may point at is that library's.
            if (void *cxx = dlopen("libstdc++.so.6", RTLD_NOW | RTLD_NOLOAD)) {
                g_api.cow_string_dtor = reinterpret_cast<decltype(g_api.cow_string_dtor)>(dlsym(cxx, "_ZNSsD1Ev"));
            }
        } else {
            ok = false;
            if (g_api.why.empty()) g_api.why = "missing StarfishMediaAPIs::Feed";
        }
    }
    g_api.get_media_id = reinterpret_cast<decltype(g_api.get_media_id)>(dlsym(h, "_ZN17StarfishMediaAPIs10getMediaIDEv"));
    ok = sym(h, g_api.play, "_ZN17StarfishMediaAPIs4PlayEv") && ok;
    ok = sym(h, g_api.pause, "_ZN17StarfishMediaAPIs5PauseEv") && ok;
    ok = sym(h, g_api.unload, "_ZN17StarfishMediaAPIs6UnloadEv") && ok;
    ok = sym(h, g_api.push_eos, "_ZN17StarfishMediaAPIs7pushEOSEv") && ok;
    ok = sym(h, g_api.notify_foreground, "_ZN17StarfishMediaAPIs16notifyForegroundEv") && ok;
    g_api.seek = reinterpret_cast<decltype(g_api.seek)>(dlsym(h, "_ZN17StarfishMediaAPIs4SeekEPKc"));
    g_api.max_video_resolution = reinterpret_cast<decltype(g_api.max_video_resolution)>(dlsym(
        h, "_ZN3smp4util21getMaxVideoResolutionENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPiS7_S7_"));
    if (g_api.max_video_resolution == nullptr) {
        g_api.max_video_resolution = reinterpret_cast<decltype(g_api.max_video_resolution)>(dlsym(
            h, "_ZN3smp4util21getMaxVideoResolutionERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEPiS8_S8_"));
    }
    g_api.set_hdr_info = reinterpret_cast<decltype(g_api.set_hdr_info)>(dlsym(h, "_ZN17StarfishMediaAPIs10setHdrInfoEPKc"));
    g_api.create_window = reinterpret_cast<decltype(g_api.create_window)>(dlsym(RTLD_DEFAULT, "SDL_webOSCreateExportedWindow"));
    g_api.set_window = reinterpret_cast<decltype(g_api.set_window)>(dlsym(RTLD_DEFAULT, "SDL_webOSSetExportedWindow"));
    g_api.destroy_window = reinterpret_cast<decltype(g_api.destroy_window)>(dlsym(RTLD_DEFAULT, "SDL_webOSDestroyExportedWindow"));
    if (g_api.create_window == nullptr || g_api.set_window == nullptr || g_api.destroy_window == nullptr) {
        g_api.create_window = nullptr;
        g_api.set_window = nullptr;
        g_api.destroy_window = nullptr;
    }
    if (!ok) {
        dlclose(h);
        return g_api;
    }
    g_api.handle = h;
    WNK_LOGI(kCat, "feed abi: %s%s", g_api.feed_cow ? "cow" : "cxx11",
             g_api.feed_cow && g_api.cow_string_dtor == nullptr ? " (no ~basic_string: replies leak)" : "");
    return g_api;
}

// libAcbAPI.so.1: the video plane before webOS 5 (absent from 5.3.1 on). Looked up, never linked:
// naming it kills the process on webOS 5 (PlxNative).
using AcbCallback = void (*)(long acb, long task, long event, long app_state, long play_state, const char *reply);
struct AcbApi {
    void *handle = nullptr;
    long (*create)() = nullptr;
    bool (*initialize)(long acb, int player_type, const char *app_id, AcbCallback cb) = nullptr;
    int (*set_media_video_data)(long acb, const char *payload, long *task) = nullptr;
    int (*set_media_audio_data)(long acb, const char *payload, long *task) = nullptr;  // optional: absent on 3.4
    int (*set_state)(long acb, int app_state, int play_state, long *task) = nullptr;
    bool (*set_media_id)(long acb, const char *media_id) = nullptr;
    bool (*set_sink_type)(long acb, int sink) = nullptr;
    int (*set_custom_display_window)(long acb, long in_x, long in_y, long in_w, long in_h, long out_x, long out_y,
                                     long out_w, long out_h, bool full_screen, long *task) = nullptr;
    int (*finalize)(long acb) = nullptr;
    void (*destroy)(long acb) = nullptr;
    std::string why;
    bool tried = false;
};

AcbApi g_acb;

const AcbApi &acb_api() {
    std::lock_guard<std::mutex> lock(g_api_mutex);
    if (g_acb.tried) return g_acb;
    g_acb.tried = true;
    void *h = dlopen("libAcbAPI.so.1", RTLD_NOW | RTLD_LOCAL);
    if (h == nullptr) {
        const char *e = dlerror();
        g_acb.why = e != nullptr ? e : "dlopen libAcbAPI.so.1 failed";
        return g_acb;
    }
    auto get = [h](auto &out, const char *name) {
        out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(dlsym(h, name));
        if (out == nullptr && g_acb.why.empty()) g_acb.why = std::string("missing ") + name;
        return out != nullptr;
    };
    bool ok = get(g_acb.create, "AcbAPI_create");
    ok = get(g_acb.initialize, "AcbAPI_initialize") && ok;
    ok = get(g_acb.set_media_video_data, "AcbAPI_setMediaVideoData") && ok;
    ok = get(g_acb.set_state, "AcbAPI_setState") && ok;
    ok = get(g_acb.set_media_id, "AcbAPI_setMediaId") && ok;
    ok = get(g_acb.set_sink_type, "AcbAPI_setSinkType") && ok;
    ok = get(g_acb.set_custom_display_window, "AcbAPI_setCustomDisplayWindow") && ok;
    ok = get(g_acb.finalize, "AcbAPI_finalize") && ok;
    ok = get(g_acb.destroy, "AcbAPI_destroy") && ok;
    g_acb.set_media_audio_data = reinterpret_cast<decltype(g_acb.set_media_audio_data)>(dlsym(h, "AcbAPI_setMediaAudioData"));
    if (!ok) {
        dlclose(h);
        return g_acb;
    }
    g_acb.handle = h;
    return g_acb;
}

void acb_callback(long acb, long task, long event, long app_state, long play_state, const char *reply) {
    WNK_LOGI(kCat, "acb %ld task %ld event %ld app %ld play %ld %s", acb, task, event, app_state, play_state,
             reply != nullptr ? reply : "");
}

int detect_webos_major() {
    std::ifstream f("/var/run/nyx/os_info.json");
    if (!f) return 0;
    std::stringstream ss;
    ss << f.rdbuf();
    return StarfishBackend::webos_major_from_os_info(ss.str());
}

bool usable() { return api().handle != nullptr; }

// Callbacks carry an epoch, never a pointer: a late event after the backend is gone finds nothing.
// The registries are never destroyed: a release worker still inside the firmware at exit uses them.
std::mutex &g_registry_mutex = *new std::mutex;
std::map<unsigned, StarfishBackend *> &g_registry = *new std::map<unsigned, StarfishBackend *>;
unsigned g_next_epoch = 0;

// One Unload in flight: Unload, the wait for UnloadCompleted and the destructor run on a worker, so
// a firmware call that never returns costs the caller its deadline, not the thread (PlxNative #1116).
struct Release {
    std::mutex m;
    std::condition_variable cv;
    bool completed = false;  // UnloadCompleted (type 23) arrived
    bool done = false;       // the worker finished
    std::chrono::steady_clock::time_point started;
    std::chrono::milliseconds deadline{0};
};
std::map<unsigned, std::shared_ptr<Release>> &g_releases = *new std::map<unsigned, std::shared_ptr<Release>>;

void event_trampoline(int type, long long num, const char *str, void *ctx) {
    const auto epoch = static_cast<unsigned>(reinterpret_cast<uintptr_t>(ctx));
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    if (type == 0x17) {  // UNLOADCOMPLETED, the same number on every release
        if (const auto r = g_releases.find(epoch); r != g_releases.end()) {
            std::lock_guard<std::mutex> rl(r->second->m);
            r->second->completed = true;
            r->second->cv.notify_all();
        }
    }
    const auto it = g_registry.find(epoch);
    if (it != g_registry.end()) it->second->on_event(type, num, str);
}

// Waits for every Unload still in flight, each until its own deadline. False with *error naming the
// oldest one when one is past it: the decoders it holds may never come back.
bool wait_for_releases(std::string *error) {
    std::vector<std::pair<unsigned, std::shared_ptr<Release>>> pending;
    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        pending.assign(g_releases.begin(), g_releases.end());
    }
    for (auto &[epoch, r] : pending) {
        std::unique_lock<std::mutex> lock(r->m);
        if (r->cv.wait_until(lock, r->started + r->deadline, [&r] { return r->done; })) continue;
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - r->started).count();
        *error = "release_timeout: the previous player's Unload has not returned for " + std::to_string(ms) + " ms";
        return false;
    }
    return true;
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
        case CodecId::Vp9: return "VP9";
        case CodecId::Av1: return "AV1";
        case CodecId::Aac: return "AAC";
        case CodecId::Ac3: return "AC3";
        case CodecId::Eac3: return "AC3 PLUS";  // LG's name; "EAC3" is not in its vocabulary
        case CodecId::PcmS16: return "PCM";
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
    // A TV that reports a smaller decoder (an FHD set) bounds the envelope further.
    const bool fhd = l.video == CodecId::H264 && l.width > 0 && l.height > 0 && l.width <= 1920 && l.height <= 1088;
    int max_w = fhd ? 1920 : 3840, max_h = fhd ? 1080 : 2160;
    if (l.decoder_max_width > 0 && l.decoder_max_height > 0) {
        max_w = std::min(max_w, l.decoder_max_width);
        max_h = std::min(max_h, l.decoder_max_height);
    }
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
    // Decoded PCM (PlxNative's pcmInfo): libpf takes the channel count from channelMode alone (mono,
    // stereo, dual: 5.1 and 7.1 cannot be declared, measured on the OLED65C1PTB) and matches
    // sampleRate in kHz (hertz silently plays at 44.1).
    if (l.audio == CodecId::PcmS16 && l.audio_rate > 0) {
        char info[192];
        std::snprintf(info, sizeof info,
                      ",\"pcmInfo\":{\"format\":\"S16LE\",\"channelMode\":\"%s\",\"layout\":\"interleaved\","
                      "\"sampleRate\":%g,\"bitsPerSample\":16}",
                      l.audio_channels == 1 ? "mono" : "stereo", l.audio_rate / 1000.0);
        extra += info;
    }
    // E-AC-3 as it is: its channels (two more for Atmos) and rate in kHz, as Kodi declares it.
    if (l.audio == CodecId::Eac3 && l.audio_channels > 0 && l.audio_rate > 0) {
        char info[96];
        std::snprintf(info, sizeof info, ",\"ac3PlusInfo\":{\"channels\":%d,\"frequency\":%g}",
                      l.audio_channels + (l.atmos ? 2 : 0), l.audio_rate / 1000.0);
        extra += info;
    }
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

bool StarfishBackend::max_video_resolution(const char *codec, int *width, int *height, int *frame_rate) {
    if (!usable() || api().max_video_resolution == nullptr) return false;
    const std::string name = codec;  // short names stay in the small-string buffer
    int32_t w = 0, h = 0, fps = 0;
    if (!api().max_video_resolution(name, &w, &h, &fps)) return false;
    *width = w;
    *height = h;
    *frame_rate = fps;
    return true;
}

std::string StarfishBackend::build_hdr_payload(const VideoColour &c, bool legacy_keys) {
    const char *type = c.transfer == 16 ? "HDR10" : c.transfer == 18 ? "HLG" : nullptr;
    if (type == nullptr) return "";
    // SEI units: chromaticity in 1/50000, luminance in 1/10000 cd/m2, primaries in G, B, R order
    // (CTA-861.3, Kodi). Without any of it the call is skipped: some TVs crash on an HDR message with
    // no SEI (Kodi), and the decoder still sees whatever the bitstream carries.
    std::string sei;
    auto add = [&sei](const char *key, long long v) {
        sei += std::string(sei.empty() ? "" : ",") + "\"" + key + "\":" + std::to_string(v);
    };
    auto chroma = [](double v) { return std::llround(v * 50000); };
    if (c.has_primaries) {
        static constexpr int kOrder[3] = {1, 2, 0};  // G, B, R from R, G, B
        for (int i = 0; i < 3; ++i) {
            add(("displayPrimariesX" + std::to_string(i)).c_str(), chroma(c.display_primaries[kOrder[i]][0]));
            add(("displayPrimariesY" + std::to_string(i)).c_str(), chroma(c.display_primaries[kOrder[i]][1]));
        }
        add("whitePointX", chroma(c.white_point[0]));
        add("whitePointY", chroma(c.white_point[1]));
    }
    if (c.has_luminance) {
        add("minDisplayMasteringLuminance", std::llround(c.min_luminance * 10000));
        add("maxDisplayMasteringLuminance", std::llround(c.max_luminance * 10000));
    }
    if (c.has_light_level) {
        add("maxContentLightLevel", c.max_cll);
        add("maxPicAverageLightLevel", c.max_fall);
    }
    if (sei.empty()) return "";
    const char *sei_key = legacy_keys ? "mediaSei" : "sei";
    const char *vui_key = legacy_keys ? "mediaVui" : "vui";
    return std::string("{\"hdrType\":\"") + type + "\",\"" + sei_key + "\":{" + sei + "},\"" + vui_key +
           "\":{\"transferCharacteristics\":" +
           std::to_string(c.transfer) + ",\"colorPrimaries\":" + std::to_string(c.primaries) +
           ",\"matrixCoeffs\":" + std::to_string(c.matrix) + ",\"videoFullRangeFlag\":" +
           (c.full_range ? "true" : "false") + "}}";
}

std::string StarfishBackend::rewrite_video_info(const std::string &envelope, const VideoColour &c) {
    if (c.transfer != 16) return "";  // HDR10 only: the envelope's name for HLG was never observed
    nlohmann::json v = nlohmann::json::parse(envelope, nullptr, false);
    if (v.is_discarded() || !v.is_object()) return "";
    const auto video = v.find("video");
    if (video == v.end() || !video->is_object()) return "";
    if (const auto t = video->find("hdrType"); t != video->end() && !(t->is_string() && *t == "none")) return "";
    (*video)["hdrType"] = "HDR10";
    (*video)["mediaVui"] = {{"transferCharacteristics", c.transfer}, {"colorPrimaries", c.primaries}, {"matrixCoeffs", c.matrix}};
    // The SEI the TV's own HEVC envelope carries: G, B, R primaries in 1/50000, luminance in 1/10000.
    if (c.has_primaries && c.has_luminance) {
        nlohmann::json sei = nlohmann::json::object();
        static constexpr int kOrder[3] = {1, 2, 0};
        for (int i = 0; i < 3; ++i) {
            sei["displayPrimariesX" + std::to_string(i)] = std::llround(c.display_primaries[kOrder[i]][0] * 50000);
            sei["displayPrimariesY" + std::to_string(i)] = std::llround(c.display_primaries[kOrder[i]][1] * 50000);
        }
        sei["whitePointX"] = std::llround(c.white_point[0] * 50000);
        sei["whitePointY"] = std::llround(c.white_point[1] * 50000);
        sei["minDisplayMasteringLuminance"] = std::llround(c.min_luminance * 10000);
        sei["maxDisplayMasteringLuminance"] = std::llround(c.max_luminance * 10000);
        sei["maxContentLightLevel"] = c.has_light_level ? c.max_cll : 0;
        sei["maxPicAverageLightLevel"] = c.has_light_level ? c.max_fall : 0;
        (*video)["mediaSei"] = sei;
    } else {
        video->erase("mediaSei");
    }
    return v.dump();
}

int StarfishBackend::webos_major_from_os_info(const std::string &json) {
    const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return 0;
    const auto it = j.find("webos_release");
    if (it == j.end() || !it->is_string()) return 0;
    const std::string r = it->get<std::string>();
    int major = 0;
    size_t i = 0;
    for (; i < r.size() && r[i] >= '0' && r[i] <= '9' && major < 1000; ++i) major = major * 10 + (r[i] - '0');
    return i > 0 ? major : 0;
}

int StarfishBackend::normalise_event_type(int type, int webos_major) {
    return webos_major >= 1 && webos_major <= 4 && type > kEvEndOfStream ? type + 2 : type;
}

int StarfishBackend::pcm_lead_ms_for(AudioStack stack, const std::string &output) {
    switch (stack) {
        case AudioStack::Liblxa:
        case AudioStack::AudioSinkNew: return output == "bt_soundbar" ? 250 : 400;
        case AudioStack::LgAudioSink: return 400;
        case AudioStack::AdecSink:
        case AudioStack::Other: return 0;
    }
    return 0;
}

AudioStack StarfishBackend::detect_audio_stack() {
    auto present = [](const char *path) { return access(path, F_OK) == 0; };
    if (present("/usr/lib/gstreamer-1.0/libgstlxaudiosink.so")) return AudioStack::Liblxa;
    if (present("/usr/lib/gstreamer-1.0/libgstlgaudiosink.so")) return AudioStack::LgAudioSink;
    if (present("/usr/lib/liblxa.so.2")) return AudioStack::Liblxa;
    if (present("/usr/lib/gstreamer-1.0/libgstaudiosink_new-1.0.so")) return AudioStack::AudioSinkNew;
    if (present("/usr/lib/gstreamer-1.0/libgstaudiosink-1.0.so")) return AudioStack::AdecSink;
    return AudioStack::Other;
}

const char *StarfishBackend::audio_stack_name(AudioStack s) {
    switch (s) {
        case AudioStack::Liblxa: return "liblxa";
        case AudioStack::LgAudioSink: return "lgaudiosink";
        case AudioStack::AudioSinkNew: return "audiosink_new";
        case AudioStack::AdecSink: return "adecsink";
        case AudioStack::Other: return "other";
    }
    return "?";
}

std::string StarfishBackend::sound_output_from_reply(const std::string &json) {
    const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return "";
    const auto s = j.find("settings");
    if (s == j.end() || !s->is_object()) return "";
    const auto o = s->find("soundOutput");
    return o != s->end() && o->is_string() ? o->get<std::string>() : "";
}

bool StarfishBackend::decoder_listed(int width, int height) {
    // An unknown name gets 1920x1088@60 back (measured on the OLED65C1PTB with "NOSUCH"), so only a
    // larger answer says the TV has that decoder.
    return static_cast<long long>(width) * height > 1920LL * 1088;
}

bool StarfishBackend::available(std::string *why) {
    const bool ok = usable();
    if (!ok && why != nullptr) *why = api().why;
    return ok;
}

// The first packet of a load goes to 100 ms on the decoder's timeline (audio may run a little ahead
// of the first video frame): the pipeline's clock starts at 0 (a live stream joined at 54 s held its
// first frame, measured on the OLED65C1PTB).
StarfishBackend::StarfishBackend(StarfishOptions options)
    : options_(std::move(options)), splice_(kSpliceGapUs, 100000, kSpliceClockLeadUs) {
    if (options_.app_id.empty()) {
        const char *id = std::getenv("APPID");
        options_.app_id = id != nullptr ? id : "";
    }
    webos_major_ = options_.webos_major >= 0 ? options_.webos_major : detect_webos_major();
    if (!usable()) return;
    for (const CodecId c : {CodecId::H264, CodecId::Hevc, CodecId::Vp9, CodecId::Av1}) {
        Limit &l = limits_[static_cast<size_t>(c)];
        l.known = max_video_resolution(codec_string(c), &l.width, &l.height, &l.frame_rate);
        WNK_LOGI(kCat, "decoder %s: %s", codec_string(c),
                 l.known ? (std::to_string(l.width) + "x" + std::to_string(l.height) + "@" + std::to_string(l.frame_rate)).c_str()
                         : "not reported");
    }
    // webOS 5+ has the exported window and no ACB; webOS 3.x and 4.x the other way round. The
    // release picks which to try first and the other is the fallback (Kodi asks the compositor).
    const bool legacy = webos_major_ >= 1 && webos_major_ <= 4;
    const bool placed = legacy ? open_acb() || open_exported_window() : open_exported_window() || open_acb();
    WNK_LOGI(kCat, "webOS %d: video plane %s", webos_major_,
             plane_ == VideoPlane::Acb ? "ACB" : plane_ == VideoPlane::ExportedWindow ? "exported window" : "none");
    if (!placed) WNK_LOGW(kCat, "no video plane: the video will not be visible");
    // PlxNative uses Seek on the exported-window plane and on a known webOS 3; webOS 4 has a private
    // segment seek the kit does not use, so it splices.
    native_seek_ = options_.native_seek && api().seek != nullptr &&
                   (plane_ == VideoPlane::ExportedWindow || webos_major_ == 3);
    WNK_LOGI(kCat, "seeks: %s", native_seek_ ? "LG's Seek" : "splice");
}

bool StarfishBackend::native_seek() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return native_seek_ && !native_seek_refused_;
}

bool StarfishBackend::open_exported_window() {
    if (api().create_window == nullptr) return false;
    // Type 0 is the video window (LG's header spells the constant EXPORED).
    const char *id = api().create_window(0);
    if (id == nullptr || *id == '\0') {
        WNK_LOGW(kCat, "SDL_webOSCreateExportedWindow failed");
        return false;
    }
    window_id_ = id;  // SDL's string has no documented lifetime: copy it
    plane_ = VideoPlane::ExportedWindow;
    WNK_LOGI(kCat, "exported window %s", window_id_.c_str());
    return true;
}

bool StarfishBackend::open_acb() {
    const AcbApi &a = acb_api();
    if (a.handle == nullptr) {
        WNK_LOGI(kCat, "no ACB: %s", a.why.c_str());
        return false;
    }
    const long id = a.create();
    if (id == 0) {
        WNK_LOGW(kCat, "AcbAPI_create failed");
        return false;
    }
    if (!a.initialize(id, kAcbPlayerMse, options_.app_id.c_str(), &acb_callback)) {
        WNK_LOGW(kCat, "AcbAPI_initialize failed");
        a.destroy(id);
        return false;
    }
    acb_id_ = id;
    plane_ = VideoPlane::Acb;
    WNK_LOGI(kCat, "ACB %ld (MSE)", id);
    return true;
}

void StarfishBackend::acb_state(int play_state) {
    if (acb_id_ == 0) return;
    const int rc = acb_api().set_state(acb_id_, kAcbForeground, play_state, &acb_task_);
    WNK_LOGI(kCat, "AcbAPI_setState(%d) -> %d", play_state, rc);
}

StarfishBackend::~StarfishBackend() {
    unload();
    if (!window_id_.empty() && usable()) api().destroy_window(window_id_.c_str());
    if (acb_id_ != 0) {
        acb_api().finalize(acb_id_);
        acb_api().destroy(acb_id_);
    }
}

BackendCaps StarfishBackend::caps() const {
    BackendCaps c;
    c.video_codecs = {CodecId::H264, CodecId::Hevc};
    // VP9 and AV1 only where the TV reports a decoder for them.
    for (const CodecId v : {CodecId::Vp9, CodecId::Av1}) {
        const Limit &l = limits_[static_cast<size_t>(v)];
        if (l.known && decoder_listed(l.width, l.height)) c.video_codecs.push_back(v);
    }
    c.audio_codecs = {CodecId::Aac, CodecId::Ac3, CodecId::Eac3, CodecId::PcmS16};
    c.pcm_max_channels = 2;  // channelMode has no wider value: 5.1 goes to AC-3
    c.pcm_rates = {8000, 12000, 16000, 22050, 24000, 32000, 44100, 48000};  // libpf's LUT_SampleRateType + 44.1
    c.aac_adts = true;
    c.adaptive_resolution = true;  // Load declares adaptiveResolution and the 4K/FHD envelope
    c.pause = true;
    c.position = true;
    c.seek_splices = !native_seek();
    c.splice_gap_us = c.seek_splices ? kSpliceGapUs : 0;
    return c;
}

void StarfishBackend::set_event_callback(EventFn fn) {
    std::lock_guard<std::mutex> lock(events_mutex_);
    events_ = std::move(fn);
}

void StarfishBackend::on_event(int raw_type, long long num, const char *str) {
    const int type = normalise_event_type(raw_type, webos_major_);
    if (type == kEvUnloadCompleted) acb_state(kAcbUnloaded);  // the release worker hears it too
    if (type == 0) {  // FRAMEREADY: the presented PTS, in ns
        presented_out_us_ = num / 1000;
        const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
        presented_at_ns_ = now;
        ++frameready_;
        if (const int64_t at = seek_issued_ns_.exchange(0); at != 0) {
            WNK_LOGI(kCat, "seek landed: first frame pts %.3f s, %.0f ms after Seek", num / 1e9, (now - at) / 1e6);
        }
        // Every 5 s: the sink's own counters and how many FRAMEREADY callbacks came.
        if (now - summary_at_ns_.load() >= 5000000000LL) {
            summary_at_ns_ = now;
            WNK_LOGI(kCat, "sink: %lld displayed, %lld dropped, %lld FRAMEREADY in 5 s (presented pts %.3f s)",
                     displayed_.exchange(0), dropped_.exchange(0), frameready_.exchange(0), num / 1e9);
        }
        if (!seen_frame_.exchange(true)) WNK_LOGI(kCat, "first frame presented (pts %lld ns)", num);
        return;
    }
    const std::string s = str != nullptr ? str : "";
    // 48/49 are the sink's dropped and displayed frames per 200 ms poll (webOS 5+ numbering,
    // PlxNative's decompile): summed into one line every 5 s; everything else is rare and logged.
    if (type == kEvDropped || type == kEvDisplayed) {
        (type == kEvDropped ? dropped_ : displayed_) += num;
    } else {
        WNK_LOGI(kCat, "event type=%d num=%lld %s", raw_type, num, s.substr(0, 300).c_str());
    }
    if (acb_id_ != 0) {
        // The ACB plane learns the stream from the pipeline's own envelopes (Kodi).
        if (type == kEvVideoInfo && !s.empty()) {
            VideoColour colour;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                colour = colour_;
            }
            std::string info = rewrite_video_info(s, colour);
            if (!info.empty()) WNK_LOGI(kCat, "video info: hdrType none -> HDR10 from the container");
            if (info.empty()) info = s;
            WNK_LOGI(kCat, "AcbAPI_setMediaVideoData -> %d", acb_api().set_media_video_data(acb_id_, info.c_str(), &acb_task_));
        } else if (type == kEvAudioInfo && !s.empty() && acb_api().set_media_audio_data != nullptr) {
            acb_api().set_media_audio_data(acb_id_, s.c_str(), &acb_task_);
        } else if (type == kEvPlaying) {
            acb_state(kAcbPlaying);
        } else if (type == kEvPaused) {
            acb_state(kAcbPaused);
        }
    }
    EventFn fn;
    {
        std::lock_guard<std::mutex> lock(events_mutex_);
        fn = events_;
    }
    if (!load_completed_ && (s.find("loadCompleted") != std::string::npos || s.find("\"loaded\"") != std::string::npos)) {
        load_completed_ = true;
        if (acb_id_ != 0) {
            void *object = loading_object_.load();
            const char *media_id = object != nullptr && api().get_media_id != nullptr ? api().get_media_id(object) : nullptr;
            acb_api().set_sink_type(acb_id_, kAcbSinkMain);
            if (media_id != nullptr) acb_api().set_media_id(acb_id_, media_id);
            WNK_LOGI(kCat, "ACB media id %s", media_id != nullptr ? media_id : "(none)");
            acb_state(kAcbLoaded);
            std::lock_guard<std::mutex> lock(mutex_);
            place_acb_locked();
        }
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
    // A stale bus role after a firmware update leaves Load hanging in ~StarfishMediaAPIs (client-webos).
    if (std::string why; !wnk::platform::media_role_ok(&why)) {
        *error = why;
        WNK_LOGW(kCat, "%s", why.c_str());
        return false;
    }
    // A Load while the last one's decoders are still being given back waited for them forever on
    // webOS 6.5.3; past the deadline the play fails with a reason instead.
    if (!wait_for_releases(error)) {
        WNK_LOGW(kCat, "%s", error->c_str());
        return false;
    }
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
    if (format.audio) {
        l.audio = format.audio->codec;
        l.audio_channels = format.audio->channels;
        l.audio_rate = format.audio->sample_rate;
    }
    if (const Limit &lim = limits_[static_cast<size_t>(l.video)]; lim.known && decoder_listed(lim.width, lim.height)) {
        l.decoder_max_width = lim.width;
        l.decoder_max_height = lim.height;
    }
    // Not on webOS 3: a declared Load never started its clock on 3.9.3 (PlxNative).
    if (options_.dolby_vision && !(webos_major_ >= 1 && webos_major_ <= 3)) {
        l.dolby_vision_profile = format.video->dolby_vision_profile;
    }
    l.atmos = options_.atmos && format.audio && format.audio->atmos;
    int64_t pcm_lead_us = 0;
    if (format.audio && format.audio->codec == CodecId::PcmS16) {
        int ms = options_.pcm_lead_ms;
        std::string why = "option";
        if (ms < 0) {
            const AudioStack stack = detect_audio_stack();
            std::string output;
            if (stack == AudioStack::Liblxa || stack == AudioStack::AudioSinkNew) {
                const auto r = wnk::platform::luna_call_sync("luna://com.webos.settingsservice/getSystemSettings",
                                                             R"({"category":"sound","keys":["soundOutput"]})", 1000);
                if (r.status == wnk::platform::LunaStatus::Ok) output = sound_output_from_reply(r.payload);
            }
            ms = pcm_lead_ms_for(stack, output);
            why = std::string("stack ") + audio_stack_name(stack) + ", output " + (output.empty() ? "unknown" : output);
        }
        pcm_lead_us = static_cast<int64_t>(ms) * 1000;
        WNK_LOGI(kCat, "pcm: audio stamped +%d ms (%s)", ms, why.c_str());
    }
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
    presented_out_us_ = kNoTime;
    {
        // Before Load: its events (the ACB placement at loadCompleted, the video info) read them.
        std::lock_guard<std::mutex> lock(mutex_);
        colour_ = format.video->colour;
        format_ = format;
    }
    loading_object_ = object;
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
    // Static HDR metadata only the container states (VP9 has no SEI of its own): after Load, as Kodi.
    if (const std::string hdr = build_hdr_payload(format.video->colour); !hdr.empty()) {
        if (api().set_hdr_info != nullptr) {
            const std::string payload =
                webos_major_ >= 1 && webos_major_ <= 4 ? build_hdr_payload(format.video->colour, true) : hdr;
            WNK_LOGI(kCat, "setHdrInfo %s -> %d", payload.c_str(), api().set_hdr_info(object, payload.c_str()) & 0xFF);
        } else if (acb_id_ != 0) {
            WNK_LOGI(kCat, "no setHdrInfo: container HDR10 goes into the ACB video info");
        } else {
            WNK_LOGI(kCat, "no setHdrInfo on this firmware: container HDR metadata not declared");
        }
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
        native_seek_pending_ = false;
        pcm_lead_us_ = pcm_lead_us;
        splice_first_out_us_ = kNoTime;
    }
    // Placing the window is an SDL call: the main thread does it through set_area().
    return true;
}

MediaBackend::Feed StarfishBackend::feed(const Packet &p) {
    if (p.type != TrackType::Video && p.type != TrackType::Audio) return Feed::Accepted;
    void *object;
    int64_t out_us;
    bool play = false, replay = false;
    int64_t seek_out_us = kNoTime;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loaded_) return Feed::Error;
        if (p.type == TrackType::Audio && !format_.audio) return Feed::Accepted;
        object = object_;
        const int64_t sink_pts = to_sink(p.pts_us);  // content time divided by the playback speed
        if (native_seek_pending_ && sink_pts != kNoTime) {
            // Content time from here on (kept non-negative: Feed reads pts as unsigned), and the
            // pipeline's new segment starts at this packet.
            native_seek_pending_ = false;
            splice_.reanchor(sink_pts < 0 ? -sink_pts : 0);
            seek_out_us = sink_pts + splice_.shift_us();
            presented_out_us_ = kNoTime;  // until the new segment presents a frame
        }
        // After a seek the new content goes after the decoder's clock if it ran past what was fed.
        const int64_t before = splice_.shift_us();
        const int64_t clock = clock_out_us(paused_);
        out_us = splice_.map(sink_pts, clock);
        // The PCM lead applies to what the sink is given only (PlxNative): not to the splice or the
        // native seek's target, and the position is read from the presented video.
        if (p.type == TrackType::Audio && out_us != kNoTime) out_us += pcm_lead_us_;
        if (splice_.shift_us() != before) {
            log_next_video_ = true;
            splice_first_out_us_ = out_us;
        }
        if (p.type == TrackType::Video && log_next_video_ && out_us != kNoTime) {
            log_next_video_ = false;
            WNK_LOGI(kCat, "splice: first video pts %.3f s -> %.3f s%s", p.pts_us / 1e6, out_us / 1e6, p.key ? " (key)" : "");
        }
        if (splice_.shift_us() != before) {
            WNK_LOGI(kCat, "splice: %s pts %.3f s -> %.3f s (decoder clock %.3f s, shift %+.3f s)",
                     p.type == TrackType::Video ? "video" : "audio", p.pts_us / 1e6, out_us / 1e6,
                     clock == kNoTime ? -1.0 : clock / 1e6, splice_.shift_us() / 1e6);
        }
        // Play issued before loadCompleted is queued; it is issued once more after it (samples README).
        if (!paused_ && !play_issued_) play = play_issued_ = true;
        if (!paused_ && load_completed_ && !replayed_) replay = replayed_ = true;
    }
    if (seek_out_us != kNoTime) {
        // On the feeding thread, before the packet: Feed takes no lock and would race the flush.
        const std::string millis = std::to_string(seek_out_us / 1000);
        const auto t = std::chrono::steady_clock::now();
        seek_issued_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
        const bool ok = (api().seek(object, millis.c_str()) & 0xFF) != 0;
        WNK_LOGI(kCat, "Seek(%s) -> %d (%.1f ms; %s pts %.3f s)", millis.c_str(), ok ? 1 : 0,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(),
                 p.type == TrackType::Video ? "video" : "audio", p.pts_us / 1e6);
        if (!ok) {
            // A refused Seek can leave every later Feed rejected (PlxNative's decompile): reload, and
            // splice for the rest of this backend's life.
            StreamFormat format;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                native_seek_refused_ = true;
                format = format_;
            }
            WNK_LOGW(kCat, "Seek refused: reloading, and splicing seeks from now on");
            std::string err;
            if (!load(format, &err)) {
                WNK_LOGW(kCat, "reload after a refused Seek failed: %s", err.c_str());
                return Feed::Error;
            }
            return feed(p);
        }
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
    Feed result = Feed::Error;
    if (s == nullptr) {
        return Feed::Error;
    } else if (std::strstr(s, "BufferFull") != nullptr) {
        result = Feed::Full;
    } else if (std::strstr(s, "Ok") != nullptr) {
        result = Feed::Accepted;
    } else {
        WNK_LOGW(kCat, "Feed reply \"%s\"", s);
    }
    if (api().feed_cow && api().cow_string_dtor != nullptr) api().cow_string_dtor(reply);
    return result;
}

bool StarfishBackend::splice_settled() const {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loaded_ || paused_) return true;  // a stopped clock cannot run past anything
    }
    const int64_t first = splice_first_out_us_.load();
    const int64_t at = presented_out_us_.load();
    return first == kNoTime || (at != kNoTime && at >= first - 50000);
}

int64_t StarfishBackend::clock_out_us(bool paused) const {
    const int64_t out = presented_out_us_.load();
    if (out == kNoTime) return kNoTime;
    if (paused) return out;
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    return out + std::max<int64_t>(0, now - presented_at_ns_.load()) / 1000;
}

bool StarfishBackend::flush(std::string *) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) return true;
    if (native_seek_ && !native_seek_refused_) {
        native_seek_pending_ = true;  // the next packet issues Seek at its own timestamp
    } else {
        splice_.splice();  // later packets continue the timeline; nothing is dropped
    }
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
    auto r = std::make_shared<Release>();
    r->started = std::chrono::steady_clock::now();
    r->deadline = options_.release_deadline;
    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        g_releases[epoch] = r;
    }
    std::thread([r, object, epoch] {
        const int rc = api().unload(object);
        const double unload_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r->started).count();
        // The destructor is what gives the decoders back (a Load after an Unload without it waited for
        // them forever on webOS 6.5.3), and PlxNative runs it only after UnloadCompleted (type 23).
        bool completed;
        {
            std::unique_lock<std::mutex> lock(r->m);
            completed = r->cv.wait_for(lock, std::chrono::seconds(3), [&r] { return r->completed; });
        }
        {
            std::lock_guard<std::mutex> lock(g_registry_mutex);
            g_registry.erase(epoch);  // later callbacks are dropped
        }
        if (completed) api().dtor(object);
        WNK_LOGI(kCat, "Unload -> %d after %.1f ms, %s (%.1f ms)", rc, unload_ms,
                 completed ? "destroyed" : "no UnloadCompleted: object kept",
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r->started).count());
        {
            std::lock_guard<std::mutex> lock(r->m);
            r->done = true;
        }
        r->cv.notify_all();
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        g_releases.erase(epoch);
    }).detach();
    std::unique_lock<std::mutex> lock(r->m);
    if (!r->cv.wait_until(lock, r->started + r->deadline, [&r] { return r->done; })) {
        lock.unlock();
        // The object stays with the worker; this backend no longer hears its events, so it may go.
        std::lock_guard<std::mutex> rl(g_registry_mutex);
        g_registry.erase(epoch);
        WNK_LOGW(kCat, "Unload overdue: still in the firmware after %lld ms; the object is parked",
                 static_cast<long long>(r->deadline.count()));
    }
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
    return from_sink(splice_.unmap(out));
}

void StarfishBackend::set_area(const Rect &pixels) {
    std::lock_guard<std::mutex> lock(mutex_);
    area_ = pixels;
    place_locked();
}

void StarfishBackend::place_locked() {
    if (plane_ == VideoPlane::Acb) {
        if (loaded_ && load_completed_) place_acb_locked();
        return;
    }
    if (window_id_.empty() || !usable()) return;
    // Source: the coded picture (or the area until the stream is known); destination: the area.
    SdlRect src{0, 0, format_.video ? format_.video->width : static_cast<int>(area_.w),
                format_.video ? format_.video->height : static_cast<int>(area_.h)};
    SdlRect dst{static_cast<int>(area_.x), static_cast<int>(area_.y), static_cast<int>(area_.w), static_cast<int>(area_.h)};
    const int rc = api().set_window(window_id_.c_str(), &src, &dst);
    WNK_LOGI(kCat, "place %dx%d -> %d,%d %dx%d rv=%d", src.w, src.h, dst.x, dst.y, dst.w, dst.h, rc);
}

void StarfishBackend::place_acb_locked() {
    if (acb_id_ == 0 || area_.w <= 0 || area_.h <= 0) return;
    // Source: the coded picture; destination: the area, in the 1920x1080 app space ACB uses (Kodi).
    const long src_w = format_.video ? format_.video->width : 1920, src_h = format_.video ? format_.video->height : 1080;
    const int rc = acb_api().set_custom_display_window(acb_id_, 0, 0, src_w, src_h, static_cast<long>(area_.x),
                                                       static_cast<long>(area_.y), static_cast<long>(area_.w),
                                                       static_cast<long>(area_.h), false, &acb_task_);
    WNK_LOGI(kCat, "ACB place %ldx%ld -> %.0f,%.0f %.0fx%.0f rv=%d", src_w, src_h, area_.x, area_.y, area_.w, area_.h, rc);
}

}  // namespace wnk::media
