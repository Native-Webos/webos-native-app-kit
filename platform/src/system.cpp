// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/platform/system.hpp"

#include <nlohmann/json.hpp>

#include <cstring>

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

namespace wnk::platform {
namespace {

constexpr const char *kLaunch = "luna://com.webos.applicationManager/launch";
constexpr const char *kToast = "luna://com.webos.notification/createToast";
constexpr const char *kStatus = "luna://com.webos.service.connectionmanager/getStatus";
constexpr const char *kSettings = "luna://com.webos.settingsservice/getSystemSettings";
constexpr const char *kSpeak = "luna://com.webos.service.tts/speak";
constexpr int kTimeoutMs = 3000;

bool connected_state(const nlohmann::json &j, const char *key) {
    return j.contains(key) && j[key].is_object() && j[key].value("state", std::string()) == "connected";
}

}  // namespace

std::string launch_payload(const std::string &app_id, const std::string &params_json) {
    nlohmann::json j = {{"id", app_id}};
    if (!params_json.empty()) {
        auto p = nlohmann::json::parse(params_json, nullptr, false);
        if (p.is_object()) j["params"] = std::move(p);
    }
    return j.dump();
}

std::string open_url_payload(const std::string &url) {
    return nlohmann::json{{"id", "com.webos.app.browser"}, {"params", {{"target", url}}}}.dump();
}

std::string toast_payload(const std::string &source_id, const std::string &message) {
    return nlohmann::json{{"sourceId", source_id}, {"noaction", true}, {"message", message}}.dump();
}

bool reply_ok(std::string_view reply_json, std::string *why) {
    const auto j = nlohmann::json::parse(reply_json, nullptr, false);
    if (j.is_object() && j.value("returnValue", false)) return true;
    if (why != nullptr) {
        if (!j.is_object()) {
            *why = "unreadable reply";
        } else if (j.contains("errorText") && j["errorText"].is_string()) {
            *why = j["errorText"].get<std::string>();
        } else {
            *why = "refused without errorText";
        }
    }
    return false;
}

std::string NetworkStatus::summary() const {
    if (!known) return "unknown";
    std::string how = wired && wifi ? "wired and Wi-Fi" : wired ? "wired" : wifi ? "Wi-Fi" : "";
    if (internet) return "online" + (how.empty() ? std::string() : " (" + how + ")");
    if (how.empty()) return "offline";
    return "offline (" + how + ", no internet)";
}

bool parse_connection_status(std::string_view reply_json, NetworkStatus *out) {
    const auto j = nlohmann::json::parse(reply_json, nullptr, false);
    if (!j.is_object() || !j.value("returnValue", true)) return false;
    if (!j.contains("isInternetConnectionAvailable") && !j.contains("wired") && !j.contains("wifi")) return false;
    NetworkStatus s;
    s.known = true;
    s.wired = connected_state(j, "wired");
    s.wifi = connected_state(j, "wifi");
    s.internet = j.value("isInternetConnectionAvailable", s.wired || s.wifi);
    *out = s;
    return true;
}

NetworkStatus host_network_status() {
    NetworkStatus s;
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0) return s;
    s.known = true;
    for (ifaddrs *i = list; i != nullptr; i = i->ifa_next) {
        if (i->ifa_addr == nullptr || i->ifa_addr->sa_family != AF_INET) continue;
        if ((i->ifa_flags & IFF_UP) == 0 || (i->ifa_flags & IFF_LOOPBACK) != 0) continue;
        const bool wireless = std::strncmp(i->ifa_name, "wl", 2) == 0;
        (wireless ? s.wifi : s.wired) = true;
    }
    freeifaddrs(list);
    s.internet = s.wired || s.wifi;  // the host cannot tell; a route is assumed
    return s;
}

bool parse_audio_guidance(std::string_view reply_json, bool *on) {
    const auto j = nlohmann::json::parse(reply_json, nullptr, false);
    if (!j.is_object() || !j.contains("settings") || !j["settings"].is_object()) return false;
    const auto &s = j["settings"];
    if (!s.contains("audioGuidance") || !s["audioGuidance"].is_string()) return false;
    *on = s["audioGuidance"].get<std::string>() == "on";
    return true;
}

std::string speak_payload(const std::string &text, const std::string &language, bool clear) {
    nlohmann::json j = {{"text", text}, {"clear", clear}};
    if (!language.empty()) j["language"] = language;
    return j.dump();
}

LunaReply speak(const std::string &text, const std::string &language, bool clear) {
    return luna_call_sync(kSpeak, speak_payload(text, language, clear), kTimeoutMs);
}

std::unique_ptr<LunaSubscription> subscribe_audio_guidance(std::function<void(bool)> on_change, std::string *error) {
    return luna_subscribe(
        kSettings, R"({"category":"option","keys":["audioGuidance"],"subscribe":true})",
        [on_change = std::move(on_change)](const std::string &reply) {
            bool on = false;
            if (parse_audio_guidance(reply, &on)) on_change(on);
        },
        true, error);
}

LunaReply launch_app(const std::string &app_id, const std::string &params_json) {
    return luna_call_sync(kLaunch, launch_payload(app_id, params_json), kTimeoutMs);
}

LunaReply open_url(const std::string &url) { return luna_call_sync(kLaunch, open_url_payload(url), kTimeoutMs); }

LunaReply system_toast(const std::string &source_id, const std::string &message) {
    return luna_call_sync(kToast, toast_payload(source_id, message), kTimeoutMs);
}

std::unique_ptr<LunaSubscription> subscribe_network_status(std::function<void(const NetworkStatus &)> on_change,
                                                           std::string *error) {
    return luna_subscribe(
        kStatus, R"({"subscribe":true})",
        [on_change = std::move(on_change)](const std::string &reply) {
            NetworkStatus s;
            if (parse_connection_status(reply, &s)) on_change(s);
        },
        true, error);
}

}  // namespace wnk::platform
