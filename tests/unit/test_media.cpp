// SPDX-License-Identifier: GPL-3.0-or-later
//
// PlaybackController tests with two test doubles, named for what they are:
//   ScriptedSource — a timeline of packets (video every 40 ms, keyframe every second, audio every
//                    20 ms), seekable to the keyframe at or before a target; no demuxing.
//   PacedBackend   — accepts packets only while they are at most `lead` ahead of a clock running
//                    `speed` times faster than real time, which is how a decoder's buffer fills and
//                    drains; records what it was asked to do. It decodes nothing.
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/media/auto_backend.hpp"
#include "wnk/media/controller.hpp"
#include "wnk/media/device_caps.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace wnk;
using namespace wnk::media;
using namespace std::chrono_literals;

namespace wnk::media {
std::ostream &operator<<(std::ostream &os, PlayState s) { return os << play_state_name(s); }
}  // namespace wnk::media

namespace {

class ScriptedSource : public MediaSource {
 public:
    explicit ScriptedSource(int64_t duration_us = 2000000, bool video = true, bool audio = true)
        : duration_(duration_us), video_(video), audio_(audio) {}

    std::atomic<int> seeks{0};
    std::atomic<int64_t> last_seek{kNoTime};
    bool select_ok = true;
    bool subtitles = false;  // a text track (index 2) with a 0.5 s cue "cue N" at every second
    int64_t audio_early_us = 0;  // audio stored this far ahead of the video it plays with
    std::atomic<int> video_read{0};  // since open or the last seek

    bool open(const std::string &uri, std::string *error) override {
        if (uri == "block") {
            while (!interrupted_) std::this_thread::sleep_for(1ms);
            *error = "interrupted";
            return false;
        }
        if (uri == "missing") {
            *error = "no such file";
            return false;
        }
        info_ = MediaInfo{};
        info_.uri = uri;
        info_.container = "scripted";
        info_.duration_us = duration_;
        info_.seekable = true;
        if (video_) {
            TrackInfo v;
            v.index = 0;
            v.type = TrackType::Video;
            v.codec = CodecId::H264;
            v.width = 1280;
            v.height = 720;
            v.frame_rate = 25;
            info_.tracks.push_back(v);
        }
        if (audio_) {
            TrackInfo a;
            a.index = 1;
            a.type = TrackType::Audio;
            a.codec = CodecId::Aac;
            a.sample_rate = 48000;
            a.channels = 2;
            info_.tracks.push_back(a);
        }
        if (subtitles) {
            TrackInfo t;
            t.index = 2;
            t.type = TrackType::Subtitle;
            t.subtitle = SubtitleKind::Text;
            info_.tracks.push_back(t);
        }
        vpts_ = 0;
        apts_ = 0;
        sub_next_ = 0;
        video_read = 0;
        return true;
    }
    const MediaInfo &info() const override { return info_; }
    bool select(int v, int a, const BackendCaps &caps, StreamFormat *out, std::string *error) override {
        if (!select_ok) {
            *error = "codec not supported";
            return false;
        }
        if (v >= 0) out->video = VideoFormat{CodecId::H264, 1280, 720, 25};
        if (a >= 0) {
            // As a real source would: decode to PCM when the backend does not take AAC.
            out->audio = AudioFormat{caps.accepts(CodecId::Aac) ? CodecId::Aac : CodecId::PcmS16, 48000, 2, {}};
        }
        return true;
    }
    Read read(Packet *out, std::string *) override {
        if (interrupted_) return Read::Interrupted;
        const bool v_more = video_ && vpts_ < duration_;
        const bool a_more = audio_ && apts_ < duration_;
        if (!v_more && !a_more) return Read::EndOfStream;
        if (subtitles && sub_next_ < duration_ && sub_next_ <= std::min(vpts_, apts_)) {
            auto cue = std::make_shared<SubtitleCue>();
            cue->track = 2;
            cue->start_us = sub_next_;
            cue->end_us = sub_next_ + 500000;
            cue->text = "cue " + std::to_string(sub_next_ / 1000000);
            out->type = TrackType::Subtitle;
            out->pts_us = sub_next_;
            out->data.clear();
            out->subtitle = std::move(cue);
            sub_next_ += 1000000;
            return Read::Packet;
        }
        // Interleave by timestamp, like a muxed file (audio_early_us shifts audio earlier in the file).
        if (v_more && (!a_more || vpts_ <= apts_ - audio_early_us)) {
            ++video_read;
            out->type = TrackType::Video;
            out->pts_us = vpts_;
            out->key = vpts_ % 1000000 == 0;
            out->data.assign(out->key ? 4000 : 800, 0);
            vpts_ += 40000;
        } else {
            out->type = TrackType::Audio;
            out->pts_us = apts_;
            out->key = true;
            out->data.assign(200, 0);
            apts_ += 20000;
        }
        return Read::Packet;
    }
    bool seek(int64_t pts_us, std::string *) override {
        ++seeks;
        last_seek = pts_us;
        vpts_ = std::min(duration_, pts_us / 1000000 * 1000000);  // keyframe at or before
        apts_ = vpts_;
        sub_next_ = vpts_;
        video_read = 0;
        return true;
    }
    void interrupt() override { interrupted_ = true; }
    void reset_interrupt() override { interrupted_ = false; }

