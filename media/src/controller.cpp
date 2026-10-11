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

PlaybackController::PlaybackController(std::unique_ptr<MediaSource> source, std::unique_ptr<MediaBackend> backend,
                                       async::MainDispatcher &main, PlaybackOptions options)
    : source_(std::move(source)), backend_(std::move(backend)), backend_name_(backend_->name()), main_(main),
      options_(options), alive_(std::make_shared<int>(0)) {
    backend_->set_event_callback([this](BackendEvent e, const std::string &d) { on_backend_event(e, d); });
    thread_ = std::thread([this] { run(); });
}

PlaybackController::~PlaybackController() {
    stop();
    alive_.reset();
}

// ---- main thread ------------------------------------------------------------------------------------

void PlaybackController::post(Command c) {
    // A blocked open/read would delay these indefinitely.
    if (c.cmd == Cmd::Stop || c.cmd == Cmd::Seek || c.cmd == Cmd::Open) source_->interrupt();
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

void PlaybackController::open(const std::string &uri) { post({Cmd::Open, uri, 0}); }
void PlaybackController::play() { post({Cmd::Play, {}, 0}); }
void PlaybackController::pause() { post({Cmd::Pause, {}, 0}); }

void PlaybackController::toggle_pause() {
    const PlayState s = state();
    if (s == PlayState::Paused || s == PlayState::Ready || s == PlayState::Ended) {
        play();
    } else {
        pause();
    }
}

void PlaybackController::seek(int64_t pts_us) { post({Cmd::Seek, {}, std::max<int64_t>(0, pts_us)}); }

void PlaybackController::seek_relative(int64_t delta_us) {
    const PlaybackSnapshot s = snapshot();
    int64_t base = s.position_us != kNoTime ? s.position_us : 0;
    int64_t target = base + delta_us;
    if (s.duration_us != kNoTime) target = std::min(target, std::max<int64_t>(0, s.duration_us - 1000000));
    seek(target);
}

bool PlaybackController::stop() {
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

void PlaybackController::set_video_area(const Rect &pixels) { backend_->set_area(pixels); }

PlaybackSnapshot PlaybackController::snapshot() const {
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

PlayState PlaybackController::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snap_.state;
}

int64_t PlaybackController::estimated_position() const {
    if (paused_at_us_ != kNoTime) return paused_at_us_;
    if (!clock_running_ || clock_base_us_ == kNoTime) return clock_base_us_;
    const auto elapsed = std::chrono::steady_clock::now() - clock_start_;
    int64_t pos = clock_base_us_ + std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    if (snap_.duration_us != kNoTime) pos = std::min(pos, snap_.duration_us);
    return pos;
}

// ---- playback thread --------------------------------------------------------------------------------

void PlaybackController::set_state(PlayState s, const std::string &error) {
    PlaybackSnapshot copy;
    PlayState from;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        from = snap_.state;
        if (from == s && error.empty()) return;
        snap_.state = s;
        if (!error.empty()) snap_.error = error;
        copy = snap_;
        copy.position_us = estimated_position();
    }
    cv_.notify_all();  // stop() waits for Stopped
    WNK_LOGI(kCat, "%s -> %s%s%s", play_state_name(from), play_state_name(s), error.empty() ? "" : ": ", error.c_str());
    std::weak_ptr<int> alive = alive_;
    main_.post([this, alive, from, s, copy] {
        if (!alive.expired() && on_state_) on_state_(from, s, copy);
    });
}

void PlaybackController::fail(const std::string &error) {
    feeding_ = false;
    pending_.reset();
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

void PlaybackController::on_backend_event(BackendEvent e, const std::string &detail) {
    WNK_LOGI(kCat, "backend event %s%s%s", backend_event_name(e), detail.empty() ? "" : ": ", detail.c_str());
    if (e == BackendEvent::EndOfStream) {
        eos_seen_ = true;
        cv_.notify_all();
    } else if (e == BackendEvent::ResourceLost) {
        // Treat like a pause: the decoder must be loaded again before more packets go in.
        post({Cmd::Pause, {}, 0});
    }
}

void PlaybackController::run() {
    for (;;) {
        std::optional<Command> cmd;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!feeding_) cv_.wait(lock, [this] { return !commands_.empty(); });
            if (!commands_.empty()) {
                cmd = std::move(commands_.front());
                commands_.pop_front();
            }
        }
        if (cmd) {
            if (!apply(*cmd)) return;
            continue;
        }
        feed_one();
    }
}

bool PlaybackController::apply(const Command &c) {
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
            if (state() == PlayState::Playing || state() == PlayState::Seeking) do_pause();
            break;
        case Cmd::Seek: do_seek(c.pts_us); break;
        case Cmd::Stop:
            feeding_ = false;
            pending_.reset();
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
    }
    return true;
}

