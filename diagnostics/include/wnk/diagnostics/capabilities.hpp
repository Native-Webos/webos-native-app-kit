// SPDX-License-Identifier: GPL-3.0-or-later
//
// What this device can do, as the running app found out, and what device it is.
//
// A capability is Supported, Unsupported or Unknown, never assumed: Unknown is the honest answer
// until something at runtime proves it either way (the GL context exists, the store opened, a
// pointer event arrived). The app shell fills the registry at startup and updates it as evidence
// arrives; the report is logged and written to <runtime dir>/capabilities.json for pulling with
// tools/logs.sh-style tools. Measured results across devices live in docs/compatibility/records.json.
//
// DeviceInfo copies only descriptive fields from nyx (release, firmware, model, board). Serial
// number, nduid and network addresses are identifiers and are never read into it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wnk::diagnostics {

enum class Support : uint8_t { Supported, Unsupported, Unknown };
const char *support_name(Support s);

struct Capability {
    std::string id;      // dotted, e.g. "display.gles2", "input.pointer"
    Support support = Support::Unknown;
    std::string detail;  // what was seen: versions, sizes, error text
    std::string source;  // "runtime" (probed now) or "not-implemented" and the like
};

class Capabilities {
 public:
    // Adds or replaces. Returns true if the support value changed (for "first seen" logging).
    bool set(const std::string &id, Support s, std::string detail, std::string source = "runtime");
    const Capability *get(const std::string &id) const;
    Support support(const std::string &id) const;  // Unknown if never set
    const std::vector<Capability> &all() const { return items_; }

 private:
    std::vector<Capability> items_;  // insertion order, for stable reports
};

struct DeviceInfo {
    std::string platform;        // "webos" or "host"
    std::string webos_release;   // e.g. "6.5.3"
    std::string webos_codename;  // e.g. "kisscurl-koli"
    std::string firmware;        // manufacturing version, e.g. "03.53.45"
    std::string model;           // product id, e.g. "OLED65C1PTB"
    std::string board;           // e.g. "O20_DVB"
    std::string device_name;     // machine name, e.g. "o20n"
    std::string ram;             // e.g. "3 GB"
    std::string kernel;          // uname release + machine

    // Reads /var/run/nyx/os_info.json and device_info.json when present, plus uname.
    static DeviceInfo detect(bool webos);
    // Parses nyx JSON texts (either may be empty or invalid; fields stay empty then).
    static DeviceInfo from_nyx(const std::string &os_info_json, const std::string &device_info_json);
    std::string summary() const;  // one line for the log
};

// {"device": {...}, "capabilities": [{"id", "support", "detail", "source"}, ...]}
std::string capability_report_json(const DeviceInfo &device, const Capabilities &caps);

}  // namespace wnk::diagnostics
