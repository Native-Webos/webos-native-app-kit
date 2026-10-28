// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaBackend on LG's StarfishMediaAPIs (libplayerAPIs.so.1, webOS 3.4+), the buffer-feed pipeline
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
//     size is unknown, so never new/delete) with uid NULL. Each Load gets a fresh block. After Unload
//     and UnloadCompleted (type 23) the destructor runs, which releases the decoders; the block itself
//     is never freed or reused, as a late callback carries only the object's address.
//   * Feed returns a std::string through a hidden sret pointer; its char* is at offset 0 (the short
//     replies "Ok" / "BufferFull" live in the small-string buffer). webOS 3.x builds the library with
//     the pre-C++11 copy-on-write string (Feed without the B5cxx11 tag): its reply is released with
//     the firmware's own ~basic_string (PlxNative).
//   * Video binds through an SDL-webOS exported window on webOS 5+: created on the main thread, its
//     id goes into the Load payload as option.windowId, and SDL_webOSSetExportedWindow places it.
//     webOS 3.x and 4.x have no exported window; there the video plane is LG's app-switching control
//     block (libAcbAPI), as Kodi and ss4s do it: an ACB of type MSE gets the pipeline's media id, sink,
//     state changes and video-info envelopes, and setCustomDisplayWindow places the picture.
//   * Events: FRAMEREADY (type 0) carries the presented PTS; loadCompleted arrives as a string; type
//     18 before any picture is a refusal. Types above 0x1c are numbered two lower before webOS 5
//     (Kodi's `type += 2`): normalise_event_type maps them to the webOS 5+ numbers.
//   * Unload runs on a release worker (Unload, UnloadCompleted, destructor). unload() waits at most
//     StarfishOptions::release_deadline (10 s); past it the object is parked with the worker and the
//     next Load fails with release_timeout until the worker returns (PlxNative #1116: an Unload that
//     never returned froze its UI for good).
//   * Pause and Play are real. Seeks go through LG's public Seek(millis) where it is known to work
//     (webOS 5+, webOS 3; PlxNative #568, decompiled on 4.5): flush() marks one pending, and the first
//     packet after it issues Seek at its own timestamp, on the feeding thread, before it is fed; the
//     timeline is then content time (PtsSplice::reanchor). A refused Seek reloads the decoder and the
//     backend splices from then on. Splicing (webOS 4, or StarfishOptions::native_seek off) moves the
//     timeline like the NDL backend instead (no flush: PlxNative found Starfish's flush leaves a stale
//     segment and wedges the sink after ~48 s).
//
//   * VP9 and AV1 are offered where getMaxVideoResolution reports a decoder for them; its answer also
//     bounds the envelope. Container HDR10/HLG metadata goes through setHdrInfo after Load (webOS 5+,
//     Kodi's payload). E-AC-3 carries ac3PlusInfo (Kodi). On the ACB plane (no setHdrInfo) a PQ
//     stream the pipeline calls SDR has its video-info envelope rewritten to HDR10 with mediaSei and
//     mediaVui before it goes to ACB (PlxNative's vp9_hdr.rs).
//   * Dolby Vision is never declared on webOS 3: PlxNative measured a declared Load that never
//     started its clock on 3.9.3 although the TV's config said supported.
//
// Unverified until measured on a TV (docs/compatibility/records.json): everything specific to this
// implementation. PlxNative's path was verified on webOS 6.5.2 (2026-08-11, its issue #22).
#pragma once

#include "wnk/media/media.hpp"
#include "wnk/media/pts_splice.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace wnk::media {

struct StarfishOptions {
    std::string app_id;  // $APPID when empty
    // Declare what the stream carries to the pipeline (PlxNative docs/dolby-vision.md, read from the
    // TV's libpf/libcbe): contents.DolbyHdrInfo for single-layer Dolby Vision HEVC, which makes the
    // pipeline add dolby-vision=TRUE to the video caps, and contents.immersive "ATMOS" for E-AC-3 JOC.
    // libpf acts on the nodes without checking the panel, so set these only when the TV says it
    // supports them (DeviceMediaCaps: an exact true).
    bool dolby_vision = false;
    bool atmos = false;
    // The webOS major release (4 for 4.4.2). -1 reads it from /var/run/nyx/os_info.json; 0 is
    // unknown, which is treated as webOS 5+.
    int webos_major = -1;
    // How long unload() waits for LG's Unload and the destructor before it parks the object, and how
    // long the next Load waits for such a release before it fails with release_timeout.
    std::chrono::milliseconds release_deadline{10000};
    // Seek with LG's public StarfishMediaAPIs::Seek (it flushes and re-anchors the pipeline, PlxNative
    // #568) where that is known to work: webOS 5+ (the exported-window plane) and webOS 3. Off, or on
    // webOS 4, or once a Seek is refused, seeks splice the timeline instead.
    bool native_seek = true;
};

// What shows the decoded picture.
enum class VideoPlane : uint8_t { None, ExportedWindow, Acb };

// Load payload inputs, for build_load_payload.
struct StarfishLoad {
    std::string app_id;
    std::string window_id;
    CodecId video = CodecId::Unknown;
    int width = 0, height = 0;
    double frame_rate = 0;
    CodecId audio = CodecId::Unknown;  // Unknown: no audio
    int dolby_vision_profile = -1;     // >= 0: declare DolbyHdrInfo with this profileId
    bool atmos = false;                // declare immersive ATMOS
    int audio_channels = 0, audio_rate = 0;
    int decoder_max_width = 0, decoder_max_height = 0;  // what the TV reports for the codec; 0 unknown
};

