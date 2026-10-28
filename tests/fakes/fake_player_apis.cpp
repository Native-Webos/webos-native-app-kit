// SPDX-License-Identifier: GPL-3.0-or-later
//
// A host stand-in for LG's libplayerAPIs.so.1: the StarfishMediaAPIs entry points StarfishBackend
// looks up, under their mangled names, with behaviour set through the environment (read per call):
//   WNK_FAKE_SF_REFUSE=1        Load returns false
//   WNK_FAKE_SF_UNLOAD_MS=N     Unload returns (and sends UnloadCompleted) after N ms
//   WNK_FAKE_SF_SEEK_REFUSE=1   Seek returns false
// wnk_fake_sf_destroyed() counts destructor calls, wnk_fake_sf_loads() Loads, wnk_fake_sf_seeks()
// Seeks; wnk_fake_sf_last_seek_ms() is the last Seek's argument and wnk_fake_sf_last_pts_ns() the
// "pts" of the last Feed.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>

namespace {
using EventCb = void (*)(int type, long long num, const char *str, void *ctx);
struct Object {
    EventCb cb;
    void *ctx;
};
std::atomic<int> g_destroyed{0}, g_loads{0}, g_seeks{0};
std::atomic<long long> g_last_seek_ms{-1}, g_last_pts_ns{-1};
long env_long(const char *name) {
    const char *v = std::getenv(name);
    return v != nullptr ? std::atol(v) : 0;
}
}  // namespace

extern "C" {
int wnk_fake_sf_destroyed() { return g_destroyed.load(); }
int wnk_fake_sf_loads() { return g_loads.load(); }
int wnk_fake_sf_seeks() { return g_seeks.load(); }
long long wnk_fake_sf_last_seek_ms() { return g_last_seek_ms.load(); }
long long wnk_fake_sf_last_pts_ns() { return g_last_pts_ns.load(); }

bool fake_seek(void *, const char *millis) __asm__("_ZN17StarfishMediaAPIs4SeekEPKc");
bool fake_seek(void *, const char *millis) {
    ++g_seeks;
    g_last_seek_ms = std::atoll(millis);
    return env_long("WNK_FAKE_SF_SEEK_REFUSE") == 0;
}

void fake_ctor(void *self, const char *) __asm__("_ZN17StarfishMediaAPIsC1EPKc");
void fake_ctor(void *self, const char *) { new (self) Object{nullptr, nullptr}; }

void fake_dtor(void *) __asm__("_ZN17StarfishMediaAPIsD1Ev");
void fake_dtor(void *) { ++g_destroyed; }

bool fake_load(void *self, const char *, EventCb cb, void *ctx) __asm__("_ZN17StarfishMediaAPIs4LoadEPKcPFvixS1_PvES2_");
bool fake_load(void *self, const char *, EventCb cb, void *ctx) {
    ++g_loads;
    if (env_long("WNK_FAKE_SF_REFUSE") != 0) return false;
    auto *o = static_cast<Object *>(self);
    o->cb = cb;
    o->ctx = ctx;
    cb(0x16, 0, "{\"loadCompleted\":true}", ctx);
    return true;
}

void fake_feed(void *sret, void *, const char *) __asm__("_ZN17StarfishMediaAPIs4FeedB5cxx11EPKc");
void fake_feed(void *sret, void *, const char *json) {
    if (const char *p = std::strstr(json, "\"pts\":")) g_last_pts_ns = std::atoll(p + 6);
    new (sret) std::string("Ok");
}

bool fake_play(void *) __asm__("_ZN17StarfishMediaAPIs4PlayEv");
bool fake_play(void *) { return true; }
bool fake_pause(void *) __asm__("_ZN17StarfishMediaAPIs5PauseEv");
bool fake_pause(void *) { return true; }
bool fake_push_eos(void *) __asm__("_ZN17StarfishMediaAPIs7pushEOSEv");
bool fake_push_eos(void *) { return true; }
void fake_notify_foreground(void *) __asm__("_ZN17StarfishMediaAPIs16notifyForegroundEv");
void fake_notify_foreground(void *) {}

bool fake_unload(void *self) __asm__("_ZN17StarfishMediaAPIs6UnloadEv");
bool fake_unload(void *self) {
    std::this_thread::sleep_for(std::chrono::milliseconds(env_long("WNK_FAKE_SF_UNLOAD_MS")));
    auto *o = static_cast<Object *>(self);
    if (o->cb != nullptr) o->cb(0x17, 0, "{\"unloadCompleted\":true}", o->ctx);
    return true;
}
}
