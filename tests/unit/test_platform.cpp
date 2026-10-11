// SPDX-License-Identifier: GPL-3.0-or-later
#define WNK_TEST_MAIN
#include "wnk_test.hpp"

#include "wnk/platform/probe.hpp"
#include "wnk/platform/process.hpp"

#include <cstdlib>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

using namespace wnk::platform;

namespace {

std::string temp_dir() {
    char tmpl[] = "/tmp/wnk-test-XXXXXX";
    const char *d = mkdtemp(tmpl);
    return d != nullptr ? std::string(d) : std::string();
}

}  // namespace

TEST("launch info takes JSON params from argv[1] only") {
    unsetenv("APPID");
    char a0[] = "app", a1[] = R"( {"contentTarget":"x"})", a2[] = "--flag";
    char *with_json[] = {a0, a1, nullptr};
    auto info = read_launch_info(2, with_json, "org.example.fallback");
    CHECK_EQ(info.app_id, std::string("org.example.fallback"));
    CHECK_EQ(info.params_json, std::string(a1));
    char *with_flag[] = {a0, a2, nullptr};
    CHECK(read_launch_info(2, with_flag, "x.y").params_json.empty());
    char *none[] = {a0, nullptr};
    CHECK(read_launch_info(1, none, "x.y").params_json.empty());
}

TEST("launch info prefers $APPID") {
    setenv("APPID", "com.example.real", 1);
    char a0[] = "app";
    char *argv[] = {a0, nullptr};
    CHECK_EQ(read_launch_info(1, argv, "fallback.id").app_id, std::string("com.example.real"));
    unsetenv("APPID");
}

TEST("executable path and dirname") {
    const std::string exe = executable_path();
    CHECK(!exe.empty());
    CHECK(exe[0] == '/');
    CHECK_EQ(dirname_of("/a/b/c"), std::string("/a/b"));
    CHECK_EQ(dirname_of("/c"), std::string("/"));
    CHECK_EQ(dirname_of("c"), std::string("."));
}

TEST("runtime_dir is created, owned and rejects unsafe ids") {
    const std::string base = temp_dir();
    REQUIRE(!base.empty());
    setenv("XDG_RUNTIME_DIR", base.c_str(), 1);
    const std::string dir = runtime_dir("org.example.test");
    CHECK_EQ(dir, base + "/org.example.test");
    struct stat st{};
    REQUIRE(lstat(dir.c_str(), &st) == 0);
    CHECK(S_ISDIR(st.st_mode));
    CHECK(runtime_dir("").empty());
    CHECK(runtime_dir("../evil").empty());
    CHECK(runtime_dir(".hidden").empty());
    // A symlink planted at the expected name must be refused.
    REQUIRE(::symlink("/tmp", (base + "/org.example.link").c_str()) == 0);
    CHECK(runtime_dir("org.example.link").empty());
    ::unlink((base + "/org.example.link").c_str());
    ::rmdir(dir.c_str());
    ::rmdir(base.c_str());
}

TEST("probe_writable writes, reads back and removes its file") {
    const std::string dir = temp_dir();
    REQUIRE(!dir.empty());
    PathCandidate c{"data", dir, ".probe", Durability::Volatile, "test"};
    auto r = probe_writable(c);
    CHECK(r.writable);
    CHECK_EQ(r.detail, std::string("ok"));
    CHECK(access((dir + "/.probe").c_str(), F_OK) != 0);
    PathCandidate missing{"data", dir + "/does-not-exist", ".probe", Durability::Unknown, "test"};
    CHECK(!probe_writable(missing).writable);
    ::rmdir(dir.c_str());
}

TEST("probe_writable reports read-only directories") {
    if (geteuid() == 0) return;  // root ignores directory permissions
    const std::string dir = temp_dir();
    REQUIRE(!dir.empty());
    REQUIRE(chmod(dir.c_str(), 0500) == 0);
    PathCandidate c{"data", dir, ".probe", Durability::Unknown, "test"};
    auto r = probe_writable(c);
    CHECK(!r.writable);
    chmod(dir.c_str(), 0700);
    ::rmdir(dir.c_str());
}

TEST("system facts include identity and environment keys") {
    auto facts = collect_system_facts();
    auto has = [&](const char *key) {
        for (const auto &kv : facts) {
            if (kv.first == key) return true;
        }
        return false;
    };
    CHECK(has("uid"));
    CHECK(has("exe"));
    CHECK(has("LD_LIBRARY_PATH"));
    CHECK(has(kIsWebOS ? "starfish-release" : "os-release"));
}

TEST("storage candidates end with the volatile runtime dir") {
    auto c = storage_candidates("org.example.app", "/opt/app", "/tmp/rt");
    REQUIRE(!c.empty());
    CHECK_EQ(c.back().directory, std::string("/tmp/rt"));
    CHECK(c.back().durability == Durability::Volatile);
}

TEST("probe: identifiers in nyx device_info are redacted, other fields kept") {
    const std::string in =
        "{ \"board_type\": \"O20_DVB\", \"nduid\": \"0f37f0840956\", \"product_id\": \"OLED65C1PTB\",\n"
        "  \"serial_number\" : \"111INPT7A032\", \"wifi_addr\":\"4C:BC:E9:00:00:0E\", \"wired_addr\": \"B0:37:95:00:00:7D\" }";
    const std::string out = wnk::platform::redact_identifiers(in);
    CHECK(out.find("0f37f0840956") == std::string::npos);
    CHECK(out.find("111INPT7A032") == std::string::npos);
    CHECK(out.find("4C:BC") == std::string::npos);
    CHECK(out.find("B0:37") == std::string::npos);
    CHECK(out.find("\"product_id\": \"OLED65C1PTB\"") != std::string::npos);
    CHECK(out.find("\"serial_number\" : \"<redacted>\"") != std::string::npos);
    // A read cut off inside a value (read_small truncates) still hides it.
    CHECK(wnk::platform::redact_identifiers("{\"nduid\": \"0f37f08").find("0f37") == std::string::npos);
}
