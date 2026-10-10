// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/platform/probe.hpp"

#include "wnk/platform/process.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace wnk::platform {
namespace {

std::string env_or(const char *name, const char *fallback) {
    const char *v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string(fallback);
}

// Read up to `limit` bytes of a small text file. Returns "<unreadable: reason>" on failure.
std::string read_small(const char *path, size_t limit = 512) {
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::string("<unreadable: ") + std::strerror(errno) + ">";
    std::string out(limit, '\0');
    ssize_t n = ::read(fd, out.data(), limit);
    ::close(fd);
    if (n < 0) return std::string("<unreadable: ") + std::strerror(errno) + ">";
    out.resize(static_cast<size_t>(n));
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    for (char &c : out) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    return out;
}

std::string groups_text() {
    gid_t groups[64];
    int n = getgroups(64, groups);
    if (n < 0) return std::string("<error: ") + std::strerror(errno) + ">";
    std::string out;
    for (int i = 0; i < n; ++i) {
        if (i) out.push_back(',');
        out += std::to_string(groups[i]);
    }
    return out;
}

}  // namespace

Facts collect_system_facts() {
    Facts f;
    f.emplace_back("uid", std::to_string(getuid()));
    f.emplace_back("euid", std::to_string(geteuid()));
    f.emplace_back("gid", std::to_string(getgid()));
    f.emplace_back("groups", groups_text());
    f.emplace_back("HOME", env_or("HOME", "<unset>"));
    f.emplace_back("APPID", env_or("APPID", "<unset>"));
    f.emplace_back("LD_LIBRARY_PATH", env_or("LD_LIBRARY_PATH", "<unset>"));
    char cwd[4096];
    f.emplace_back("cwd", getcwd(cwd, sizeof cwd) != nullptr ? std::string(cwd) : std::string("<error>"));
    f.emplace_back("XDG_RUNTIME_DIR", env_or("XDG_RUNTIME_DIR", "<unset>"));
    f.emplace_back("EGL_PLATFORM", env_or("EGL_PLATFORM", "<unset>"));
    f.emplace_back("exe", executable_path());
    utsname u{};
    if (uname(&u) == 0) {
        f.emplace_back("kernel", std::string(u.release) + " " + u.machine);
    }
    if constexpr (kIsWebOS) {
        f.emplace_back("starfish-release", read_small("/etc/starfish-release"));
        f.emplace_back("machineName", read_small("/etc/prefs/properties/machineName"));
        f.emplace_back("nyx os_info", read_small("/var/run/nyx/os_info.json"));
        f.emplace_back("nyx device_info", read_small("/var/run/nyx/device_info.json"));
    } else {
        f.emplace_back("os-release", read_small("/etc/os-release", 256));
    }
    return f;
}

const char *durability_name(Durability d) {
    switch (d) {
        case Durability::Durable: return "durable";
        case Durability::Volatile: return "volatile";
        case Durability::Unknown: return "unknown";
    }
    return "?";
}

std::vector<PathCandidate> storage_candidates(const std::string &app_id, const std::string &app_dir,
                                              const std::string &runtime_dir) {
    std::vector<PathCandidate> c;
    const std::string probe = "." + app_id + ".wnk-probe";
    if constexpr (kIsWebOS) {
        c.push_back({"data", "/media/developer", probe, Durability::Durable,
                     "Dev Mode jail: rw; survives reinstall (plx paths.rs)"});
        c.push_back({"data", "/media/internal", probe, Durability::Durable,
                     "Homebrew jail: rw; Dev Mode jail: ro (plx paths.rs)"});
        c.push_back({"data", app_dir, probe, Durability::Unknown,
                     "replaced on every (re)install (plx paths.rs); moonlight writes $HOME/conf"});
        const char *home = std::getenv("HOME");
        if (home != nullptr && home[0] == '/' && app_dir != home) {
            c.push_back({"data", home, probe, Durability::Unknown, "$HOME (moonlight-tv path_webos.c)"});
        }
    } else {
        const char *xdg = std::getenv("XDG_DATA_HOME");
        const char *home = std::getenv("HOME");
        std::string data_home = (xdg != nullptr && xdg[0] == '/') ? std::string(xdg)
                                : (home != nullptr ? std::string(home) + "/.local/share" : std::string());
        if (!data_home.empty()) {
            c.push_back({"data", data_home, probe, Durability::Durable, "XDG data home"});
        }
    }
    if (!runtime_dir.empty()) {
        c.push_back({"runtime", runtime_dir, probe, Durability::Volatile, "cleared on reboot"});
    }
    return c;
}

PathProbeResult probe_writable(const PathCandidate &candidate) {
    PathProbeResult r;
    r.candidate = candidate;
    if (candidate.directory.empty()) {
        r.detail = "no directory";
        return r;
    }
    const std::string path = candidate.directory + "/" + candidate.file_name;
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        r.detail = std::strerror(errno);
        return r;
    }
    static const char kPayload[] = "wnk-probe\n";
    bool ok = ::write(fd, kPayload, sizeof kPayload - 1) == static_cast<ssize_t>(sizeof kPayload - 1);
    ok = ok && ::fsync(fd) == 0;
    ::close(fd);
    if (ok) {
        char back[sizeof kPayload] = {};
        int rfd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        ok = rfd >= 0 && ::read(rfd, back, sizeof back - 1) == static_cast<ssize_t>(sizeof kPayload - 1) &&
             std::memcmp(back, kPayload, sizeof kPayload - 1) == 0;
        if (rfd >= 0) ::close(rfd);
    }
    const int unlink_rc = ::unlink(path.c_str());
    r.writable = ok;
    r.detail = ok ? (unlink_rc == 0 ? "ok" : "ok (probe file could not be removed)") : "write/readback failed";
    return r;
}

}  // namespace wnk::platform
