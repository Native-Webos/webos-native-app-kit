// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/diagnostics/capabilities.hpp"

#include <string>

using namespace wnk::diagnostics;

namespace {
// Shapes as read from an OLED65C1PTB (webOS 6.5.3); identifier values replaced with dummies.
const char *kOsInfo = R"({ "core_os_kernel_version": "4.4.84-229.kcl4tv.6", "core_os_release": "6.5.3-47",
  "webos_manufacturing_version": "03.53.45", "webos_name": "webOS TV", "webos_release": "6.5.3",
  "webos_release_codename": "kisscurl-koli" })";
const char *kDeviceInfo = R"({ "board_type": "O20_DVB", "device_name": "o20n", "hardware_id": "HE_DTV_W21O_AFABATAA",
  "nduid": "aaaabbbbccccddddeeeeffff0000111122223333", "product_id": "OLED65C1PTB", "ram_size": "3 GB",
  "serial_number": "SERIAL0000XYZ", "wifi_addr": "02:00:00:00:00:01", "wired_addr": "02:00:00:00:00:02" })";
}  // namespace

TEST("device info: descriptive nyx fields are read, identifiers never are") {
    DeviceInfo d = DeviceInfo::from_nyx(kOsInfo, kDeviceInfo);
    d.platform = "webos";
    CHECK_EQ(d.webos_release, std::string("6.5.3"));
    CHECK_EQ(d.webos_codename, std::string("kisscurl-koli"));
    CHECK_EQ(d.firmware, std::string("03.53.45"));
    CHECK_EQ(d.model, std::string("OLED65C1PTB"));
    CHECK_EQ(d.board, std::string("O20_DVB"));
    CHECK_EQ(d.ram, std::string("3 GB"));
    const std::string report = capability_report_json(d, Capabilities{}) + d.summary();
    for (const char *secret : {"SERIAL0000XYZ", "aaaabbbb", "02:00:00:00:00:01", "02:00:00:00:00:02"}) {
        CHECK(report.find(secret) == std::string::npos);
    }
    CHECK(d.summary().find("OLED65C1PTB webOS 6.5.3") != std::string::npos);
}

TEST("device info: missing or broken nyx files leave fields empty") {
    const DeviceInfo d = DeviceInfo::from_nyx("", "{not json");
    CHECK(d.webos_release.empty());
    CHECK(d.model.empty());
    const DeviceInfo host = DeviceInfo::detect(false);
    CHECK_EQ(host.platform, std::string("host"));
    CHECK(!host.kernel.empty());
}

TEST("capabilities: unknown until set, updates report changes, report lists all in order") {
    Capabilities c;
    CHECK(c.support("input.pointer") == Support::Unknown);
    CHECK(c.set("display.gles2", Support::Supported, "Mali-G51"));
    CHECK(c.set("input.pointer", Support::Unknown, "no pointer event yet"));
    CHECK(!c.set("input.pointer", Support::Unknown, "still none"));
    CHECK(c.set("input.pointer", Support::Supported, "motion seen"));
    CHECK(c.support("input.pointer") == Support::Supported);
    CHECK_EQ(c.all().size(), size_t{2});
    const std::string j = capability_report_json(DeviceInfo{}, c);
    const size_t a = j.find("display.gles2"), b = j.find("input.pointer");
    CHECK(a != std::string::npos && b != std::string::npos && a < b);
    CHECK(j.find("\"support\": \"supported\"") != std::string::npos);
    CHECK(j.find("motion seen") != std::string::npos);
}
