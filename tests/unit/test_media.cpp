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

#include "wnk/media/controller.hpp"

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
        vpts_ = 0;
        apts_ = 0;
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
        // Interleave by timestamp, like a muxed file.
        if (v_more && (!a_more || vpts_ <= apts_)) {
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
        return true;
    }
    void interrupt() override { interrupted_ = true; }
    void reset_interrupt() override { interrupted_ = false; }

 private:
    MediaInfo info_;
    int64_t duration_;
    bool video_, audio_;
    int64_t vpts_ = 0, apts_ = 0;
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