 private:
    MediaInfo info_;
    int64_t duration_;
    bool video_, audio_;
    int64_t vpts_ = 0, apts_ = 0, sub_next_ = 0;
    std::atomic<bool> interrupted_{false};
};

class PacedBackend : public MediaBackend {
 public:
    struct Log {
        std::atomic<int> loads{0}, unloads{0}, flushes{0};
        std::atomic<int> video{0}, audio{0};
        std::atomic<int64_t> first_video_after_flush{kNoTime};
        std::atomic<bool> first_was_key{false};
        std::atomic<bool> pause_drains{false};
        std::atomic<bool> seek_splices{false};
        std::atomic<int64_t> last_video_pts{kNoTime};
        const std::atomic<int> *source_video_read = nullptr;
        std::atomic<int> video_read_at_first_feed{-1};
        std::atomic<int> starts{0};  // times a first video packet arrived after a load or flush
        AudioFormat audio_format;
        std::mutex m;
    };
    PacedBackend(std::shared_ptr<Log> log, bool can_pause = false, double speed = 20.0, int64_t lead_us = 500000)
        : log_(std::move(log)), can_pause_(can_pause), speed_(speed), lead_(lead_us) {}

    const char *name() const override { return "paced-test-backend"; }
    BackendCaps caps() const override {
        BackendCaps c;
        c.video_codecs = {CodecId::H264};
        c.audio_codecs = {CodecId::PcmS16};
        c.pause = can_pause_;
        c.pause_drains = log_->pause_drains;
        c.seek_splices = log_->seek_splices;
        c.splice_gap_us = log_->seek_splices ? 100000 : 0;
        return c;
    }
    void set_event_callback(EventFn fn) override { events_ = std::move(fn); }
    bool load(const StreamFormat &f, std::string *) override {
        ++log_->loads;
        if (f.audio) {
            std::lock_guard<std::mutex> lock(log_->m);
            log_->audio_format = *f.audio;
        }
        restart();
        return true;
    }
    Feed feed(const Packet &p) override {
        if (base_ == kNoTime) {
            base_ = p.pts_us;
            start_ = std::chrono::steady_clock::now();
        }
        const auto wall = std::chrono::steady_clock::now() - start_;
        const int64_t presented = base_ + static_cast<int64_t>(
                                              std::chrono::duration_cast<std::chrono::microseconds>(wall).count() * speed_);
        if (p.pts_us > presented + lead_) return Feed::Full;
        if (p.type == TrackType::Video) {
            if (log_->first_video_after_flush == kNoTime) {
                log_->first_video_after_flush = p.pts_us;
                log_->first_was_key = p.key;
                ++log_->starts;
                if (log_->source_video_read != nullptr) log_->video_read_at_first_feed = log_->source_video_read->load();
            }
            log_->last_video_pts = p.pts_us;
            ++log_->video;
        } else {
            ++log_->audio;
        }
        return Feed::Accepted;
    }
    bool flush(std::string *) override {
        ++log_->flushes;
        restart();
        return true;
    }
    void unload() override { ++log_->unloads; }
    void set_area(const Rect &) override {}
    bool pause(bool) override { return can_pause_; }
    void emit(BackendEvent e) {
        if (events_) events_(e, "test");
    }

 private:
    void restart() {
        base_ = kNoTime;
        log_->first_video_after_flush = kNoTime;
    }
    std::shared_ptr<Log> log_;
    bool can_pause_;
    double speed_;
    int64_t lead_;
    int64_t base_ = kNoTime;
    std::chrono::steady_clock::time_point start_;
    EventFn events_;
};

struct Rig {
    async::MainDispatcher main;
    std::shared_ptr<PacedBackend::Log> log = std::make_shared<PacedBackend::Log>();
    ScriptedSource *source = nullptr;
    PacedBackend *backend = nullptr;
    std::unique_ptr<PlaybackController> pc;
    std::vector<std::pair<PlayState, PlayState>> transitions;

    explicit Rig(std::unique_ptr<ScriptedSource> src = std::make_unique<ScriptedSource>(), bool can_pause = false,
                 PlaybackOptions opts = {}) {
        source = src.get();
        log->source_video_read = &source->video_read;
        auto be = std::make_unique<PacedBackend>(log, can_pause);
        backend = be.get();
        pc = std::make_unique<PlaybackController>(std::move(src), std::move(be), main, opts);
        pc->set_state_callback([this](PlayState f, PlayState t, const PlaybackSnapshot &) { transitions.push_back({f, t}); });
    }
    // Drains the dispatcher until `pred` holds or the timeout passes.
    template <typename P>
    bool wait(P pred, std::chrono::milliseconds timeout = 5000ms) {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < end) {
            main.drain();
            if (pred()) return true;
            std::this_thread::sleep_for(2ms);
        }
        main.drain();
        return pred();
    }
    bool wait_state(PlayState s, std::chrono::milliseconds timeout = 5000ms) {
        // The state is published before its callback is posted: wait for the callback too.
        return wait([&] { return pc->state() == s && !transitions.empty() && transitions.back().second == s; }, timeout);
    }
    bool saw(PlayState from, PlayState to) const {
        for (const auto &t : transitions) {
            if (t.first == from && t.second == to) return true;
        }
        return false;
    }
};

}  // namespace

