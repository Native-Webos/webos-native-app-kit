// SPDX-License-Identifier: GPL-3.0-or-later
// StarfishBackend's Unload deadline, against tests/fakes/fake_player_apis.cpp (on LD_LIBRARY_PATH).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/media/starfish_backend.hpp"

#include <chrono>
#include <cstdlib>
#include <thread>

#include <dlfcn.h>

using namespace wnk::media;

namespace {
StreamFormat h264() {
    StreamFormat f;
    VideoFormat v;
    v.codec = CodecId::H264;
    v.width = 1920;
    v.height = 1080;
    f.video = v;
    return f;
}
int destroyed() {
    auto fn = reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "wnk_fake_sf_destroyed"));
    return fn != nullptr ? fn() : -1;
}
double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
}  // namespace

TEST("starfish release: an Unload that returns in time destroys the object before unload() returns") {
    std::string why;
    CHECK(StarfishBackend::available(&why));
    StarfishOptions o;
    o.app_id = "test";
    o.webos_major = 6;  // no exported window on the host: these splice
    StarfishBackend b(o);
    std::string err;
    CHECK(b.load(h264(), &err));
    const int before = destroyed();
    setenv("WNK_FAKE_SF_UNLOAD_MS", "50", 1);
    b.unload();
    CHECK_EQ(destroyed(), before + 1);
}

TEST("starfish release: a hung Unload costs the deadline, the next Load fails typed, then recovers") {
    StarfishOptions o;
    o.app_id = "test";
    o.webos_major = 6;  // no exported window on the host: these splice
    o.release_deadline = std::chrono::milliseconds(300);
    StarfishBackend b(o);
    std::string err;
    CHECK(b.load(h264(), &err));
    const int before = destroyed();
    setenv("WNK_FAKE_SF_UNLOAD_MS", "1500", 1);
    auto t = std::chrono::steady_clock::now();
    b.unload();
    const double waited = ms_since(t);
    CHECK(waited >= 250 && waited < 1000);  // the deadline, not the firmware call
    CHECK_EQ(destroyed(), before);           // parked, not destroyed under the caller
    setenv("WNK_FAKE_SF_UNLOAD_MS", "0", 1);
    t = std::chrono::steady_clock::now();
    CHECK(!b.load(h264(), &err));
    CHECK(err.rfind("release_timeout", 0) == 0);
    CHECK(ms_since(t) < 250);  // the release's own deadline already passed: no second wait
    std::this_thread::sleep_for(std::chrono::milliseconds(1400));
    CHECK_EQ(destroyed(), before + 1);  // the worker finished the tail
    err.clear();
    CHECK(b.load(h264(), &err));
    b.unload();
}

TEST("starfish release: a backend destroyed while its Unload hangs leaves the worker safe") {
    const int before = destroyed();
    {
        StarfishOptions o;
        o.app_id = "test";
        o.webos_major = 6;  // no exported window on the host: these splice
        o.release_deadline = std::chrono::milliseconds(100);
        StarfishBackend b(o);
        std::string err;
        CHECK(b.load(h264(), &err));
        setenv("WNK_FAKE_SF_UNLOAD_MS", "400", 1);
    }  // ~StarfishBackend -> unload() returns after 100 ms; the late UnloadCompleted finds no backend
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK_EQ(destroyed(), before + 1);
    setenv("WNK_FAKE_SF_UNLOAD_MS", "0", 1);
}

TEST("starfish release: a refused Load reports it") {
    StarfishOptions o;
    o.app_id = "test";
    o.webos_major = 6;  // no exported window on the host: these splice
    StarfishBackend b(o);
    setenv("WNK_FAKE_SF_REFUSE", "1", 1);
    std::string err;
    CHECK(!b.load(h264(), &err));
    CHECK(!err.empty());
    unsetenv("WNK_FAKE_SF_REFUSE");
}

