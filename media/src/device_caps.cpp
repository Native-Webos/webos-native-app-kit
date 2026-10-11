// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/device_caps.hpp"

#include "wnk/platform/luna.hpp"

#include <nlohmann/json.hpp>

namespace wnk::media {
namespace {

constexpr const char *kKeys[] = {"tv.conti.supportHevc",          "tv.hw.panelResolution",     "tv.hw.panelOutputFrameRate",
                                 "tv.model.supportHDR",           "tv.config.supportDolbyHDRContents",
                                 "tv.config.supportDolbyTVATMOS", "tv.hw.SoCChipType"};

std::optional<bool> boolean(const nlohmann::json &c, const char *key) {
    const auto it = c.find(key);
    if (it == c.end() || !it->is_boolean()) return std::nullopt;
    return it->get<bool>();
}

std::string yes_no(const std::optional<bool> &v) { return !v ? "?" : *v ? "yes" : "no"; }

}  // namespace

std::string DeviceMediaCaps::request() {
    nlohmann::json names = nlohmann::json::array();
    for (const char *k : kKeys) names.push_back(k);
    return nlohmann::json{{"configNames", names}}.dump();
}

DeviceMediaCaps DeviceMediaCaps::parse(const std::string &reply) {
    DeviceMediaCaps d;
    const auto j = nlohmann::json::parse(reply, nullptr, false);
    if (!j.is_object() || !j.contains("configs") || !j["configs"].is_object()) {
        d.detail = "not a getConfigs reply";
        return d;
    }
    const auto &c = j["configs"];
    d.measured = true;
    d.hevc = boolean(c, "tv.conti.supportHevc");
    d.hdr = boolean(c, "tv.model.supportHDR");
    d.dolby_vision = boolean(c, "tv.config.supportDolbyHDRContents");
    d.dolby_atmos = boolean(c, "tv.config.supportDolbyTVATMOS");
    if (const auto it = c.find("tv.hw.panelResolution"); it != c.end() && it->is_string()) {
        const std::string r = it->get<std::string>();
        d.uhd_panel = r == "UD" || r == "8K";  // "FHD"/"HD" otherwise
    }
    if (const auto it = c.find("tv.hw.panelOutputFrameRate"); it != c.end() && it->is_string()) {
        try {
            const int hz = std::stoi(it->get<std::string>());
            if (hz > 0) d.panel_hz = hz;
        } catch (...) {
        }
    }
    if (const auto it = c.find("tv.hw.SoCChipType"); it != c.end() && it->is_string()) d.soc = it->get<std::string>();
    return d;
}

DeviceMediaCaps DeviceMediaCaps::detect(int timeout_ms) {
    const auto r = platform::luna_call_sync("luna://com.webos.service.config/getConfigs", request(), timeout_ms);
    if (r.status != platform::LunaStatus::Ok) {
        DeviceMediaCaps d;
        d.detail = std::string("getConfigs: ") + platform::luna_status_name(r.status) + (r.detail.empty() ? "" : " " + r.detail);
        return d;
    }
    return parse(r.payload);
}

std::string DeviceMediaCaps::summary() const {
    if (!measured) return "not measured (" + detail + ")";
    return "hevc " + yes_no(hevc) + ", uhd " + yes_no(uhd_panel) + ", " +
           (panel_hz ? std::to_string(*panel_hz) + " Hz" : std::string("? Hz")) + ", hdr " + yes_no(hdr) + ", dv " +
           yes_no(dolby_vision) + ", atmos " + yes_no(dolby_atmos) + ", soc " + (soc.empty() ? "?" : soc);
}

}  // namespace wnk::media