TEST("controller: open, play to the end, every packet fed in order") {
    Rig r;
    r.pc->open("file:///clip.mp4");
    REQUIRE(r.wait_state(PlayState::Ended));
    CHECK_EQ(r.log->video.load(), 50);  // 2 s at 25 fps
    CHECK_EQ(r.log->audio.load(), 100);
    CHECK_EQ(r.log->loads.load(), 1);
    CHECK(r.saw(PlayState::Idle, PlayState::Opening));
    CHECK(r.saw(PlayState::Opening, PlayState::Ready));
    CHECK(r.saw(PlayState::Ready, PlayState::Playing));
    CHECK(r.saw(PlayState::Playing, PlayState::Ended));
    const auto s = r.pc->snapshot();
    CHECK_EQ(s.duration_us, int64_t{2000000});
    CHECK_EQ(s.position_us, int64_t{2000000});
    CHECK(s.full_retries > 0);  // the backend's buffer paced the feeding
    CHECK(s.format.audio && s.format.audio->codec == CodecId::PcmS16);  // backend does not take AAC
    CHECK(r.pc->stop());
}

TEST("controller: a backend that never reports Full is still fed at most max_lead ahead") {
    // Speed 1000x and a huge lead: the backend accepts everything immediately, like NDL v2.
    async::MainDispatcher main;
    auto log = std::make_shared<PacedBackend::Log>();
    PlaybackOptions opts;
    opts.max_lead_us = 500000;
    PlaybackController pc(std::make_unique<ScriptedSource>(10000000),
                          std::make_unique<PacedBackend>(log, false, 1000.0, int64_t{1} << 40), main, opts);
    pc.open("x");
    std::this_thread::sleep_for(300ms);
    const auto s = pc.snapshot();
    // After 0.3 s of a 10 s clip: about 0.3 s presented + 0.5 s lead fed, nowhere near all of it.
    CHECK(s.last_video_pts_us <= 900000);
    CHECK(s.last_video_pts_us >= 500000);
    CHECK_EQ(s.full_retries, uint64_t{0});
    pc.stop();
}

TEST("controller: open and selection failures end in Error with the reason") {
    Rig r;
    r.pc->open("missing");
    REQUIRE(r.wait_state(PlayState::Error));
    CHECK(r.pc->snapshot().error.find("no such file") != std::string::npos);
    auto src = std::make_unique<ScriptedSource>();
    src->select_ok = false;
    Rig r2(std::move(src));
    r2.pc->open("x");
    REQUIRE(r2.wait_state(PlayState::Error));
    CHECK(r2.pc->snapshot().error.find("codec not supported") != std::string::npos);
    CHECK_EQ(r2.log->loads.load(), 0);
}

TEST("controller: pause without backend pause releases the decoder; resume reloads and seeks back") {
    Rig r(std::make_unique<ScriptedSource>(20000000));  // 20 s
    r.pc->open("x");
    REQUIRE(r.wait([&] { return r.log->video.load() > 30; }));
    r.pc->pause();
    REQUIRE(r.wait_state(PlayState::Paused));
    CHECK_EQ(r.log->unloads.load(), 1);
    const int64_t paused_at = r.pc->snapshot().position_us;
    CHECK(paused_at > 0);
    const int fed = r.log->video.load();
    std::this_thread::sleep_for(50ms);
    CHECK_EQ(r.log->video.load(), fed);  // nothing fed while paused
    CHECK_EQ(r.pc->snapshot().position_us, paused_at);  // the clock is frozen
    r.pc->play();
    REQUIRE(r.wait([&] { return r.log->video.load() > fed; }));
    CHECK_EQ(r.log->loads.load(), 2);
    CHECK_EQ(r.source->seeks.load(), 1);
    CHECK_EQ(r.source->last_seek.load(), paused_at);
    CHECK(r.log->first_was_key.load());  // resumed from a keyframe
    r.pc->stop();
}

TEST("controller: with backend pause the decoder stays loaded") {
    Rig r(std::make_unique<ScriptedSource>(20000000), /*can_pause=*/true);
    r.pc->open("x");
    REQUIRE(r.wait([&] { return r.log->video.load() > 10; }));
    r.pc->pause();
    REQUIRE(r.wait_state(PlayState::Paused));
    r.pc->play();
    REQUIRE(r.wait_state(PlayState::Playing));
    CHECK_EQ(r.log->unloads.load(), 0);
    CHECK_EQ(r.log->loads.load(), 1);
    CHECK_EQ(r.source->seeks.load(), 0);
    r.pc->stop();
}

