// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaBackend on LG's StarfishMediaAPIs (libplayerAPIs.so.1, webOS 5.3.1+), the buffer-feed pipeline
// LG's own apps, Kodi, Moonlight (ss4s) and PlxNative use. Compared with NDL v2 it has a real pause,
// reports the presented position, applies backpressure ("BufferFull"), takes AAC, AC-3 and E-AC-3
// directly, and is told the stream's frame rate (adaptiveStreaming.maxFrameRate and esInfo
// videoFps): PlxNative measured 24p presented at 13 fps when declared as 60 and at 24.1 when
// declared as 24, which is the 24p judder NDL shows on this kit's TV.
//
// Design follows PlxNative's src/starfish.c and player/engine.rs (Copyright the PlxNative authors,
// GPL-3.0-or-later; THIRD-PARTY-NOTICES.md):
//   * The C++ class is reached through its mangled symbols, dlsym'd from a dlopen'd libplayerAPIs
//     (no DT_NEEDED). The object is constructed in place in a 64 KiB, 16-byte aligned block (its real
//     size is unknown, so never new/delete) with uid NULL. Each Load gets a fresh block that is never
//     freed or reused: a late callback carries only the object's address.
//   * Feed returns a std::string through a hidden sret pointer; its char* is at offset 0 (the short
//     replies "Ok" / "BufferFull" live in the small-string buffer).
//   * Video binds through an SDL-webOS exported window: created on the main thread, its id goes into
//     the Load payload as option.windowId, and SDL_webOSSetExportedWindow places it.
//   * Events: FRAMEREADY (type 0) carries the presented PTS; loadCompleted arrives as a string; type
//     18 before any picture is a refusal. Types above 0x1c are numbered two higher on webOS 5+ than
//     on 4 (Kodi, PlxNative), so none of those is relied on.
//   * Pause and Play are real. Seeks splice the timeline like the NDL backend (no flush: PlxNative
//     found Starfish's flush leaves a stale segment and wedges the sink after ~48 s).
//
// Unverified until measured on a TV (docs/compatibility/records.json): everything specific to this
// implementation. PlxNative's path was verified on webOS 6.5.2 (2026-08-11, its issue #22).
#pragma once

#include "wnk/media/media.hpp"
#include "wnk/media/pts_splice.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace wnk::media {

struct StarfishOptions {
    std::string app_id;  // $APPID when empty
};

// Load payload inputs, for build_load_payload.
struct StarfishLoad {
    std::string app_id;
    std::string window_id;
    CodecId video = CodecId::Unknown;
    int width = 0, height = 0;
    double frame_rate = 0;
    CodecId audio = CodecId::Unknown;  // Unknown: no audio
};

class StarfishBackend : public MediaBackend {
 public:
    // Main thread: creates the exported window.
    explicit StarfishBackend(StarfishOptions options = {});
    ~StarfishBackend() override;  // main thread

    // libplayerAPIs.so.1 with every entry point, and SDL-webOS's exported window.
    static bool available(std::string *why = nullptr);

    const char *name() const override { return "starfish"; }
    BackendCaps caps() const override;
    void set_event_callback(EventFn fn) override;
    bool load(const StreamFormat &format, std::string *error) override;
    Feed feed(const Packet &p) override;
    bool flush(std::string *error) override;
    void unload() override;
    void set_area(const Rect &pixels) override;  // main thread
    int64_t position_us() const override;
    bool pause(bool paused) override;
    void end_of_stream() override;

    // Pure helpers, exposed for tests.
    static std::string build_load_payload(const StarfishLoad &l);
    static const char *codec_string(CodecId c);  // "H264", "AC3 PLUS", ... or nullptr
    static int frame_rate_class(double fps);      // 24, 25, 30, 50, 60 or the rate rounded up
    static void frame_rate_fraction(double fps, int64_t *num, int64_t *den);  // 24000/1001, 25/1, ...

    // Called from the library's thread through the epoch registry.
    void on_event(int type, long long num, const char *str);

 private:
    void place_locked();

    StarfishOptions options_;
    std::string window_id_;
    mutable std::mutex mutex_;  // format_, area_, object_
    StreamFormat format_;
    Rect area_{0, 0, 1920, 1080};
    void *object_ = nullptr;     // the current Load's object (never freed)
    unsigned epoch_ = 0;
    bool loaded_ = false;
    bool play_issued_ = false;
    bool replayed_ = false;  // Play issued again after loadCompleted
    bool paused_ = false;
    PtsSplice splice_;
    std::atomic<bool> load_completed_{false};
    std::atomic<bool> seen_frame_{false};
    std::atomic<int64_t> presented_out_us_{kNoTime};
    std::atomic<int> events_logged_{0};
    std::mutex events_mutex_;
    EventFn events_;
};

}  // namespace wnk::media
