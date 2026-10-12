// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal synchronous Luna (webOS service bus) calls for native apps.
//
// Uses the firmware's libhelpers.so.2 (present in every inventoried release), loaded with dlopen so the
// binary keeps no DT_NEEDED on it. libhelpers delivers replies on its own thread; the call waits on a
// condition variable with a timeout. The caller's bus identity is the app's own (SDL-webOS registers
// the app through the same libhelpers). On non-webOS builds every call reports Unsupported.
//
// Pattern after mariotaku/commons-c platform/webos/luna-sync (MIT), with a timeout added.
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace wnk::platform {

enum class LunaStatus { Ok, Unsupported, CallFailed, Timeout };
const char *luna_status_name(LunaStatus s);

struct LunaReply {
    LunaStatus status = LunaStatus::Unsupported;
    std::string payload;  // raw JSON reply (only when status == Ok)
    std::string detail;   // why it failed
};

// Blocking one-shot call. Never call it from code that must keep rendering; it is meant for startup and
// for worker threads. `public_bus` selects the public (true) or private bus.
LunaReply luna_call_sync(const std::string &uri, const std::string &payload, int timeout_ms = 3000,
                         bool public_bus = true);

// A call that keeps replying (a Luna subscription, "subscribe":true in the payload). `on_reply` runs on
// libhelpers' thread for every reply until the handle is closed or destroyed; it must not block. The
// bus call itself is not cancelled (libhelpers has no cancel for HLunaServiceCall): closing only stops
// the deliveries, and the small call state stays allocated for replies that may still come.
class LunaSubscription {
 public:
    ~LunaSubscription() { close(); }
    void close();
    bool active() const;

    struct State;

 private:
    friend std::unique_ptr<LunaSubscription> luna_subscribe(const std::string &, const std::string &,
                                                            std::function<void(const std::string &)>, bool,
                                                            std::string *);
    std::shared_ptr<State> state_;
};

// Null with *error set when the call could not be made (not webOS, no libhelpers, refused).
std::unique_ptr<LunaSubscription> luna_subscribe(const std::string &uri, const std::string &payload,
                                                 std::function<void(const std::string &)> on_reply,
                                                 bool public_bus = true, std::string *error = nullptr);

}  // namespace wnk::platform
