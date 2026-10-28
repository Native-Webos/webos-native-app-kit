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
    o.webos_major = 6;
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
    o.webos_major = 6;
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
        o.webos_major = 6;
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
    o.webos_major = 6;
    StarfishBackend b(o);
    setenv("WNK_FAKE_SF_REFUSE", "1", 1);
    std::string err;
    CHECK(!b.load(h264(), &err));
    CHECK(!err.empty());
    unsetenv("WNK_FAKE_SF_REFUSE");
}
