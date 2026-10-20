// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/controller.hpp"

#include "wnk/log.hpp"

#include <algorithm>

namespace wnk::media {
namespace {
constexpr const char *kCat = "media";
// With no end-of-stream event from the backend, playback is over once the estimated position has
// passed the last PTS fed by this much (the decoder's own delay).
constexpr int64_t kEndSlackUs = 300000;
}  // namespace

const char *play_state_name(PlayState s) {
    switch (s) {
        case PlayState::Idle: return "Idle";
        case PlayState::Opening: return "Opening";
        case PlayState::Ready: return "Ready";
        case PlayState::Playing: return "Playing";
        case PlayState::Paused: return "Paused";
        case PlayState::Seeking: return "Seeking";
        case PlayState::Ended: return "Ended";
        case PlayState::Error: return "Error";
        case PlayState::Stopped: return "Stopped";
    }
    return "?";
}

struct PlaybackController::Core {
    using StateFn = PlaybackController::StateFn;

    Core(std::unique_ptr<MediaSource> source, std::unique_ptr<MediaBackend> backend, async::MainDispatcher &main,
         PlaybackOptions options);
    ~Core();
    // Starts the playback thread, which keeps `self` (this core) alive until it ends.
    void start(const std::shared_ptr<Core> &self);

    void set_state_callback(StateFn fn) { on_state_ = std::move(fn); }

    void open(const std::string &uri);
    void play();
    void pause();
    // Pauses and gives the decoder back even if the backend could hold it (app backgrounded, decoder
    // reclaimed). Resuming reloads it and seeks back.
    void pause_and_release();
    void toggle_pause();
    void seek(int64_t pts_us);
    void seek_relative(int64_t delta_us);
    bool stop();

    void set_video_area(const Rect &pixels);
    void set_audio_track(int track);

    // Subtitle track to show (a stream index from MediaInfo, -1 for none).
    void set_subtitle_track(int track) { subtitle_track_.store(track); }
    int subtitle_track() const { return subtitle_track_.load(); }
    // Cues of the selected track at the current position (empty when none or paused before any).
    std::vector<std::shared_ptr<const SubtitleCue>> subtitles_now() const;
    const SubtitleStore &subtitle_store() const { return subtitles_; }
    PlaybackSnapshot snapshot() const;
    PlayState state() const;
    const char *backend_name() const { return backend_->name(); }  // may change at open (AutoBackend)

    enum class Cmd : uint8_t { Open, Play, Pause, Seek, Stop, Audio };
    struct Command {
        Cmd cmd;
        std::string uri;
        int64_t pts_us = 0;  // Audio: the track
        bool release = false;  // Pause: unload the decoder even if the backend can pause
    };
    void post(Command c);
    void run();
    bool apply(const Command &c);  // false: leave the loop
    bool hold_seek(const Command &c) const;  // mutex_ held
    bool has_commands() const;
    void do_open(const std::string &uri);
    void do_seek(int64_t pts_us);
    void do_pause(bool release);
    void do_resume();
    void do_audio(int track);
    bool load_backend();
    void feed_one();
    bool prime_done() const;  // playback thread
    void clear_lanes();
    // Reader thread: reads the source into the lanes while they have room and it is resumed.
    void reader_loop();
    bool lanes_room() const;  // lanes_mutex_ held
    // Playback thread, around every use of the source: park stops the reader (interrupting a read
    // in progress) and waits until it is idle; resume lets it read on from where the source is.
    void park_reader();
    void resume_reader();
    void quit_reader();
    // Buffering (playback thread): stop or restart the backend and the clock.
    bool update_buffering();  // true: hold feeding for now
    void leave_buffering(bool restart_clock);
    void set_state(PlayState s, const std::string &error = {});
    void fail(const std::string &error);
    int64_t estimated_position() const;  // lock held; never before the clock's base (for display)
    int64_t raw_position() const;        // lock held; before the base while a splice backlog plays
    void on_backend_event(BackendEvent e, const std::string &detail);

