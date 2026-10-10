// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/platform/process.hpp"

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>

#include <sys/stat.h>
#include <unistd.h>

namespace wnk::platform {

void prepare_process_environment() {
    if constexpr (kIsWebOS) {
        setenv("EGL_PLATFORM", "wayland", 0);
        setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 0);
    }
    std::signal(SIGPIPE, SIG_IGN);
}

std::string executable_path() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return std::string(buf, static_cast<size_t>(n));
}

std::string dirname_of(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

LaunchInfo read_launch_info(int argc, char **argv, const std::string &fallback_app_id) {
    LaunchInfo info;
    const char *env_id = std::getenv("APPID");
    info.app_id = (env_id != nullptr && env_id[0] != '\0') ? env_id : fallback_app_id;
    if (argc > 1 && argv[1] != nullptr) {
        const char *p = argv[1];
        while (*p == ' ' || *p == '\t' || *p == '\n') ++p;
        if (*p == '{') info.params_json = argv[1];
    }
    info.exe_path = executable_path();
    info.app_dir = info.exe_path.empty() ? std::string() : dirname_of(info.exe_path);
    return info;
}

namespace {

bool ensure_owned_dir(const std::string &dir) {
    if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return false;
    struct stat st{};
    if (lstat(dir.c_str(), &st) != 0) return false;
    // lstat: a symlink planted at a predictable /tmp name is rejected by S_ISDIR.
    return S_ISDIR(st.st_mode) && st.st_uid == geteuid();
}

}  // namespace

std::string runtime_dir(const std::string &app_id) {
    if (app_id.empty() || app_id.find('/') != std::string::npos || app_id[0] == '.') return {};
    std::string dir;
    if constexpr (kIsWebOS) {
        dir = "/tmp/" + app_id;
    } else {
        const char *xdg = std::getenv("XDG_RUNTIME_DIR");
        if (xdg != nullptr && xdg[0] == '/') {
            dir = std::string(xdg) + "/" + app_id;
        } else {
            dir = "/tmp/" + app_id + "-" + std::to_string(geteuid());
        }
    }
    return ensure_owned_dir(dir) ? dir : std::string();
}

}  // namespace wnk::platform