TEST("controller: a draining pause stops the clock at the last fed frame and resumes from there") {
    PlaybackOptions o;
    o.max_lead_us = 2000000;  // well ahead of the clock, so the difference is visible
    Rig r(std::make_unique<ScriptedSource>(60000000), /*can_pause=*/true, o);
    r.log->pause_drains = true;
    r.pc->open("x");
    REQUIRE(r.wait([&] { return r.log->video.load() > 30; }));
    r.pc->pause();
    REQUIRE(r.wait_state(PlayState::Paused));
    const int64_t fed = r.log->video.load();
    const int64_t paused_at = r.pc->snapshot().position_us;
    CHECK_EQ(paused_at, r.log->last_video_pts.load());
    std::this_thread::sleep_for(50ms);
    CHECK_EQ(r.log->video.load(), fed);  // nothing fed while paused
    r.pc->play();
    REQUIRE(r.wait_state(PlayState::Playing));
    REQUIRE(r.wait([&] { return r.log->video.load() > fed; }));
    CHECK(r.pc->snapshot().position_us >= paused_at);
    CHECK_EQ(r.log->unloads.load(), 0);
    CHECK_EQ(r.source->seeks.load(), 0);
    r.pc->stop();
}

TEST("controller: pause_and_release unloads even a pausing backend, also when already paused; resume seeks back") {
    Rig r(std::make_unique<ScriptedSource>(20000000), /*can_pause=*/true);
    r.log->pause_drains = true;
    r.pc->open("x");
    REQUIRE(r.wait([&] { return r.log->video.load() > 10; }));
    r.pc->pause_and_release();
    REQUIRE(r.wait_state(PlayState::Paused));
    CHECK_EQ(r.log->unloads.load(), 1);
    r.pc->play();
    REQUIRE(r.wait_state(PlayState::Playing));
    CHECK_EQ(r.log->loads.load(), 2);
    CHECK_EQ(r.source->seeks.load(), 1);
    // Held pause first, then released (the app goes to the background while paused).
    r.pc->pause();
    REQUIRE(r.wait_state(PlayState::Paused));
    CHECK_EQ(r.log->unloads.load(), 1);
    r.pc->pause_and_release();
    REQUIRE(r.wait([&] { return r.log->unloads.load() == 2; }));
    r.pc->play();
    REQUIRE(r.wait_state(PlayState::Playing));
    CHECK_EQ(r.log->loads.load(), 3);
    r.pc->stop();
}

TEST("controller: subtitle packets go to the store, never to the backend; the selected track shows at the position") {
    auto src = std::make_unique<ScriptedSource>(3000000);
    src->subtitles = true;
    Rig r(std::move(src));
    r.pc->open("x");
    CHECK(r.pc->subtitles_now().empty());  // no track selected
    r.pc->set_subtitle_track(2);
    std::string seen;
    REQUIRE(r.wait([&] {
        const auto cues = r.pc->subtitles_now();
        const int64_t pos = r.pc->snapshot().position_us;
        if (!cues.empty() && pos >= 1050000 && pos < 1450000) seen = cues[0]->text;
        return !seen.empty();
    }, 4000ms));
    CHECK_EQ(seen, std::string("cue 1"));
    REQUIRE(r.wait_state(PlayState::Ended, 5000ms));
    CHECK_EQ(r.log->audio.load(), 150);  // 3 s of audio only: no subtitle reached the backend
    CHECK_EQ(r.pc->subtitle_store().count(2), 3u);
    r.pc->set_subtitle_track(-1);
    CHECK(r.pc->subtitles_now().empty());
    r.pc->stop();
}

TEST("controller: audio stored far ahead of its video does not hold the video back") {
    PlaybackOptions o;
    o.max_lead_us = 1000000;
    auto src = std::make_unique<ScriptedSource>(8000000);
    src->audio_early_us = 2500000;  // in file order, audio runs 2.5 s ahead of the video
    Rig r(std::move(src), false, o);
    r.pc->open("x");
    REQUIRE(r.wait([&] { return r.pc->snapshot().position_us > 1500000; }, 4000ms));
    // Sample the video lead for a while: a single file-order lane would starve it below zero.
    int64_t worst = INT64_MAX;
    for (int i = 0; i < 20; ++i) {
        const auto snap = r.pc->snapshot();
        worst = std::min(worst, snap.last_video_pts_us - snap.position_us);
        std::this_thread::sleep_for(50ms);
    }
    CHECK(worst > 500000);
    r.pc->stop();
}

