// SPDX-License-Identifier: GPL-3.0-or-later
//
// PlaybackController: drives a MediaSource into a MediaBackend on its own thread.
//
//   Idle ─open─► Opening ─► Ready ─play─► Playing ⇄ Paused
//                  │          │              │  │
//                  │          └─seek─► Seeking ┘  └─(source ends, buffer drained)─► Ended
//                  └──────────────(any failure)──────────────────────────────────► Error
//                                 (a BackendEvent::Error too: the decoder refused the stream)
//   any state ─stop─► Stopped
//
// A reader thread reads packets into one lane per stream (video, audio), up to max_queue_us ahead;
// the playback thread feeds each lane when its next packet is due, so a packet held back on one
// stream never blocks the other however the file interleaves them, and a read that waits (a live
// segment not published yet, a slow server) never stops what is queued from reaching the decoder.
// The playback thread retries while the backend reports Full and never feeds more than
// max_lead_us ahead of the playback clock (decoders that accept everything would otherwise receive
// the whole file at once). After a load or seek it primes: nothing is fed until both lanes hold a
// little (prime_video_us / prime_audio_us), then that goes in as one burst, so the decoder does not
// start on a trickle (PlxNative's prime-then-play). Commands from the main thread are queued and applied
// between packets; a blocked read or open is interrupted for stop and seek.
//
// Pause: when the backend cannot pause at all the controller stops feeding, unloads the decoder
// and, on resume, reloads and seeks back to where playback stopped. A backend whose pause drains
// (NDL: it only stops being fed) stops on the last frame fed, so that is where the clock stops.
//
// Buffering: when a stream's lane is empty and the decoder holds less than rebuffer_low_us of it, a
// backend that can pause is paused and the clock stops (PlaybackSnapshot::buffering), so nothing fed
// later arrives late for a decoder whose clock ran on (Starfish then drops every such frame). Once
// every lane holds rebuffer_goal_us, or the source has ended, playback resumes where it stopped, as
// a browser's media element does while it waits for data. The play state stays Playing.
//
// Seek: the source repositions and the backend is flushed. A backend that splices instead keeps
// presenting what it holds first, so the clock for the new content starts only after that backlog.
//
// Position: from the backend if it reports one, else estimated on a clock started at the first
// packet after load/seek (offset by that packet's PTS) and frozen while paused. The estimate is
// marked as such in the snapshot. While a seek is on its way (asked for, and its content not yet
// fed or, with a splicing backend, not yet on screen) the position is its target, so a time shown
// from it follows the remote at once instead of the picture that is still being replaced.
//
// Subtitles: every cue the source decodes goes into a SubtitleStore (all subtitle tracks, kept across
// seeks) instead of the backend; subtitles_now() answers what the selected track shows at the
// current position, so switching tracks needs no seek.
//
// Threading: public methods are main-thread calls and never block on the playback thread except
// stop()/~PlaybackController, which join it with a deadline. State changes are posted to the
// MainDispatcher; snapshot() can be read at any time.
#pragma once

#include "wnk/async/async.hpp"
#include "wnk/media/media.hpp"
#include "wnk/media/subtitles.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace wnk::media {

enum class PlayState : uint8_t { Idle, Opening, Ready, Playing, Paused, Seeking, Ended, Error, Stopped };
const char *play_state_name(PlayState s);

struct PlaybackSnapshot {
    PlayState state = PlayState::Idle;
    int64_t position_us = kNoTime;
    bool position_estimated = true;
    int64_t duration_us = kNoTime;
    MediaInfo info;  // after Opening
    StreamFormat format;
    int video_track = -1, audio_track = -1;  // stream indexes in `info` being played, -1 for none
    std::string error;
    uint64_t video_packets = 0, audio_packets = 0;  // fed since open
    uint64_t full_retries = 0;  // feed() returned Full
    int64_t last_video_pts_us = kNoTime;
    int64_t last_audio_pts_us = kNoTime;  // of the latest audio packet fed
    uint64_t untimed_packets = 0;         // fed without a timestamp (a source bug if it grows)
    bool buffering = false;               // Playing, held until enough is queued (see above)
    uint64_t rebuffers = 0;               // times playback stopped to buffer since open
};

