// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/diagnostics/capabilities.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

#include <sys/utsname.h>

namespace wnk::diagnostics {
namespace {

std::string read_file(const std::string &path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string str(const nlohmann::json &j, const char *key) {
    return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

}  // namespace

const char *support_name(Support s) {
    switch (s) {
        case Support::Supported: return "supported";
        case Support::Unsupported: return "unsupported";
        case Support::Unknown: return "unknown";
    }
    return "?";
}

bool Capabilities::set(const std::string &id, Support s, std::string detail, std::string source) {
    auto it = std::find_if(items_.begin(), items_.end(), [&id](const Capability &c) { return c.id == id; });
    if (it == items_.end()) {
        items_.push_back({id, s, std::move(detail), std::move(source)});
        return true;
    }
    const bool changed = it->support != s;
    it->support = s;
    it->detail = std::move(detail);
    it->source = std::move(source);
    return changed;
}

const Capability *Capabilities::get(const std::string &id) const {
    auto it = std::find_if(items_.begin(), items_.end(), [&id](const Capability &c) { return c.id == id; });
    return it == items_.end() ? nullptr : &*it;
}

Support Capabilities::support(const std::string &id) const {
    const Capability *c = get(id);
    return c != nullptr ? c->support : Support::Unknown;
}

DeviceInfo DeviceInfo::from_nyx(const std::string &os_info_json, const std::string &device_info_json) {
    DeviceInfo d;
    const auto os = nlohmann::json::parse(os_info_json, nullptr, false);
    const auto dev = nlohmann::json::parse(device_info_json, nullptr, false);
    // Allow-list only: identifiers in device_info (serial_number, nduid, wifi_addr, wired_addr) are
    // never copied.
    d.webos_release = str(os, "webos_release");
    d.webos_codename = str(os, "webos_release_codename");
    d.firmware = str(os, "webos_manufacturing_version");
    d.model = str(dev, "product_id");
    d.board = str(dev, "board_type");
    d.device_name = str(dev, "device_name");
    d.ram = str(dev, "ram_size");
    return d;
}

DeviceInfo DeviceInfo::detect(bool webos) {
    DeviceInfo d = from_nyx(read_file("/var/run/nyx/os_info.json"), read_file("/var/run/nyx/device_info.json"));
    d.platform = webos ? "webos" : "host";
    utsname u{};
    if (uname(&u) == 0) d.kernel = std::string(u.release) + " " + u.machine;
    return d;
}

std::string DeviceInfo::summary() const {
    std::string s = platform;
    if (!model.empty()) s += " " + model;
    if (!webos_release.empty()) s += " webOS " + webos_release;
    if (!webos_codename.empty()) s += " (" + webos_codename + ")";
    if (!firmware.empty()) s += " firmware " + firmware;
    if (!board.empty()) s += " board " + board;
    if (!ram.empty()) s += " ram " + ram;
    if (!kernel.empty()) s += " kernel " + kernel;
    return s;
}

std::string capability_report_json(const DeviceInfo &d, const Capabilities &caps) {
    nlohmann::json j;
    j["device"] = {{"platform", d.platform}, {"webos_release", d.webos_release}, {"webos_codename", d.webos_codename},
                   {"firmware", d.firmware},  {"model", d.model},                 {"board", d.board},
                   {"device_name", d.device_name}, {"ram", d.ram},                {"kernel", d.kernel}};
    j["capabilities"] = nlohmann::json::array();
    for (const auto &c : caps.all()) {
        j["capabilities"].push_back(
            {{"id", c.id}, {"support", support_name(c.support)}, {"detail", c.detail}, {"source", c.source}});
    }
    return j.dump(2) + "\n";
}

}  // namespace wnk::diagnostics
