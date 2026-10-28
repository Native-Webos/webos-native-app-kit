// SPDX-License-Identifier: GPL-3.0-or-later
// NdlBackend's unload deadline, against tests/fakes/fake_ndl.c (on LD_LIBRARY_PATH).
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/media/ndl_backend.hpp"
#include "wnk/platform/media_role.hpp"

#include <chrono>
#include <cstdlib>
#include <thread>

#include <dlfcn.h>

using namespace wnk::media;

namespace {
struct Counts {
    int init = 0, load = 0, unload = 0, quit = 0;
};
Counts counts() {
    Counts c;
    // NdlBackend opens the library RTLD_LOCAL: look the counter up through its own handle.
    void *lib = dlopen("libNDL_directmedia.so.1", RTLD_NOW | RTLD_NOLOAD);
    auto fn = lib != nullptr ? reinterpret_cast<void (*)(int *, int *, int *, int *)>(dlsym(lib, "wnk_fake_ndl_counts")) : nullptr;
    if (fn != nullptr) fn(&c.init, &c.load, &c.unload, &c.quit);
    return c;
}
StreamFormat h264() {
    StreamFormat f;
    VideoFormat v;
    v.codec = CodecId::H264;
    v.width = 1920;
    v.height = 1080;
    f.video = v;
    return f;
}
double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
NdlOptions quick() {
    NdlOptions o;
    o.app_id = "test";
    o.release_deadline = std::chrono::milliseconds(300);
    return o;
}
}  // namespace

TEST("ndl release: an unload that returns in time is waited for") {
    std::string why;
    REQUIRE(NdlBackend::available(&why));
    NdlBackend b(quick());
    std::string err;
    CHECK(b.load(h264(), &err));
    const Counts before = counts();
    setenv("WNK_FAKE_NDL_UNLOAD_MS", "50", 1);
    b.unload();
    CHECK_EQ(counts().unload, before.unload + 1);
}

TEST("ndl release: a hung unload costs the deadline; placing, loading and quitting wait for it") {
    const Counts before = counts();
    {
        NdlBackend b(quick());
        std::string err;
        CHECK(b.load(h264(), &err));
        setenv("WNK_FAKE_NDL_UNLOAD_MS", "1500", 1);
        auto t = std::chrono::steady_clock::now();
        b.unload();
        CHECK(ms_since(t) >= 250 && ms_since(t) < 1000);  // the deadline, not NDL
        CHECK_EQ(counts().unload, before.unload);
        t = std::chrono::steady_clock::now();
        b.set_area({0, 0, 1280, 720});  // never blocks on the unload's lock
        CHECK(ms_since(t) < 50);
        setenv("WNK_FAKE_NDL_UNLOAD_MS", "0", 1);
        t = std::chrono::steady_clock::now();
        CHECK(!b.load(h264(), &err));
        CHECK(err.rfind("release_timeout", 0) == 0);
        CHECK(ms_since(t) < 100);  // that unload's deadline already passed
    }  // the last backend: NDL_DirectMediaQuit is deferred to the worker
    CHECK_EQ(counts().quit, before.quit);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const Counts after = counts();
    CHECK_EQ(after.unload, before.unload + 1);
    CHECK_EQ(after.quit, before.quit + 1);  // quit once the unload returned
    NdlBackend b(quick());
    std::string err;
    CHECK(b.load(h264(), &err));  // a fresh init, then a load
    CHECK_EQ(counts().init, after.init + 1);
    b.unload();
}

TEST("media role: only a template naming the player bus asks for the probe; none here") {
    CHECK(wnk::platform::media_role_template_names_player_bus(R"({"permissions":[{"service":"com.webos.media.smp.*"}]})"));
    CHECK(!wnk::platform::media_role_template_names_player_bus(R"({"permissions":[{"service":"com.webos.app.*"}]})"));
    std::string why;
    CHECK(wnk::platform::media_role_ok(&why));  // the host has no /usr/share/rolegen template
    CHECK(why.empty());
}