TEST("controller: after a seek nothing is fed until the lanes are primed") {
    auto src = std::make_unique<ScriptedSource>(20000000);
    Rig r(std::move(src));
    r.pc->open("x");
    REQUIRE(r.wait([&] { return r.log->video.load() > 10; }));
    r.pc->seek(10000000);
    REQUIRE(r.wait([&] { return r.log->flushes.load() == 1 && r.log->first_video_after_flush.load() != kNoTime; }));
    // 0.7 s of 25 fps video (18 frames, the span counts the last frame's duration) had been read
    // since the seek when the first one went in.
    CHECK(r.log->video_read_at_first_feed.load() >= 17);
    r.pc->stop();
}

TEST("controller: a burst of seeks on a splicing backend feeds only the last target") {
    Rig r(std::make_unique<ScriptedSource>(60000000), /*can_pause=*/true);
    r.log->pause_drains = true;
    r.log->seek_splices = true;
    r.pc->open("x");
    REQUIRE(r.wait([&] { return r.log->video.load() > 10; }));
    const int starts_before = r.log->starts.load();
    for (int64_t t : {5000000, 10000000, 15000000, 20000000, 25000000}) {
        r.pc->seek(t);
        std::this_thread::sleep_for(100ms);
    }
    REQUIRE(r.wait([&] { return r.log->flushes.load() >= 1 && r.log->first_video_after_flush.load() == 25000000; }, 3000ms));
    std::this_thread::sleep_for(100ms);
    CHECK_EQ(r.log->starts.load() - starts_before, 1);  // no piece of 5, 10, 15 or 20 s was fed
    r.pc->stop();
}

TEST("controller: seek flushes, restarts at a keyframe, and only the latest of a burst runs") {
    Rig r(std::make_unique<ScriptedSource>(60000000));  // 60 s
    r.pc->open("x");
    REQUIRE(r.wait([&] { return r.log->video.load() > 10; }));
    r.pc->seek(10000000);
    r.pc->seek(20000000);
    r.pc->seek(35500000);  // a burst, like a held right arrow
    REQUIRE(r.wait([&] { return r.source->last_seek.load() == 35500000 && r.log->first_video_after_flush.load() != kNoTime; }));
    CHECK(r.source->seeks.load() <= 2);  // coalesced
    CHECK_EQ(r.log->first_video_after_flush.load(), int64_t{35000000});
    CHECK(r.log->first_was_key.load());
    CHECK(r.log->flushes.load() >= 1);
    REQUIRE(r.wait_state(PlayState::Playing));
    const int64_t pos = r.pc->snapshot().position_us;
    CHECK(pos >= 35000000 && pos < 45000000);
    r.pc->stop();
}

TEST("controller: seeking while paused stays paused at the target") {
    Rig r(std::make_unique<ScriptedSource>(30000000));
    r.pc->open("x");
    REQUIRE(r.wait([&] { return r.log->video.load() > 5; }));
    r.pc->pause();
    REQUIRE(r.wait_state(PlayState::Paused));
    r.pc->seek(12000000);
    REQUIRE(r.wait([&] { return r.source->last_seek.load() == 12000000 && r.pc->state() == PlayState::Paused; }));
    CHECK_EQ(r.pc->snapshot().position_us, int64_t{12000000});
    r.pc->stop();
}

TEST("controller: stop interrupts a blocked open and joins quickly") {
    Rig r;
    r.pc->open("block");
    REQUIRE(r.wait_state(PlayState::Opening));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(r.pc->stop());
    CHECK(std::chrono::steady_clock::now() - t0 < 1s);
    CHECK_EQ(r.pc->state(), PlayState::Stopped);
}

TEST("controller: losing the decoder resource pauses and unloads, even with a pausing backend") {
    Rig r(std::make_unique<ScriptedSource>(20000000), /*can_pause=*/true);
    r.log->pause_drains = true;
    r.pc->open("x");
    REQUIRE(r.wait([&] { return r.log->video.load() > 10; }));
    r.backend->emit(BackendEvent::ResourceLost);
    REQUIRE(r.wait_state(PlayState::Paused));
    CHECK_EQ(r.log->unloads.load(), 1);
    r.pc->stop();
}

TEST("controller: audio-only media plays and ends on the audio clock; Ended + play restarts") {
    Rig r(std::make_unique<ScriptedSource>(1000000, /*video=*/false, /*audio=*/true));
    const auto t0 = std::chrono::steady_clock::now();
    r.pc->open("x");
    REQUIRE(r.wait_state(PlayState::Ended));
    // Ended once the audio has had time to play out, not when the last packet was fed.
    CHECK(std::chrono::steady_clock::now() - t0 >= 900ms);
    CHECK_EQ(r.log->video.load(), 0);
    CHECK_EQ(r.log->audio.load(), 50);
    r.pc->play();
    REQUIRE(r.wait([&] { return r.source->seeks.load() == 1; }));
    CHECK_EQ(r.source->last_seek.load(), int64_t{0});
    r.pc->stop();
}