class StarfishBackend : public MediaBackend {
 public:
    // Main thread: creates the exported window.
    explicit StarfishBackend(StarfishOptions options = {});
    ~StarfishBackend() override;  // main thread

    // libplayerAPIs.so.1 with every entry point (either Feed ABI).
    static bool available(std::string *why = nullptr);
    // The largest picture this TV's decoder takes for a Starfish codec name ("H264", "H265", "VP9",
    // "AV1"), from libplayerAPIs' smp::util::getMaxVideoResolution. False when Starfish or the
    // function is missing, or the TV does not say.
    static bool max_video_resolution(const char *codec, int *width, int *height, int *frame_rate);

    const char *name() const override { return "starfish"; }
    VideoPlane plane() const { return plane_; }
    bool native_seek() const;  // seeks use LG's Seek now
    int webos_major() const { return webos_major_; }
    BackendCaps caps() const override;
    void set_event_callback(EventFn fn) override;
    bool load(const StreamFormat &format, std::string *error) override;
    Feed feed(const Packet &p) override;
    bool flush(std::string *error) override;
    void unload() override;
    void set_area(const Rect &pixels) override;  // main thread
    int64_t position_us() const override;
    bool pause(bool paused) override;
    bool splice_settled() const override;
    void end_of_stream() override;

    // Pure helpers, exposed for tests.
    static std::string build_load_payload(const StarfishLoad &l);
    static const char *codec_string(CodecId c);  // "H264", "AC3 PLUS", ... or nullptr
    static int frame_rate_class(double fps);      // 24, 25, 30, 50, 60 or the rate rounded up
    static void frame_rate_fraction(double fps, int64_t *num, int64_t *den);  // 24000/1001, 25/1, ...
    // setHdrInfo payload for the container's HDR10/HLG metadata, or "" when there is none to declare.
    // legacy_keys: mediaSei/mediaVui, the names before webOS 5 (Kodi), instead of sei/vui.
    static std::string build_hdr_payload(const VideoColour &colour, bool legacy_keys = false);
    // A pipeline video-info envelope ({"video":{...,"hdrType":"none",...}}) rewritten to declare
    // HDR10 when the container says PQ and the pipeline said nothing; "" leaves it as it was.
    static std::string rewrite_video_info(const std::string &envelope, const VideoColour &colour);
    // The webOS major from os_info.json's webos_release ("4.4.2" -> 4), 0 when it cannot say.
    static int webos_major_from_os_info(const std::string &json);
    // The webOS 5+ number for a callback type a firmware of this major sent.
    static int normalise_event_type(int type, int webos_major);
    // Whether a getMaxVideoResolution answer names a real decoder (larger than the default it gives
    // for any name, 1920x1088).
    static bool decoder_listed(int width, int height);

    // Called from the library's thread through the epoch registry.
    void on_event(int type, long long num, const char *str);

 private:
    void place_locked();
    void place_acb_locked();
    bool open_exported_window();
    bool open_acb();
    void acb_state(int play_state);

    StarfishOptions options_;
    struct Limit {
        bool known = false;
        int width = 0, height = 0, frame_rate = 0;
    };
    Limit limits_[16];  // by CodecId, measured at construction
    int webos_major_ = 0;
    VideoPlane plane_ = VideoPlane::None;
    std::string window_id_;
    long acb_id_ = 0;  // the ACB plane's handle, 0 without one
    long acb_task_ = 0;
    std::atomic<void *> loading_object_{nullptr};  // the object of the Load the events belong to
    VideoColour colour_;                           // the current stream's container colour; mutex_
    mutable std::mutex mutex_;  // format_, area_, object_
    StreamFormat format_;
    Rect area_{0, 0, 1920, 1080};
    void *object_ = nullptr;     // the current Load's object (never freed)
    unsigned epoch_ = 0;
    bool loaded_ = false;
    bool play_issued_ = false;
    bool replayed_ = false;  // Play issued again after loadCompleted
    bool paused_ = false;
    bool native_seek_ = false;          // chosen at construction
    bool native_seek_refused_ = false;  // a Seek returned false: splice from then on (mutex_)
    bool native_seek_pending_ = false;  // flush() asked; the next packet issues it (mutex_)
    PtsSplice splice_;
    std::atomic<bool> load_completed_{false};
    std::atomic<bool> seen_frame_{false};
    std::atomic<int64_t> presented_out_us_{kNoTime};
    std::atomic<int64_t> presented_at_ns_{0};  // steady clock when presented_out_us_ arrived
    std::atomic<int64_t> seek_issued_ns_{0};   // steady clock of the last native Seek until its first frame
    std::atomic<int64_t> splice_first_out_us_{kNoTime};  // where the content after the last splice starts
    // Where the decoder's clock is now: the last presented PTS plus the time since (when playing).
    int64_t clock_out_us(bool paused) const;
    std::atomic<long long> dropped_{0}, displayed_{0}, frameready_{0};  // since the last summary
    std::atomic<int64_t> summary_at_ns_{0};
    bool log_next_video_ = false;  // guarded by mutex_
    std::mutex events_mutex_;
    EventFn events_;
};

}  // namespace wnk::media