    std::unique_ptr<MediaSource> source_;
    std::unique_ptr<MediaBackend> backend_;
    async::MainDispatcher &main_;
    PlaybackOptions options_;
    StateFn on_state_;
    std::shared_ptr<int> alive_;  // posted callbacks check it (guarded by mutex_)

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
    bool source_ready_ = false;  // opened and selected: the reader may read it
    bool buffering_ = false;     // the backend is paused for buffering
    int64_t last_primary_pts_ = kNoTime;  // last PTS fed on the stream that drives the clock (guarded by mutex_)
    int64_t last_any_pts_ = kNoTime;      // last PTS fed on any stream (guarded by mutex_)
    int64_t splice_delay_us_ = 0;         // the next clock start waits this long (guarded by mutex_)
    // Lanes, filled by the reader thread and emptied by the playback thread (guarded by lanes_mutex_;
    // only the playback thread pops or clears them, so the front element stays put while it is fed).
    mutable std::mutex lanes_mutex_;
    std::condition_variable reader_cv_;
    std::deque<Packet> lanes_[2];  // [0] video, [1] audio
    size_t lane_bytes_ = 0;
    bool reader_on_ = false;    // the playback thread lets it read
    bool reader_busy_ = false;  // in source_->read()
    bool reader_quit_ = false;
    std::string reader_error_;  // a read failed; the playback thread fails with it
    std::atomic<bool> source_done_{false};
    std::thread reader_;
    bool priming_ = false;
    bool eos_sent_ = false;  // backend told everything is fed
    std::atomic<int64_t> last_seek_request_ms_{0};  // steady clock, from seek() on the main thread
    int64_t last_seek_applied_ms_ = 0;               // playback thread
    std::atomic<int64_t> last_seek_target_us_{kNoTime};  // main thread: the latest seek() target
    SubtitleStore subtitles_;
    std::atomic<int> subtitle_track_{-1};
    std::atomic<bool> eos_seen_{false};
    std::thread thread_;
    bool joined_ = false;
};

PlaybackController::Core::Core(std::unique_ptr<MediaSource> source, std::unique_ptr<MediaBackend> backend,
                               async::MainDispatcher &main, PlaybackOptions options)
    : source_(std::move(source)), backend_(std::move(backend)), main_(main),
      options_(options), alive_(std::make_shared<int>(0)) {
    backend_->set_event_callback([this](BackendEvent e, const std::string &d) { on_backend_event(e, d); });
}

void PlaybackController::Core::start(const std::shared_ptr<Core> &self) {
    thread_ = std::thread([self] { self->run(); });
    reader_ = std::thread([self] { self->reader_loop(); });
}

PlaybackController::Core::~Core() {
    // The last reference may be the playback or reader thread's own (detached, and now finished).
    for (std::thread *t : {&thread_, &reader_}) {
        if (!t->joinable()) continue;
        if (t->get_id() == std::this_thread::get_id()) {
            t->detach();
        } else {
            t->join();
        }
    }
}

// ---- the public handle ------------------------------------------------------------------------------

PlaybackController::PlaybackController(std::unique_ptr<MediaSource> source, std::unique_ptr<MediaBackend> backend,
                                       async::MainDispatcher &main, PlaybackOptions options)
    : core_(std::make_shared<Core>(std::move(source), std::move(backend), main, options)) {
    core_->start(core_);
}

PlaybackController::~PlaybackController() {
    core_->stop();
    std::lock_guard<std::mutex> lock(core_->mutex_);
    core_->alive_.reset();  // nothing posted from now on reaches the state callback
}

void PlaybackController::set_state_callback(StateFn fn) { core_->on_state_ = std::move(fn); }
void PlaybackController::open(const std::string &uri) { core_->open(uri); }
void PlaybackController::play() { core_->play(); }
void PlaybackController::pause() { core_->pause(); }
void PlaybackController::pause_and_release() { core_->pause_and_release(); }
void PlaybackController::toggle_pause() { core_->toggle_pause(); }
void PlaybackController::seek(int64_t pts_us) { core_->seek(pts_us); }
void PlaybackController::seek_relative(int64_t delta_us) { core_->seek_relative(delta_us); }
bool PlaybackController::stop() { return core_->stop(); }
void PlaybackController::set_video_area(const Rect &pixels) { core_->set_video_area(pixels); }
void PlaybackController::set_audio_track(int track) { core_->set_audio_track(track); }
void PlaybackController::set_subtitle_track(int track) { core_->subtitle_track_.store(track); }
int PlaybackController::subtitle_track() const { return core_->subtitle_track_.load(); }
std::vector<std::shared_ptr<const SubtitleCue>> PlaybackController::subtitles_now() const { return core_->subtitles_now(); }
const SubtitleStore &PlaybackController::subtitle_store() const { return core_->subtitles_; }
PlaybackSnapshot PlaybackController::snapshot() const { return core_->snapshot(); }
PlayState PlaybackController::state() const { return core_->state(); }
const char *PlaybackController::backend_name() const { return core_->backend_->name(); }



// ---- main thread ------------------------------------------------------------------------------------

void PlaybackController::Core::post(Command c) {
    // A blocked open/read would delay these indefinitely.
    if (c.cmd == Cmd::Stop || c.cmd == Cmd::Seek || c.cmd == Cmd::Open || c.cmd == Cmd::Audio) source_->interrupt();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (c.cmd == Cmd::Seek) {
            // Coalesce: only the latest seek target matters (held arrow keys send many).
            commands_.erase(std::remove_if(commands_.begin(), commands_.end(),
                                           [](const Command &q) { return q.cmd == Cmd::Seek; }),
                            commands_.end());
        }
        commands_.push_back(std::move(c));
    }
    cv_.notify_all();
}

void PlaybackController::Core::open(const std::string &uri) { post({Cmd::Open, uri, 0}); }
void PlaybackController::Core::play() { post({Cmd::Play, {}, 0}); }
void PlaybackController::Core::pause() { post({Cmd::Pause, {}, 0}); }
void PlaybackController::Core::pause_and_release() { post({Cmd::Pause, {}, 0, true}); }
void PlaybackController::Core::set_audio_track(int track) { post({Cmd::Audio, {}, track}); }

