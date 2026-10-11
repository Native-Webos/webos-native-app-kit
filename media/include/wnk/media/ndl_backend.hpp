// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaBackend on LG's NDL DirectMedia v2 (webOS 5.3.1+; docs/research/NATIVE-MEDIA.md).
//
//   * libNDL_directmedia.so.1 is dlopened (DT_NEEDED breaks startup where it is missing) and every
//     call goes through one process-wide mutex: NDL is not thread-safe (punktfunk).
//   * NDL_DirectMediaInit runs lazily in the first load(): warm init at startup reportedly makes the
//     first load fail (punktfunk). NDL_DirectMediaQuit runs when the last backend is destroyed, at
//     exit, and from the crash handler, so the decoder is never left acquired.
//   * flush() is unload + load: an NDL flush can silently kill audio for the session (punktfunk), and
//     must not happen before load completed.
//   * NDL v2 has no pause and no position query (caps say so); the controller handles both.
//   * Audio: PCM S16 stereo and MP3 are fed directly. Opus is in the v2 header but its stream-header
//     format is unverified, so the source decodes nothing to Opus and Opus files play without sound.
//
// Unverified until measured on a TV (docs/compatibility/records.json): the PTS unit (µs per webosbrew
// samples; ms since load per ss4s and punktfunk), the PCM layout string, the coordinate space of
// NDL_DirectVideoSetArea, and which load-callback events this firmware sends. Every NDL call is
// logged with its result for exactly that reason.
#pragma once

#include "wnk/media/media.hpp"

#include <cstdint>
#include <mutex>
#include <string>

namespace wnk::media {

enum class PtsUnit : uint8_t { Microseconds, Milliseconds };

struct NdlOptions {
    PtsUnit pts_unit = PtsUnit::Microseconds;
    std::string app_id;  // $APPID when empty
};

class NdlBackend : public MediaBackend {
 public:
    explicit NdlBackend(NdlOptions options = {});
    ~NdlBackend() override;

    // Whether libNDL_directmedia.so.1 with the v2 entry points can be loaded on this device.
    static bool available(std::string *why = nullptr);
    // Tell NDL whether the app is in the foreground (call from lifecycle transitions). Safe before init.
    static void set_app_foreground(bool foreground);

    const char *name() const override { return "ndl-v2"; }
    BackendCaps caps() const override;
    void set_event_callback(EventFn fn) override;
    bool load(const StreamFormat &format, std::string *error) override;
    Feed feed(const Packet &p) override;
    bool flush(std::string *error) override;
    void unload() override;
    void set_area(const Rect &pixels) override;

    // NDL wants an enum, not a rate; 0 if the rate has no enum value.
    static int pcm_rate_enum(int hertz);
    static long long convert_pts(int64_t pts_us, PtsUnit unit);

 private:
    bool load_locked(const StreamFormat &format, std::string *error);
    void unload_locked();
    void emit(BackendEvent e, const std::string &detail);
    static void load_callback(int type, long long num, const char *str);
    static void resource_released(const char *type);

    NdlOptions options_;
    StreamFormat format_;
    bool loaded_ = false;
    bool area_set_ = false;
    Rect area_;
    std::mutex events_mutex_;
    EventFn events_;
    uint64_t video_fed_ = 0, audio_fed_ = 0;
};

}  // namespace wnk::media