TEST("controller: destroying a playing controller stops it cleanly") {
    async::MainDispatcher main;
    auto log = std::make_shared<PacedBackend::Log>();
    {
        PlaybackController pc(std::make_unique<ScriptedSource>(60000000), std::make_unique<PacedBackend>(log), main);
        pc.open("x");
        std::this_thread::sleep_for(50ms);
    }
    CHECK(log->video.load() > 0);
    CHECK_EQ(log->unloads.load(), 1);
    main.drain();  // callbacks posted before destruction must not touch the dead controller
}

#include "wnk/media/ndl_backend.hpp"

TEST("ndl backend: PTS conversion, PCM rate table, and a clear failure where NDL is absent") {
    CHECK_EQ(NdlBackend::convert_pts(1500000, PtsUnit::Microseconds), 1500000LL);
    CHECK_EQ(NdlBackend::convert_pts(1500000, PtsUnit::Milliseconds), 1500LL);
    CHECK_EQ(NdlBackend::convert_pts(kNoTime, PtsUnit::Milliseconds), 0LL);
    CHECK_EQ(NdlBackend::pcm_rate_enum(48000), 1);
    CHECK_EQ(NdlBackend::pcm_rate_enum(44100), 2);
    CHECK_EQ(NdlBackend::pcm_rate_enum(96000), 0);
    std::string why;
    if (!NdlBackend::available(&why)) {  // the host: no LG library
        NdlBackend b;
        StreamFormat f;
        f.video = VideoFormat{CodecId::H264, 1920, 1080, 25};
        std::string err;
        CHECK(!b.load(f, &err));
        CHECK(err.find("NDL unavailable") != std::string::npos);
        CHECK(b.feed(Packet{}) == MediaBackend::Feed::Error);
    }
    const BackendCaps c = NdlBackend().caps();
    CHECK(c.accepts(CodecId::Hevc));
    CHECK(!c.accepts(CodecId::Aac));  // decoded to PCM by the source
    CHECK(c.pause && c.pause_drains && c.seek_splices);
    CHECK(c.splice_gap_us > 0);
    NdlOptions reload;
    reload.reload_to_pause_and_seek = true;
    const BackendCaps r = NdlBackend(reload).caps();
    CHECK(!r.pause && !r.seek_splices);
}

TEST("ndl splice: PTS after a seek continue just after the highest one fed, forwards and backwards") {
    PtsSplice s(200000);
    CHECK_EQ(s.map(0), int64_t{0});
    CHECK_EQ(s.map(40000), int64_t{40000});
    CHECK_EQ(s.map(1000000), int64_t{1000000});  // audio ahead of video
    CHECK_EQ(s.map(kNoTime), kNoTime);
    s.splice();                                  // seek forward to 30 s
    CHECK_EQ(s.map(30000000), int64_t{1200000});
    CHECK_EQ(s.map(30040000), int64_t{1240000});
    CHECK_EQ(s.map(29990000), int64_t{1190000}); // audio slightly before the keyframe keeps the offset
    s.splice();                                  // and back to 5 s
    CHECK_EQ(s.map(5000000), int64_t{1440000});
    CHECK_EQ(s.map(5040000), int64_t{1480000});
    s.reset();
    CHECK_EQ(s.map(7000000), int64_t{7000000});
}

TEST("controller: a splicing backend's new content waits for the backlog; the shown position holds at the target") {
    PlaybackOptions o;
    o.max_lead_us = 600000;
    Rig r(std::make_unique<ScriptedSource>(60000000), /*can_pause=*/true, o);
    r.log->pause_drains = true;
    r.log->seek_splices = true;
    r.pc->open("x");
    REQUIRE(r.wait([&] { return r.log->video.load() > 30; }));
    r.pc->seek(20000000);
    REQUIRE(r.wait([&] { return r.log->flushes.load() == 1 && r.log->first_video_after_flush.load() != kNoTime; }));
    const int64_t base = r.log->first_video_after_flush.load();
    // About 0.6 s queued before the seek plus a 0.1 s gap: the clock holds at the new base meanwhile.
    std::this_thread::sleep_for(250ms);
    CHECK_EQ(r.pc->snapshot().position_us, base);
    REQUIRE(r.wait([&] { return r.pc->snapshot().position_us > base + 100000; }, 3000ms));
    CHECK_EQ(r.log->unloads.load(), 0);
    CHECK_EQ(r.log->flushes.load(), 1);
    r.pc->stop();
}