void PlaybackController::Core::toggle_pause() {
    const PlayState s = state();
    if (s == PlayState::Paused || s == PlayState::Ready || s == PlayState::Ended) {
        play();
    } else {
        pause();
    }
}

void PlaybackController::Core::seek(int64_t pts_us) {
    last_seek_request_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
    last_seek_target_us_ = std::max<int64_t>(0, pts_us);
    post({Cmd::Seek, {}, std::max<int64_t>(0, pts_us)});
}

void PlaybackController::Core::seek_relative(int64_t delta_us) {
    const PlaybackSnapshot s = snapshot();
    int64_t base = s.position_us != kNoTime ? s.position_us : 0;
    // Presses in quick succession add up: while a seek is still on its way the position shown is
    // not its target yet, so the next step starts from the target.
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    if (now_ms - last_seek_request_ms_.load() < 2000 && last_seek_target_us_.load() != kNoTime) {
        base = last_seek_target_us_.load();
    }
    int64_t target = base + delta_us;
    if (s.duration_us != kNoTime) target = std::min(target, std::max<int64_t>(0, s.duration_us - 1000000));
    seek(target);
}

bool PlaybackController::Core::stop() {
    if (joined_) return true;
    post({Cmd::Stop, {}, 0});
    // Wait for the thread to finish, bounded: a hardware call that never returns must not hang shutdown.
    const auto deadline = std::chrono::steady_clock::now() + options_.stop_deadline;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_until(lock, deadline, [this] { return snap_.state == PlayState::Stopped; });
    }
    joined_ = true;
    if (state() == PlayState::Stopped) {
        thread_.join();
        return true;
    }
    WNK_LOGW(kCat, "playback thread did not stop within %lld ms; detached",
             static_cast<long long>(options_.stop_deadline.count()));
    thread_.detach();
    return false;
}

void PlaybackController::Core::set_video_area(const Rect &pixels) { backend_->set_area(pixels); }

PlaybackSnapshot PlaybackController::Core::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    PlaybackSnapshot s = snap_;
    const int64_t backend_pos = backend_->caps().position ? backend_->position_us() : kNoTime;
    if (backend_pos != kNoTime) {
        s.position_us = backend_pos;
        s.position_estimated = false;
    } else {
        s.position_us = estimated_position();
        s.position_estimated = true;
    }
    return s;
}

std::vector<std::shared_ptr<const SubtitleCue>> PlaybackController::Core::subtitles_now() const {
    const int track = subtitle_track_.load();
    if (track < 0) return {};
    int64_t pos;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pos = estimated_position();
    }
    return subtitles_.active(track, pos);
}

PlayState PlaybackController::Core::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snap_.state;
}

int64_t PlaybackController::Core::raw_position() const {
    if (paused_at_us_ != kNoTime) return paused_at_us_;
    if (!clock_running_ || clock_base_us_ == kNoTime) return clock_base_us_;
    const auto elapsed = std::chrono::steady_clock::now() - clock_start_;
    int64_t pos = clock_base_us_ + std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    if (snap_.duration_us != kNoTime) pos = std::min(pos, snap_.duration_us);
    return pos;
}

int64_t PlaybackController::Core::estimated_position() const {
    const int64_t pos = raw_position();
    return pos != kNoTime && clock_base_us_ != kNoTime && clock_running_ ? std::max(pos, clock_base_us_) : pos;
}

// ---- playback thread --------------------------------------------------------------------------------

void PlaybackController::Core::set_state(PlayState s, const std::string &error) {
    PlaybackSnapshot copy;
    PlayState from;
    std::weak_ptr<int> alive;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        from = snap_.state;
        if (from == s && error.empty()) return;
        snap_.state = s;
        if (!error.empty()) snap_.error = error;
        copy = snap_;
        copy.position_us = estimated_position();
        alive = alive_;
    }
    cv_.notify_all();  // stop() waits for Stopped
    WNK_LOGI(kCat, "%s -> %s%s%s", play_state_name(from), play_state_name(s), error.empty() ? "" : ": ", error.c_str());
    main_.post([this, alive, from, s, copy] {
        if (!alive.expired() && on_state_) on_state_(from, s, copy);
    });
}

void PlaybackController::Core::fail(const std::string &error) {
    feeding_ = false;
    park_reader();
    source_ready_ = false;
    leave_buffering(false);
    clear_lanes();
    if (loaded_) {
        backend_->unload();
        loaded_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clock_running_ = false;
    }
    set_state(PlayState::Error, error);
}

void PlaybackController::Core::on_backend_event(BackendEvent e, const std::string &detail) {
    WNK_LOGI(kCat, "backend event %s%s%s", backend_event_name(e), detail.empty() ? "" : ": ", detail.c_str());
    if (e == BackendEvent::EndOfStream) {
        eos_seen_ = true;
        cv_.notify_all();
    } else if (e == BackendEvent::ResourceLost) {
        // Treat like a pause: the decoder must be loaded again before more packets go in.
        post({Cmd::Pause, {}, 0, true});
    }
}