struct PlaybackOptions {
    bool autoplay = true;          // start feeding as soon as the backend is loaded
    int video_track = -2;          // -2: best, -1: none, else a stream index
    int audio_track = -2;
    std::chrono::milliseconds full_backoff{8};  // wait after Full before retrying
    // Never feed more than this far ahead of the playback clock, whatever the backend says. NDL v2
    // was measured accepting a whole 15 s clip in 0.6 s without ever reporting Full (OLED65C1PTB,
    // webOS 6.5.3), so its own backpressure cannot be relied on. 0 disables.
    int64_t max_lead_us = 1000000;
    // Read-ahead: lanes stop filling at this span per lane or this many bytes in all.
    int64_t max_queue_us = 4000000;
    size_t max_queue_bytes = 64u << 20;
    // Priming after a load or seek; video_max ends it when a file has no audio near the start.
    int64_t prime_video_us = 700000;
    int64_t prime_audio_us = 300000;
    int64_t prime_video_max_us = 2500000;
    // Buffering: entered when a lane is empty and the decoder holds less than rebuffer_low_us of its
    // stream; left when every lane holds rebuffer_goal_us. Backends without pause never buffer.
    int64_t rebuffer_low_us = 300000;
    int64_t rebuffer_goal_us = 2000000;
    // With a splicing backend nothing fed is ever dropped, so a burst of seeks would play a piece of
    // every target. After a seek, feeding waits until no further seek has been asked for this long.
    std::chrono::milliseconds seek_settle{400};
    std::chrono::milliseconds stop_deadline{2000};
};

class PlaybackController {
 public:
    using StateFn = std::function<void(PlayState from, PlayState to, const PlaybackSnapshot &)>;

    // The controller owns source and backend. `main` receives state change callbacks.
    PlaybackController(std::unique_ptr<MediaSource> source, std::unique_ptr<MediaBackend> backend,
                       async::MainDispatcher &main, PlaybackOptions options = {});
    ~PlaybackController();
    PlaybackController(const PlaybackController &) = delete;
    PlaybackController &operator=(const PlaybackController &) = delete;

    void set_state_callback(StateFn fn);

    void open(const std::string &uri);
    void play();
    void pause();
    // Pauses and gives the decoder back even if the backend could hold it (app backgrounded, decoder
    // reclaimed). Resuming reloads it and seeks back.
    void pause_and_release();
    void toggle_pause();
    void seek(int64_t pts_us);
    void seek_relative(int64_t delta_us);
    // Stops playback, unloads the backend and joins the thread (bounded by stop_deadline).
    // Returns false if the thread did not finish in time: it is then detached and keeps the
    // controller's internal state alive until it ends, so it never touches freed memory. The
    // warning logged then names the source/backend call the thread is blocked in.
    bool stop();

    void set_video_area(const Rect &pixels);

    // Plays another audio track (a stream index from MediaInfo) from the current position. The decoder
    // is reloaded, so there is a short gap; while paused it is reloaded on play. A track that cannot be
    // played is logged and the current one kept. Ignored before Ready.
    void set_audio_track(int track);

    // Subtitle track to show (a stream index from MediaInfo, -1 for none).
    void set_subtitle_track(int track);
    int subtitle_track() const;
    // Cues of the selected track at the current position (empty when none or paused before any).
    std::vector<std::shared_ptr<const SubtitleCue>> subtitles_now() const;
    const SubtitleStore &subtitle_store() const;
    PlaybackSnapshot snapshot() const;
    PlayState state() const;
    const char *backend_name() const;  // may change at open (AutoBackend)

    struct Core;  // everything the playback thread uses; shared with that thread

 private:
    std::shared_ptr<Core> core_;
};

}  // namespace wnk::media