TEST("adts: headers from AudioSpecificConfig for AAC-LC and HE-AAC; refusals") {
    uint8_t h[7];
    // AAC-LC 48 kHz stereo: object 2, index 3, channels 2 -> 0x11 0x90
    REQUIRE(adts_header({0x11, 0x90}, 100, h));
    CHECK_EQ(h[0], 0xFF);
    CHECK_EQ(h[1], 0xF1);
    CHECK_EQ(h[2], (1 << 6) | (3 << 2) | 0);       // profile LC, 48 kHz, channel bit 2 = 0
    const int len = ((h[3] & 3) << 11) | (h[4] << 3) | (h[5] >> 5);
    CHECK_EQ(len, 107);
    CHECK_EQ(h[3] >> 6, 2);                          // channels 2
    // HE-AAC v1 (object 5, 24 kHz core index 6, stereo, extension 48 kHz index 3, core object 2).
    // Bits: 00101 0110 0010 0011 00010 -> 0x2B 0x11 0x88 (padded)
    REQUIRE(adts_header({0x2B, 0x11, 0x88}, 10, h));
    CHECK_EQ(h[2] >> 6, 1);                          // core profile LC
    CHECK_EQ((h[2] >> 2) & 0xF, 6);                  // core rate index
    CHECK(!adts_header({0x12}, 10, h));              // too short
    CHECK(!adts_header({0x17, 0x80, 0x00, 0x00, 0x00}, 10, h));  // explicit frequency (index 15)
}

TEST("pts splice: unmap gives content time for timestamps from before and after a splice") {
    PtsSplice s(200000);
    CHECK_EQ(s.map(0), int64_t{0});
    CHECK_EQ(s.map(1000000), int64_t{1000000});
    s.splice();
    CHECK_EQ(s.map(30000000), int64_t{1200000});
    CHECK_EQ(s.unmap(500000), int64_t{500000});      // presented before the splice took effect
    CHECK_EQ(s.unmap(1200000), int64_t{30000000});
    CHECK_EQ(s.unmap(1300000), int64_t{30100000});
    CHECK_EQ(s.unmap(kNoTime), kNoTime);
}

TEST("device caps: parses a getConfigs reply (OLED65C1PTB, webOS 6.5.3); unknown stays unknown") {
    // Captured from the TV on 2026-10-11 (the keys this module asks for).
    const std::string reply =
        R"({"subscribed":false,"configs":{"tv.conti.supportHevc":true,"tv.hw.panelResolution":"UD",)"
        R"("tv.hw.panelOutputFrameRate":"120Hz","tv.model.supportHDR":true,"tv.config.supportDolbyHDRContents":true,)"
        R"("tv.config.supportDolbyTVATMOS":true,"tv.hw.SoCChipType":"O20N"},"returnValue":true})";
    const DeviceMediaCaps d = DeviceMediaCaps::parse(reply);
    CHECK(d.measured);
    CHECK(d.hevc == true);
    CHECK(d.uhd_panel == true);
    CHECK(d.panel_hz == 120);
    CHECK(d.hdr == true);
    CHECK(d.dolby_vision == true);
    CHECK(d.dolby_atmos == true);
    CHECK_EQ(d.soc, std::string("O20N"));
    CHECK_EQ(d.summary(), std::string("hevc yes, uhd yes, 120 Hz, hdr yes, dv yes, atmos yes, soc O20N"));

    const DeviceMediaCaps partial = DeviceMediaCaps::parse(
        R"({"configs":{"tv.hw.panelResolution":"FHD","tv.conti.supportHevc":"maybe"},"returnValue":true})");
    CHECK(partial.measured);
    CHECK(partial.uhd_panel == false);
    CHECK(!partial.hevc.has_value());  // wrong type: unknown, not "no"
    CHECK(!partial.dolby_vision.has_value());
    CHECK(!partial.panel_hz.has_value());

    const DeviceMediaCaps bad = DeviceMediaCaps::parse("not json");
    CHECK(!bad.measured);
    CHECK(bad.summary().find("not measured") == 0);
    CHECK(DeviceMediaCaps::request().find("tv.conti.supportHevc") != std::string::npos);
    // Off webOS the call is unsupported and says so.
    CHECK(!DeviceMediaCaps::detect(100).measured);
}

namespace {
// A backend that records what it is asked; caps are set by the test.
class RecordingBackend : public MediaBackend {
 public:
    RecordingBackend(const char *name, std::vector<CodecId> video, std::vector<CodecId> audio) : name_(name) {
        caps_.video_codecs = std::move(video);
        caps_.audio_codecs = std::move(audio);
    }
    const char *name() const override { return name_; }
    BackendCaps caps() const override { return caps_; }
    void prepare(const MediaInfo &, const TrackInfo *, const TrackInfo *) override { ++prepares; }
    void set_event_callback(EventFn) override {}
    bool load(const StreamFormat &, std::string *) override {
        ++loads;
        return true;
    }
    Feed feed(const Packet &) override {
        ++feeds;
        return Feed::Accepted;
    }
    bool flush(std::string *) override { return true; }
    void unload() override {}
    void set_area(const Rect &) override { ++areas; }
    std::atomic<int> prepares{0}, loads{0}, feeds{0}, areas{0};

 private:
    const char *name_;
    BackendCaps caps_;
};

TrackInfo track(TrackType type, CodecId codec, const char *label) {
    TrackInfo t;
    t.type = type;
    t.codec = codec;
    t.codec_label = label;
    return t;
}
}  // namespace