void PlaybackController::Core::run() {
    for (;;) {
        std::optional<Command> cmd;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!feeding_) cv_.wait(lock, [this] { return !commands_.empty(); });
            if (!commands_.empty() && !hold_seek(commands_.front())) {
                cmd = std::move(commands_.front());
                commands_.pop_front();
            }
        }
        if (cmd) {
            if (!apply(*cmd)) return;
            resume_reader();  // a command may have interrupted it; reads on from where the source is
            continue;
        }
        if (has_commands()) {
            // A seek held until the last one's content is on screen (hold_seek): keep the decoder fed
            // with what is queued meanwhile, without spinning on a source that has been interrupted.
            feed_one();
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(20));
            continue;
        }
        feed_one();
    }
}

// Splicing backends: a seek waits until the previous seek's content has reached the screen (at most
// 2.5 s), as PlxNative does: a second splice stacked on one still pending left Starfish dropping
// every frame for the rest of playback. Seeks requested meanwhile coalesce into the newest. Called
// with mutex_ held.
bool PlaybackController::Core::hold_seek(const Command &c) const {
    if (c.cmd != Cmd::Seek || commands_.size() > 1 || !loaded_ || !backend_->caps().seek_splices) return false;  // Stop & co. go through
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    if (now_ms - last_seek_applied_ms_ > 2500) return false;
    return !backend_->splice_settled();
}

bool PlaybackController::Core::has_commands() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !commands_.empty();
}

bool PlaybackController::Core::apply(const Command &c) {
    switch (c.cmd) {
        case Cmd::Open: do_open(c.uri); break;
        case Cmd::Play: {
            const PlayState s = state();
            if (s == PlayState::Paused) {
                do_resume();
            } else if (s == PlayState::Ready) {
                feeding_ = true;
                set_state(PlayState::Playing);
            } else if (s == PlayState::Ended) {
                do_seek(0);
            }
            break;
        }
        case Cmd::Pause:
            if (state() == PlayState::Playing || state() == PlayState::Seeking) {
                do_pause(c.release);
            } else if (c.release && state() == PlayState::Paused && loaded_) {
                // Already held on a frame: give the decoder back; resume reloads and seeks back.
                clear_lanes();
                backend_->unload();
                loaded_ = false;
            }
            break;
        case Cmd::Seek:
            last_seek_applied_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count();
            do_seek(c.pts_us);
            break;
        case Cmd::Stop:
            feeding_ = false;
            quit_reader();
            source_ready_ = false;
            leave_buffering(false);
            clear_lanes();
            if (loaded_) {
                backend_->unload();
                loaded_ = false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                clock_running_ = false;
            }
            set_state(PlayState::Stopped);
            return false;
        case Cmd::Audio: do_audio(static_cast<int>(c.pts_us)); break;
    }
    return true;
}

void PlaybackController::Core::do_open(const std::string &uri) {
    park_reader();
    source_ready_ = false;
    leave_buffering(false);
    if (loaded_) {
        backend_->unload();
        loaded_ = false;
    }
    feeding_ = false;
    clear_lanes();
    priming_ = true;
    source_done_ = false;
    eos_seen_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snap_ = PlaybackSnapshot{};
        last_primary_pts_ = kNoTime;
        last_any_pts_ = kNoTime;
        splice_delay_us_ = 0;
        clock_running_ = false;
        clock_base_us_ = kNoTime;
        paused_at_us_ = kNoTime;
    }
    subtitles_.clear();
    set_state(PlayState::Opening);
    source_->reset_interrupt();
    std::string err;
    if (!source_->open(uri, &err)) {
        fail("open failed: " + err);
        return;
    }
    const MediaInfo &info = source_->info();
    const TrackInfo *v = options_.video_track == -2 ? info.best(TrackType::Video) : info.track(options_.video_track);
    const TrackInfo *a = options_.audio_track == -2 ? info.best(TrackType::Audio) : info.track(options_.audio_track);
    StreamFormat format;
    backend_->prepare(info, v, a);
    if (!source_->select(v != nullptr ? v->index : -1, a != nullptr ? a->index : -1, backend_->caps(), &format, &err)) {
        fail("tracks not playable: " + err);
        return;
    }
    source_ready_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.info = info;
        snap_.format = format;
        snap_.duration_us = info.duration_us;
        snap_.video_track = v != nullptr ? v->index : -1;
        snap_.audio_track = a != nullptr ? a->index : -1;
        clock_base_us_ = 0;
    }
    WNK_LOGI(kCat, "opened %s: %s, %.1f s, video %s %dx%d, audio %s %d Hz x%d", uri.c_str(), info.container.c_str(),
             info.duration_us == kNoTime ? -1.0 : info.duration_us / 1e6,
             format.video ? codec_name(format.video->codec) : "-", format.video ? format.video->width : 0,
             format.video ? format.video->height : 0, format.audio ? codec_name(format.audio->codec) : "-",
             format.audio ? format.audio->sample_rate : 0, format.audio ? format.audio->channels : 0);
    if (!load_backend()) return;
    set_state(PlayState::Ready);
    if (options_.autoplay) {
        feeding_ = true;
        set_state(PlayState::Playing);
    }
}

