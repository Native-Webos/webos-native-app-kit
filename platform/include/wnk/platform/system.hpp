// SPDX-License-Identifier: GPL-3.0-or-later
//
// Asking the TV to do things outside the app: launch another app, open a web page, show a
// notification, and tell whether the network is up.
//
// The calls block on a Luna round trip: run them on a worker (the app shell's Context::launch_app,
// open_url and system_toast do). Payloads and reply parsing are pure functions, tested on the host;
// off webOS every call reports LunaStatus::Unsupported.
//
// Sources: applicationManager/launch with "params" (moonlight-tv, client-webos, PlxNative); createToast
// with a sourceId equal to the caller's app id (PlxNative, webOS OSE notificationmgr); the browser as
// com.webos.app.browser with {"target": url} and connectionmanager/getStatus (LG and webOS OSE
// documentation). None of these has been seen working from a Developer Mode app of this kit yet.
#pragma once

#include "wnk/platform/luna.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace wnk::platform {

// ---- payloads and replies (pure) -------------------------------------------------------------------

// {"id": app_id, "params": params} (params must be a JSON object, or empty for none).
std::string launch_payload(const std::string &app_id, const std::string &params_json = {});
// The system browser on `url`.
std::string open_url_payload(const std::string &url);
// A notification card attributed to `source_id` (the app's own id), without a launch action.
std::string toast_payload(const std::string &source_id, const std::string &message);
// True for {"returnValue": true}; otherwise *why gets the service's errorText (or a description).
bool reply_ok(std::string_view reply_json, std::string *why = nullptr);

struct NetworkStatus {
    bool known = false;     // a status has been received (or, on the host, read)
    bool internet = false;  // the TV says it can reach the internet
    bool wired = false;     // Ethernet connected
    bool wifi = false;      // Wi-Fi connected
    bool connected() const { return wired || wifi; }
    std::string summary() const;  // "online (wired)", "offline (Wi-Fi, no internet)", "unknown"
    bool operator==(const NetworkStatus &o) const {
        return known == o.known && internet == o.internet && wired == o.wired && wifi == o.wifi;
    }
    bool operator!=(const NetworkStatus &o) const { return !(*this == o); }
};
// A com.webos.service.connectionmanager/getStatus reply (or subscription update).
bool parse_connection_status(std::string_view reply_json, NetworkStatus *out);
// The host's view (no connection manager): any non-loopback interface up with an IPv4 address.
NetworkStatus host_network_status();

// ---- calls (blocking; use a worker) ----------------------------------------------------------------

LunaReply launch_app(const std::string &app_id, const std::string &params_json = {});
LunaReply open_url(const std::string &url);
LunaReply system_toast(const std::string &source_id, const std::string &message);

// Connection status updates on libhelpers' thread (post them to the main thread). Null with *error
// off webOS or when the service refuses.
std::unique_ptr<LunaSubscription> subscribe_network_status(std::function<void(const NetworkStatus &)> on_change,
                                                           std::string *error = nullptr);

}  // namespace wnk::platform