namespace {
template <typename R>
R fake(const char *name) {
    auto fn = reinterpret_cast<R (*)()>(dlsym(RTLD_DEFAULT, name));
    return fn != nullptr ? fn() : R(-1);
}
Packet video(int64_t pts_us, bool key = true) {
    Packet p;
    p.type = TrackType::Video;
    p.pts_us = pts_us;
    p.key = key;
    p.data.assign(16, 0);
    return p;
}
// The fake has no exported window, so native seek is chosen as on webOS 3.
StarfishOptions seeking() {
    StarfishOptions o;
    o.app_id = "test";
    o.webos_major = 3;
    return o;
}
}  // namespace

TEST("starfish seek: flush makes the next packet issue LG's Seek at its own time, in content time") {
    StarfishBackend b(seeking());
    CHECK(b.native_seek());
    CHECK(!b.caps().seek_splices);
    std::string err;
    CHECK(b.load(h264(), &err));
    CHECK(b.feed(video(0)) == MediaBackend::Feed::Accepted);
    CHECK_EQ(fake<long long>("wnk_fake_sf_last_pts_ns"), 100000000LL);  // a load starts at 100 ms
    const int seeks = fake<int>("wnk_fake_sf_seeks");
    CHECK(b.flush(&err));
    CHECK(b.feed(video(40000000)) == MediaBackend::Feed::Accepted);  // forwards to 40 s
    CHECK_EQ(fake<int>("wnk_fake_sf_seeks"), seeks + 1);
    CHECK_EQ(fake<long long>("wnk_fake_sf_last_seek_ms"), 40000LL);
    CHECK_EQ(fake<long long>("wnk_fake_sf_last_pts_ns"), 40000000000LL);
    CHECK(b.feed(video(40040000, false)) == MediaBackend::Feed::Accepted);
    CHECK_EQ(fake<int>("wnk_fake_sf_seeks"), seeks + 1);  // one Seek per flush
    CHECK(b.flush(&err));
    CHECK(b.feed(video(10000000)) == MediaBackend::Feed::Accepted);  // and backwards to 10 s
    CHECK_EQ(fake<long long>("wnk_fake_sf_last_seek_ms"), 10000LL);
    CHECK_EQ(fake<long long>("wnk_fake_sf_last_pts_ns"), 10000000000LL);
    CHECK(b.flush(&err));
    CHECK(b.feed(video(-80000)) == MediaBackend::Feed::Accepted);  // a negative first PTS stays >= 0
    CHECK_EQ(fake<long long>("wnk_fake_sf_last_seek_ms"), 0LL);
    CHECK_EQ(fake<long long>("wnk_fake_sf_last_pts_ns"), 0LL);
    b.unload();
}

TEST("starfish seek: a refused Seek reloads the decoder and splices from then on") {
    StarfishBackend b(seeking());
    std::string err;
    CHECK(b.load(h264(), &err));
    CHECK(b.feed(video(0)) == MediaBackend::Feed::Accepted);
    const int loads = fake<int>("wnk_fake_sf_loads");
    setenv("WNK_FAKE_SF_SEEK_REFUSE", "1", 1);
    CHECK(b.flush(&err));
    CHECK(b.feed(video(30000000)) == MediaBackend::Feed::Accepted);
    unsetenv("WNK_FAKE_SF_SEEK_REFUSE");
    CHECK_EQ(fake<int>("wnk_fake_sf_loads"), loads + 1);                // reloaded
    CHECK_EQ(fake<long long>("wnk_fake_sf_last_pts_ns"), 100000000LL);  // a fresh load's start
    CHECK(!b.native_seek());
    CHECK(b.caps().seek_splices);
    const int seeks = fake<int>("wnk_fake_sf_seeks");
    CHECK(b.flush(&err));
    CHECK(b.feed(video(50000000)) == MediaBackend::Feed::Accepted);
    CHECK_EQ(fake<int>("wnk_fake_sf_seeks"), seeks);  // spliced, not sought
    b.unload();
}

TEST("starfish seek: off by option, and on webOS 4 (no exported window), seeks splice") {
    StarfishOptions o = seeking();
    o.native_seek = false;
    StarfishBackend off(o);
    CHECK(!off.native_seek());
    CHECK(off.caps().seek_splices);
    o = seeking();
    o.webos_major = 4;
    StarfishBackend four(o);
    CHECK(!four.native_seek());
}