bool PlaybackController::Core::load_backend() {
    StreamFormat format;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        format = snap_.format;
    }
    std::string err;
    if (!backend_->load(format, &err)) {
        fail(std::string(backend_->name()) + " load failed: " + err);
        return false;
    }
    loaded_ = true;
    return true;
}

void PlaybackController::Core::do_seek(int64_t pts_us) {
    const PlayState before = state();
    if (before == PlayState::Idle || before == PlayState::Opening || before == PlayState::Error ||
        before == PlayState::Stopped) {
        return;
    }
    const bool resume = before != PlayState::Paused;
    const BackendCaps caps = backend_->caps();
    int64_t splice_delay = 0;
    if (loaded_ && caps.seek_splices) {
        // What the decoder still holds plays first: from the clock's position to the last PTS fed.
        std::lock_guard<std::mutex> lock(mutex_);
        const int64_t pos = raw_position();
        if (resume && pos != kNoTime && last_any_pts_ != kNoTime) splice_delay = std::max<int64_t>(0, last_any_pts_ - pos);
        splice_delay += caps.splice_gap_us;
    }
    park_reader();
    if (buffering_) {
        if (loaded_) backend_->pause(false);  // the seek decides whether it plays or holds
        leave_buffering(false);
    }
    set_state(PlayState::Seeking);
    source_->reset_interrupt();
    clear_lanes();
    priming_ = true;
    source_done_ = false;
    eos_seen_ = false;
    std::string err;
    if (!source_->seek(pts_us, &err)) {
        fail("seek failed: " + err);
        return;
    }
    if (loaded_) {
        if (!backend_->flush(&err)) {
            fail(std::string(backend_->name()) + " flush failed: " + err);
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clock_running_ = false;
        clock_base_us_ = pts_us;
        last_primary_pts_ = kNoTime;
        last_any_pts_ = kNoTime;
        splice_delay_us_ = splice_delay;
        paused_at_us_ = resume ? kNoTime : pts_us;
    }
    if (resume) {
        if (!loaded_ && !load_backend()) return;
        feeding_ = true;
        set_state(PlayState::Playing);
    } else {
        // Stay paused at the new position; the decoder is reloaded on resume.
        feeding_ = false;
        if (loaded_ && !caps.pause) {
            backend_->unload();
            loaded_ = false;
        }
        set_state(PlayState::Paused);
    }
}

void PlaybackController::Core::do_pause(bool release) {
    const BackendCaps caps = backend_->caps();
    const bool hold = caps.pause && !release;
    leave_buffering(false);  // already held where buffering stopped it; paused_at_us_ stays
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int64_t at = raw_position();
        // A draining pause stops the picture where feeding stopped, not where the clock is now;
        // resuming from the clock would leave the decoder that much short of data.
        if (hold && caps.pause_drains && loaded_ && last_primary_pts_ != kNoTime) {
            at = std::max(at, last_primary_pts_);
        }
        paused_at_us_ = at;
        clock_running_ = false;
    }
    if (hold && loaded_ && backend_->pause(true)) {
        feeding_ = false;
    } else {
        // No real pause: stop feeding and release the decoder; resume reloads and seeks back.
        feeding_ = false;
        clear_lanes();
        if (loaded_) {
            backend_->unload();
            loaded_ = false;
        }
    }
    set_state(PlayState::Paused);
}

void PlaybackController::Core::do_resume() {
    int64_t at;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        at = paused_at_us_ != kNoTime ? paused_at_us_ : 0;
    }
    if (loaded_ && backend_->caps().pause && backend_->pause(false)) {
        std::lock_guard<std::mutex> lock(mutex_);
        paused_at_us_ = kNoTime;
        clock_base_us_ = at;
        clock_start_ = std::chrono::steady_clock::now() + std::chrono::microseconds(splice_delay_us_);
        splice_delay_us_ = 0;
        clock_running_ = true;
        feeding_ = true;
    } else {
        if (!loaded_ && !load_backend()) return;
        std::string err;
        park_reader();
        source_->reset_interrupt();
        source_done_ = false;
        eos_seen_ = false;
        clear_lanes();
        priming_ = true;
        if (!source_->seek(at, &err)) {
            fail("seek on resume failed: " + err);
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        paused_at_us_ = kNoTime;
        clock_base_us_ = at;
        clock_running_ = false;
        feeding_ = true;
    }
    set_state(PlayState::Playing);
}

