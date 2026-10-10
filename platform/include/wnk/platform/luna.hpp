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

}  // namespace wnk::platform
