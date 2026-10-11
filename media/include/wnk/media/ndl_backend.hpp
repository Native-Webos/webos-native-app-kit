// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaBackend on LG's NDL DirectMedia v2 (webOS 5.3.1+; docs/research/NATIVE-MEDIA.md).
//
//   * libNDL_directmedia.so.1 is dlopened (DT_NEEDED breaks startup where it is missing) and every
//     call goes through one process-wide mutex: NDL is not thread-safe (punktfunk).
//   * NDL_DirectMediaInit runs lazily in the first load(): warm init at startup reportedly makes the
//     first load fail (punktfunk). NDL_DirectMediaQuit runs when the last backend is destroyed, at
//     exit, and from the crash handler, so the decoder is never left acquired.
//   * NDL v2 has no pause, no usable flush and no position query. Pause therefore only stops
//     feeding: what is queued plays out and the picture holds on the last frame (pause_drains).
//     A seek drops nothing: later packets get PTS shifted to continue 200 ms after the last one
//     fed, so the queue plays out and the new content follows (seek_splices). Both keep the
//     decoder loaded, so there is no black picture and audio survives; measured on an OLED65C1PTB
//     (webOS 6.5.3). NDL_DirectVideoFlushRenderBuffer was tried for seeks: video flushed in 4-15 ms
//     but sound was gone for the rest of the session, as punktfunk warned for NDL flushes.
//   * Audio: PCM S16 stereo only (measured: AAC and AC-3 decoded to PCM play in sync). MP3 passed
//     through played silently, so it is decoded too. Opus is in the v2 header but unverified.
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

// The PTS mapping behind splice seeks: after splice(), the next packet with a PTS is placed `gap_us`
// after the highest PTS mapped so far and everything after it keeps that offset, so the decoder's
// timeline only moves forward. Pure, for tests.
class PtsSplice {
 public:
    explicit PtsSplice(int64_t gap_us) : gap_us_(gap_us) {}
    void reset() { *this = PtsSplice(gap_us_); }
    void splice() { pending_ = true; }
    // kNoTime stays kNoTime.
    int64_t map(int64_t pts_us);
    int64_t shift_us() const { return shift_us_; }

 private:
    int64_t gap_us_;
    int64_t shift_us_ = 0;
    bool pending_ = false;
    int64_t max_out_us_ = kNoTime;
};

struct NdlOptions {
    PtsUnit pts_unit = PtsUnit::Microseconds;
    std::string app_id;  // $APPID when empty
    // Unload and reload the decoder to pause and to seek, as before pause/splice were measured.
    // The picture goes black each time; kept for comparison and as a fallback.
    bool reload_to_pause_and_seek = false;
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
    bool pause(bool paused) override;

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
    PtsSplice splice_;
};

}  // namespace wnk::media