// Another audio track: the decoder is reloaded (the codec may differ, and AutoBackend may pick the other
// backend for it) and the source restarts at the position, as a resume after a release does. Paused
// stays paused with the decoder unloaded; play reloads it.
void PlaybackController::Core::do_audio(int track) {
    const PlayState before = state();
    if (before != PlayState::Ready && before != PlayState::Playing && before != PlayState::Paused &&
        before != PlayState::Seeking) {
        WNK_LOGI(kCat, "audio track %d ignored in %s", track, play_state_name(before));
        return;
    }
    const MediaInfo &info = source_->info();
    const TrackInfo *a = info.track(track);
    int video = -1, old = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        video = snap_.video_track;
        old = snap_.audio_track;
    }
    if (a == nullptr || a->type != TrackType::Audio || track == old) return;
    const TrackInfo *v = info.track(video);
    int64_t at;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        at = paused_at_us_ != kNoTime ? paused_at_us_ : clock_running_ ? raw_position() : clock_base_us_;
        if (at == kNoTime || at < 0) at = 0;
        clock_running_ = false;
    }
    // Selecting can fetch (DASH/HLS: the new rendition's init segment), so say so before it starts.
    if (before != PlayState::Paused) set_state(PlayState::Seeking);
    feeding_ = false;
    park_reader();
    leave_buffering(false);
    clear_lanes();
    if (loaded_) {
        backend_->unload();
        loaded_ = false;
    }
    source_->reset_interrupt();
    StreamFormat format;
    std::string err;
    backend_->prepare(info, v, a);
    if (!source_->select(video, track, backend_->caps(), &format, &err)) {
        WNK_LOGW(kCat, "audio track %d not playable (%s); keeping track %d", track, err.c_str(), old);
        a = info.track(old);
        backend_->prepare(info, v, a);
        if (!source_->select(video, old, backend_->caps(), &format, &err)) {
            fail("tracks not playable: " + err);
            return;
        }
        track = old;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.format = format;
        snap_.audio_track = track;
    }
    WNK_LOGI(kCat, "audio track %d (%s) at %.2f s", track, format.audio ? codec_name(format.audio->codec) : "-", at / 1e6);
    if (before == PlayState::Paused) {
        std::lock_guard<std::mutex> lock(mutex_);
        paused_at_us_ = at;
        return;  // do_resume loads the decoder and seeks to `at`
    }
    source_done_ = false;
    eos_seen_ = false;
    priming_ = true;
    if (!source_->seek(at, &err)) {
        fail("seek after audio change failed: " + err);
        return;
    }
    if (!load_backend()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clock_base_us_ = at;
        paused_at_us_ = kNoTime;
        last_primary_pts_ = kNoTime;
        last_any_pts_ = kNoTime;
        splice_delay_us_ = 0;
    }
    feeding_ = before != PlayState::Ready;
    set_state(before == PlayState::Ready ? PlayState::Ready : PlayState::Playing);
}

void PlaybackController::Core::clear_lanes() {
    {
        std::lock_guard<std::mutex> lock(lanes_mutex_);
        lanes_[0].clear();
        lanes_[1].clear();
        lane_bytes_ = 0;
    }
    reader_cv_.notify_all();  // room again
    eos_sent_ = false;  // every caller restarts or stops feeding
}

namespace {
int64_t lane_span(const std::deque<Packet> &lane) {
    if (lane.empty()) return 0;
    const Packet &a = lane.front(), &b = lane.back();
    if (a.pts_us == kNoTime || b.pts_us == kNoTime) return 0;
    return std::max<int64_t>(0, b.pts_us - a.pts_us) + b.duration_us;
}
}  // namespace

bool PlaybackController::Core::prime_done() const {
    if (source_done_) return true;
    bool has_video, has_audio;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        has_video = snap_.format.video.has_value();
        has_audio = snap_.format.audio.has_value();
    }
    std::lock_guard<std::mutex> lock(lanes_mutex_);
    const int64_t v = lane_span(lanes_[0]), a = lane_span(lanes_[1]);
    if (has_video && v >= options_.prime_video_max_us) return true;  // no audio coming soon
    if (lane_bytes_ >= options_.max_queue_bytes) return true;
    return (!has_video || v >= options_.prime_video_us) && (!has_audio || a >= options_.prime_audio_us);
}

// ---- reader thread ----------------------------------------------------------------------------------

bool PlaybackController::Core::lanes_room() const {
    return lane_bytes_ < options_.max_queue_bytes &&
           (lanes_[0].empty() || lanes_[1].empty() ||
            (lane_span(lanes_[0]) < options_.max_queue_us && lane_span(lanes_[1]) < options_.max_queue_us));
}

void PlaybackController::Core::reader_loop() {
    std::unique_lock<std::mutex> lock(lanes_mutex_);
    for (;;) {
        reader_cv_.wait(lock, [this] {
            return reader_quit_ || (reader_on_ && !source_done_ && reader_error_.empty() && lanes_room());
        });
        if (reader_quit_) return;
        reader_busy_ = true;
        lock.unlock();
        Packet p;
        std::string err;
        const MediaSource::Read r = source_->read(&p, &err);
        lock.lock();
        reader_busy_ = false;
        // Parked meanwhile: whatever came back belongs to a position about to be left.
        if (reader_on_) {
            switch (r) {
                case MediaSource::Read::Packet:
                    if (p.type == TrackType::Subtitle) {
                        // Never fed to the backend: the app draws them from the store.
                        if (p.subtitle) subtitles_.add(p.subtitle);
                    } else if (p.type == TrackType::Video || p.type == TrackType::Audio) {
                        lane_bytes_ += p.data.size();
                        lanes_[p.type == TrackType::Video ? 0 : 1].push_back(std::move(p));
                    }
                    break;
                case MediaSource::Read::EndOfStream:
                    WNK_LOGI(kCat, "source finished");
                    source_done_ = true;
                    break;
                case MediaSource::Read::Interrupted:
                    reader_on_ = false;  // a command is waiting; the playback thread resumes it
                    break;
                case MediaSource::Read::Error: reader_error_ = err.empty() ? "unknown error" : err; break;
            }
        }
        reader_cv_.notify_all();  // park_reader() waits for it to be idle
        cv_.notify_all();         // the playback thread waits for packets
    }
}

