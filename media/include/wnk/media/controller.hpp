// SPDX-License-Identifier: GPL-3.0-or-later
//
// PlaybackController: drives a MediaSource into a MediaBackend on its own thread.
//
//   Idle ─open─► Opening ─► Ready ─play─► Playing ⇄ Paused
//                  │          │              │  │
//                  │          └─seek─► Seeking ┘  └─(source ends, buffer drained)─► Ended
//                  └──────────────(any failure)──────────────────────────────────► Error
//   any state ─stop─► Stopped
//
// The playback thread reads packets, feeds the backend and retries while the backend reports Full,
// and never runs more than max_lead_us ahead of the playback clock (decoders that accept everything
// would otherwise receive the whole file at once). Commands from the main thread are queued and applied
// between packets; a blocked read or open is interrupted for stop and seek.
//
// Pause: when the backend cannot pause (NDL v2 has no pause call) the controller stops feeding,
// unloads the decoder and, on resume, reloads and seeks back to where playback stopped. That is
// slower than a real pause but never lets the decoder run on and desynchronise.
//
// Position: from the backend if it reports one, else estimated on a clock started at the first
// packet after load/seek (offset by that packet's PTS) and frozen while paused. The estimate is
// marked as such in the snapshot.
//
// Threading: public methods are main-thread calls and never block on the playback thread except
// stop()/~PlaybackController, which join it with a deadline. State changes are posted to the
// MainDispatcher; snapshot() can be read at any time.
#pragma once

#include "wnk/async/async.hpp"
#include "wnk/media/media.hpp"

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
    std::string error;
    uint64_t video_packets = 0, audio_packets = 0;  // fed since open
    uint64_t full_retries = 0;  // feed() returned Full
    int64_t last_video_pts_us = kNoTime;
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

    void set_state_callback(StateFn fn) { on_state_ = std::move(fn); }

    void open(const std::string &uri);
    void play();
    void pause();
    void toggle_pause();
    void seek(int64_t pts_us);
    void seek_relative(int64_t delta_us);
    // Stops playback, unloads the backend and joins the thread (bounded by stop_deadline).
    // Returns false if the thread did not finish in time (it is then detached).
    bool stop();

    void set_video_area(const Rect &pixels);
    PlaybackSnapshot snapshot() const;
    PlayState state() const;
    const char *backend_name() const { return backend_name_; }

 private:
    enum class Cmd : uint8_t { Open, Play, Pause, Seek, Stop };
    struct Command {
        Cmd cmd;
        std::string uri;
        int64_t pts_us = 0;
    };
    void post(Command c);
    void run();
    bool apply(const Command &c);  // false: leave the loop
    void do_open(const std::string &uri);
    void do_seek(int64_t pts_us);
    void do_pause();
    void do_resume();
    bool load_backend();
    void feed_one();
    void set_state(PlayState s, const std::string &error = {});
    void fail(const std::string &error);
    int64_t estimated_position() const;  // lock held
    void on_backend_event(BackendEvent e, const std::string &detail);

    std::unique_ptr<MediaSource> source_;
    std::unique_ptr<MediaBackend> backend_;
    const char *backend_name_;
    async::MainDispatcher &main_;
    PlaybackOptions options_;
    StateFn on_state_;
    std::shared_ptr<int> alive_;  // posted callbacks check it

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Command> commands_;
    PlaybackSnapshot snap_;  // guarded by mutex_
    // Position clock (guarded by mutex_).
    bool clock_running_ = false;
    std::chrono::steady_clock::time_point clock_start_;
    int64_t clock_base_us_ = kNoTime;
    int64_t paused_at_us_ = kNoTime;

    // Playback thread only.
    bool loaded_ = false;
    bool feeding_ = false;
    bool source_done_ = false;
    int64_t last_primary_pts_ = kNoTime;  // last PTS fed on the stream that drives the clock (guarded by mutex_)
    std::optional<Packet> pending_;
    std::atomic<bool> eos_seen_{false};
    std::thread thread_;
    bool joined_ = false;
};

}  // namespace wnk::media