TEST("auto backend: the preferred one when it takes video and audio as they are, else the fallback") {
    auto pref = std::make_unique<RecordingBackend>("pref", std::vector<CodecId>{CodecId::H264, CodecId::Hevc},
                                                   std::vector<CodecId>{CodecId::Aac, CodecId::Ac3});
    auto fall = std::make_unique<RecordingBackend>("fall", std::vector<CodecId>{CodecId::H264, CodecId::Av1},
                                                   std::vector<CodecId>{CodecId::PcmS16});
    RecordingBackend *p = pref.get(), *f = fall.get();
    AutoBackend a(std::move(pref), std::move(fall));
    CHECK_EQ(std::string(a.name()), std::string("pref"));  // before prepare
    MediaInfo info;
    const TrackInfo h264 = track(TrackType::Video, CodecId::H264, "h264");
    const TrackInfo av1 = track(TrackType::Video, CodecId::Av1, "av1");
    const TrackInfo aac = track(TrackType::Audio, CodecId::Aac, "aac");
    const TrackInfo mp3 = track(TrackType::Audio, CodecId::Mp3, "mp3");

    a.prepare(info, &h264, &aac);
    CHECK(!a.using_fallback());
    std::string err;
    CHECK(a.load(StreamFormat{}, &err));
    CHECK_EQ(p->loads.load(), 1);

    a.prepare(info, &h264, &mp3);  // audio the preferred one cannot take
    CHECK(a.using_fallback());
    CHECK_EQ(std::string(a.name()), std::string("fall"));
    CHECK(a.caps().accepts(CodecId::PcmS16));
    CHECK(a.load(StreamFormat{}, &err));
    CHECK_EQ(f->loads.load(), 1);

    a.prepare(info, &av1, nullptr);  // video the preferred one cannot take
    CHECK(a.using_fallback());
    a.prepare(info, &h264, nullptr);  // video only, taken
    CHECK(!a.using_fallback());
    CHECK_EQ(p->prepares.load(), 2);
    CHECK_EQ(f->prepares.load(), 2);
    a.set_area({0, 0, 10, 10});  // both keep the area
    CHECK(p->areas.load() == 1 && f->areas.load() == 1);
    std::string why;
    CHECK(!AutoBackend::takes(*p, &h264, &mp3, &why));
    CHECK_EQ(why, std::string("audio mp3"));

    TrackInfo mp3_reencodable = mp3;  // the source can turn it into AC-3, which the preferred one takes
    mp3_reencodable.decodable = mp3_reencodable.ac3_encodable = true;
    a.prepare(info, &h264, &mp3_reencodable);
    CHECK(!a.using_fallback());
    TrackInfo mp3_decodable = mp3;  // PCM only: the fallback
    mp3_decodable.decodable = true;
    a.prepare(info, &h264, &mp3_decodable);
    CHECK(a.using_fallback());
}

TEST("controller: prepare reaches the backend before tracks are selected") {
    async::MainDispatcher main;
    auto be = std::make_unique<RecordingBackend>("rec", std::vector<CodecId>{CodecId::H264},
                                                 std::vector<CodecId>{CodecId::PcmS16});
    RecordingBackend *b = be.get();
    PlaybackController pc(std::make_unique<ScriptedSource>(), std::move(be), main);
    pc.open("x");
    const auto end = std::chrono::steady_clock::now() + 3s;
    while (b->loads.load() == 0 && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(5ms);
    CHECK_EQ(b->prepares.load(), 1);
    CHECK_EQ(b->loads.load(), 1);
    CHECK_EQ(std::string(pc.backend_name()), std::string("rec"));
    pc.stop();
    main.drain();
}

namespace {
// Load blocks until released, like a decoder that never gets its resources.
class StuckBackend : public RecordingBackend {
 public:
    StuckBackend() : RecordingBackend("stuck", {CodecId::H264}, {CodecId::PcmS16}) {}
    bool load(const StreamFormat &f, std::string *e) override {
        entered = true;
        while (!release) std::this_thread::sleep_for(2ms);
        return RecordingBackend::load(f, e);
    }
    static std::atomic<bool> entered, release;
};
std::atomic<bool> StuckBackend::entered{false};
std::atomic<bool> StuckBackend::release{false};
}  // namespace

TEST("controller: a playback thread stuck past the stop deadline outlives the controller safely") {
    async::MainDispatcher main;
    PlaybackOptions o;
    o.stop_deadline = 100ms;
    auto pc = std::make_unique<PlaybackController>(std::make_unique<ScriptedSource>(), std::make_unique<StuckBackend>(),
                                                   main, o);
    pc->open("x");
    const auto end = std::chrono::steady_clock::now() + 3s;
    while (!StuckBackend::entered && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(2ms);
    REQUIRE(StuckBackend::entered.load());
    CHECK(!pc->stop());  // detached
    pc.reset();          // the handle goes; the thread still holds the state
    StuckBackend::release = true;
    std::this_thread::sleep_for(300ms);  // the thread finishes, feeds, sees Stop and ends on its own
    main.drain();                        // nothing posted after destruction reaches a callback
}