void PlaybackController::Core::park_reader() {
    {
        std::lock_guard<std::mutex> lock(lanes_mutex_);
        reader_on_ = false;
    }
    source_->interrupt();
    std::unique_lock<std::mutex> lock(lanes_mutex_);
    reader_cv_.wait(lock, [this] { return !reader_busy_; });
}

void PlaybackController::Core::resume_reader() {
    source_->reset_interrupt();
    {
        std::lock_guard<std::mutex> lock(lanes_mutex_);
        if (reader_on_ || !source_ready_) return;
        reader_on_ = true;
    }
    reader_cv_.notify_all();
}

void PlaybackController::Core::quit_reader() {
    {
        std::lock_guard<std::mutex> lock(lanes_mutex_);
        reader_on_ = false;
        reader_quit_ = true;
    }
    source_->interrupt();
    reader_cv_.notify_all();
    if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id()) reader_.join();
}

// ---- buffering (playback thread) ---------------------------------------------------------------------

// Enters or leaves buffering; true while it holds feeding.
bool PlaybackController::Core::update_buffering() {
    const BackendCaps caps = backend_->caps();
    if (!loaded_ || !caps.pause || priming_) return buffering_;
    const bool done = source_done_;
    int64_t span[2], lead[2];
    bool active[2], empty[2];
    {
        std::lock_guard<std::mutex> lock(lanes_mutex_);
        for (int i = 0; i < 2; ++i) {
            empty[i] = lanes_[i].empty();
            span[i] = lane_span(lanes_[i]);
        }
    }
    int64_t pos;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active[0] = snap_.format.video.has_value();
        active[1] = snap_.format.audio.has_value();
        pos = raw_position();
        const int64_t last[2] = {snap_.last_video_pts_us, snap_.last_audio_pts_us};
        for (int i = 0; i < 2; ++i) lead[i] = last[i] == kNoTime || pos == kNoTime ? 0 : last[i] - pos;
        if (!buffering_ && !clock_running_) return false;  // nothing presented yet, or paused
    }
    if (!buffering_) {
        bool starving = false;
        for (int i = 0; i < 2; ++i) starving = starving || (active[i] && empty[i] && lead[i] < options_.rebuffer_low_us);
        if (!starving || done) return false;
        if (!backend_->pause(true)) return false;  // cannot hold: carry on as without buffering
        buffering_ = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            int64_t at = raw_position();
            if (caps.pause_drains && last_primary_pts_ != kNoTime) at = std::max(at, last_primary_pts_);
            paused_at_us_ = at;
            clock_running_ = false;
            snap_.buffering = true;
            ++snap_.rebuffers;
        }
        WNK_LOGI(kCat, "buffering at %.2f s: decoder ahead video %.2f s, audio %.2f s; queued %.2f s, %.2f s",
                 pos / 1e6, lead[0] / 1e6, lead[1] / 1e6, span[0] / 1e6, span[1] / 1e6);
        return true;
    }
    bool enough = true;
    for (int i = 0; i < 2; ++i) enough = enough && (!active[i] || span[i] >= options_.rebuffer_goal_us);
    size_t bytes;
    {
        std::lock_guard<std::mutex> lock(lanes_mutex_);
        bytes = lane_bytes_;
    }
    if (!enough && !done && bytes < options_.max_queue_bytes) return true;
    WNK_LOGI(kCat, "buffered: queued %.2f s video, %.2f s audio%s", span[0] / 1e6, span[1] / 1e6, done ? " (source ended)" : "");
    backend_->pause(false);
    leave_buffering(/*restart_clock=*/true);
    return false;
}

// restart_clock: the backend was unpaused here, playback carries on from where it stopped; else the
// caller takes over the paused state (a pause, seek, reload).
void PlaybackController::Core::leave_buffering(bool restart_clock) {
    if (!buffering_) return;
    buffering_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    snap_.buffering = false;
    if (restart_clock && paused_at_us_ != kNoTime) {
        clock_base_us_ = paused_at_us_;
        clock_start_ = std::chrono::steady_clock::now();
        paused_at_us_ = kNoTime;
        clock_running_ = true;
    }
}

// ---- playback thread: feeding ------------------------------------------------------------------------