void PlaybackController::do_open(const std::string &uri) {
    if (loaded_) {
        backend_->unload();
        loaded_ = false;
    }
    feeding_ = false;
    pending_.reset();
    source_done_ = false;
    eos_seen_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snap_ = PlaybackSnapshot{};
        last_primary_pts_ = kNoTime;
        clock_running_ = false;
        clock_base_us_ = kNoTime;
        paused_at_us_ = kNoTime;
    }
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
    if (!source_->select(v != nullptr ? v->index : -1, a != nullptr ? a->index : -1, backend_->caps(), &format, &err)) {
        fail("tracks not playable: " + err);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snap_.info = info;
        snap_.format = format;
        snap_.duration_us = info.duration_us;
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

bool PlaybackController::load_backend() {
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

void PlaybackController::do_seek(int64_t pts_us) {
    const PlayState before = state();
    if (before == PlayState::Idle || before == PlayState::Opening || before == PlayState::Error ||
        before == PlayState::Stopped) {
        return;
    }
    const bool resume = before != PlayState::Paused;
    set_state(PlayState::Seeking);
    source_->reset_interrupt();
    pending_.reset();
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
        paused_at_us_ = resume ? kNoTime : pts_us;
    }
    if (resume) {
        if (!loaded_ && !load_backend()) return;
        feeding_ = true;
        set_state(PlayState::Playing);
    } else {
        // Stay paused at the new position; the decoder is reloaded on resume.
        feeding_ = false;
        if (loaded_ && !backend_->caps().pause) {
            backend_->unload();
            loaded_ = false;
        }
        set_state(PlayState::Paused);
    }
}

void PlaybackController::do_pause() {
    int64_t at;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        at = estimated_position();
        paused_at_us_ = at;
        clock_running_ = false;
    }
    if (backend_->caps().pause && loaded_ && backend_->pause(true)) {
        feeding_ = false;
    } else {
        // No real pause: stop feeding and release the decoder; resume reloads and seeks back.
        feeding_ = false;
        pending_.reset();
        if (loaded_) {
            backend_->unload();
            loaded_ = false;
        }
    }
    set_state(PlayState::Paused);
}

void PlaybackController::do_resume() {
    int64_t at;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        at = paused_at_us_ != kNoTime ? paused_at_us_ : 0;
    }
    if (loaded_ && backend_->caps().pause && backend_->pause(false)) {
        std::lock_guard<std::mutex> lock(mutex_);
        paused_at_us_ = kNoTime;
        clock_base_us_ = at;
        clock_start_ = std::chrono::steady_clock::now();
        clock_running_ = true;
        feeding_ = true;
    } else {
        if (!loaded_ && !load_backend()) return;
        std::string err;
        source_->reset_interrupt();
        source_done_ = false;
        eos_seen_ = false;
        pending_.reset();
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

void PlaybackController::feed_one() {
    if (!pending_) {
        if (source_done_) {
            // Everything is fed: wait for the backend to present it.
            bool ended = eos_seen_;
            if (!ended) {
                std::lock_guard<std::mutex> lock(mutex_);
                const int64_t pos = estimated_position();
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
        Packet p;
        std::string err;
        switch (source_->read(&p, &err)) {
            case MediaSource::Read::Packet: pending_ = std::move(p); break;
            case MediaSource::Read::EndOfStream:
                WNK_LOGI(kCat, "source finished");
                source_done_ = true;
                return;
            case MediaSource::Read::Interrupted: return;  // a command is waiting
            case MediaSource::Read::Error: fail("read failed: " + err); return;
        }
    }
    if (options_.max_lead_us > 0 && pending_->pts_us != kNoTime) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (clock_running_) {
            const int64_t ahead = pending_->pts_us - estimated_position() - options_.max_lead_us;
            if (ahead > 0) {
                const auto wait = std::chrono::microseconds(std::min<int64_t>(ahead, 20000));
                cv_.wait_for(lock, wait, [this] { return !commands_.empty(); });
                return;
            }
        }
    }
    switch (backend_->feed(*pending_)) {
        case MediaBackend::Feed::Accepted: {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_->type == TrackType::Video) {
                ++snap_.video_packets;
                if (pending_->pts_us != kNoTime) snap_.last_video_pts_us = std::max(snap_.last_video_pts_us, pending_->pts_us);
            } else {
                ++snap_.audio_packets;
            }
            // Start the position clock at the first packet after a load or seek.
            const bool primary = pending_->type == TrackType::Video || !snap_.format.video;
            if (primary && pending_->pts_us != kNoTime) {
                last_primary_pts_ = std::max(last_primary_pts_ == kNoTime ? pending_->pts_us : last_primary_pts_, pending_->pts_us);
            }
            if (!clock_running_ && primary && pending_->pts_us != kNoTime) {
                clock_base_us_ = pending_->pts_us;
                clock_start_ = std::chrono::steady_clock::now();
                clock_running_ = true;
            }
            pending_.reset();
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