void PlaybackController::Core::feed_one() {
    bool lanes_empty;
    std::string read_error;
    {
        std::lock_guard<std::mutex> lock(lanes_mutex_);
        lanes_empty = lanes_[0].empty() && lanes_[1].empty();
        read_error.swap(reader_error_);
    }
    if (!read_error.empty()) {
        fail("read failed: " + read_error);
        return;
    }
    if (source_done_ && lanes_empty) {
        // Everything is fed: wait for the backend to present it.
        if (!eos_sent_ && loaded_) {
            eos_sent_ = true;
            backend_->end_of_stream();
        }
        bool ended = eos_seen_;
        if (!ended) {
            std::lock_guard<std::mutex> lock(mutex_);
            const int64_t pos = raw_position();
            // The estimate is capped at the duration, so the target must be too.
            int64_t end = last_primary_pts_ == kNoTime ? 0 : last_primary_pts_ + kEndSlackUs;
            if (snap_.duration_us != kNoTime) end = std::min(end, snap_.duration_us);
            ended = pos != kNoTime && pos >= end;
        }
        if (ended) {
            feeding_ = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (snap_.duration_us != kNoTime) paused_at_us_ = snap_.duration_us;
                clock_running_ = false;
            }
            set_state(PlayState::Ended);
            return;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(20), [this] { return !commands_.empty() || eos_seen_; });
        return;
    }

    if (priming_) {
        if (!prime_done()) {
            // The reader is filling the lanes.
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(10), [this] { return !commands_.empty(); });
            return;
        }
        if (backend_->caps().seek_splices) {
            // Let a burst of seeks settle before anything of this target reaches the decoder.
            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count();
            const int64_t wait_ms = last_seek_request_ms_.load() + options_.seek_settle.count() - now_ms;
            if (wait_ms > 0) {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(std::min<int64_t>(wait_ms, 20)),
                             [this] { return !commands_.empty(); });
                return;
            }
        }
        priming_ = false;
    }

    if (update_buffering()) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(20), [this] { return !commands_.empty(); });
        return;
    }

    // The lane whose head is due and earliest; how long until one is, otherwise.
    int lane = -1;
    int64_t wait_us = 20000;
    Packet *heads[2] = {nullptr, nullptr};
    {
        // Only this thread pops the lanes, so a head stays valid after the lock is let go.
        std::lock_guard<std::mutex> lock(lanes_mutex_);
        for (int i = 0; i < 2; ++i) heads[i] = lanes_[i].empty() ? nullptr : &lanes_[i].front();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int64_t pos = raw_position();
        for (int i = 0; i < 2; ++i) {
            if (heads[i] == nullptr) continue;
            const int64_t pts = heads[i]->pts_us;
            const int64_t ahead = options_.max_lead_us > 0 && clock_running_ && pts != kNoTime && pos != kNoTime
                                      ? pts - pos - options_.max_lead_us
                                      : 0;
            if (ahead > 0) {
                wait_us = std::min(wait_us, ahead);
                continue;
            }
            if (lane < 0 || (pts != kNoTime && heads[lane]->pts_us != kNoTime && pts < heads[lane]->pts_us)) lane = i;
        }
    }
    if (lane < 0) {
        // Nothing due: wait for the clock or for the reader.
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::microseconds(wait_us), [this] { return !commands_.empty(); });
        return;
    }

    Packet &pkt = *heads[lane];
    switch (backend_->feed(pkt)) {
        case MediaBackend::Feed::Accepted: {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (pkt.type == TrackType::Video) {
                    ++snap_.video_packets;
                    if (pkt.pts_us != kNoTime) snap_.last_video_pts_us = std::max(snap_.last_video_pts_us, pkt.pts_us);
                } else {
                    ++snap_.audio_packets;
                    snap_.last_audio_pts_us = pkt.pts_us;
                }
                if (pkt.pts_us == kNoTime) ++snap_.untimed_packets;
                // Start the position clock at the first packet after a load or seek.
                const bool primary = pkt.type == TrackType::Video || !snap_.format.video;
                if (primary && pkt.pts_us != kNoTime) {
                    last_primary_pts_ = std::max(last_primary_pts_ == kNoTime ? pkt.pts_us : last_primary_pts_, pkt.pts_us);
                }
                if (pkt.pts_us != kNoTime) {
                    last_any_pts_ = last_any_pts_ == kNoTime ? pkt.pts_us : std::max(last_any_pts_, pkt.pts_us);
                }
                if (!clock_running_ && primary && pkt.pts_us != kNoTime) {
                    clock_base_us_ = pkt.pts_us;
                    clock_start_ = std::chrono::steady_clock::now() + std::chrono::microseconds(splice_delay_us_);
                    splice_delay_us_ = 0;
                    clock_running_ = true;
                }
            }
            {
                std::lock_guard<std::mutex> lock(lanes_mutex_);
                lane_bytes_ -= std::min(lane_bytes_, pkt.data.size());
                lanes_[lane].pop_front();
            }
            reader_cv_.notify_all();  // room again
            break;
        }
        case MediaBackend::Feed::Full: {
            std::unique_lock<std::mutex> lock(mutex_);
            ++snap_.full_retries;
            cv_.wait_for(lock, options_.full_backoff, [this] { return !commands_.empty(); });
            break;
        }
        case MediaBackend::Feed::Error: fail(std::string(backend_->name()) + " rejected a packet"); break;
    }
}

}  // namespace wnk::media
