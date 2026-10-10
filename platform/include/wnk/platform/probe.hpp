// SPDX-License-Identifier: GPL-3.0-or-later
//
// Device/system probe: collects facts that the research could not settle offline (writable paths,
// release, SoC, user ids, environment) so a single launch on a TV produces evidence for
// docs/research/COMPATIBILITY.md. Everything here is read-only except probe_writable(), which
// creates and immediately removes one small file.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace wnk::platform {

using Facts = std::vector<std::pair<std::string, std::string>>;

// Process and OS facts: uid/gid/euid/groups, HOME, APPID, LD_LIBRARY_PATH, cwd, exe path,
// /etc/starfish-release, /etc/prefs/properties/machineName, /var/run/nyx/os_info.json and
// device_info.json (first 512 bytes each, if readable), kernel release.
Facts collect_system_facts();

enum class Durability { Durable, Volatile, Unknown };
const char *durability_name(Durability d);

struct PathCandidate {
    std::string purpose;    // e.g. "data", "runtime"
    std::string directory;  // where a file would be written
    std::string file_name;  // probe file name inside it
    Durability durability;  // what the research says about this location
    std::string evidence;   // short source note
};

struct PathProbeResult {
    PathCandidate candidate;
    bool writable = false;
    std::string detail;  // errno text or "ok"
};

// Candidate locations for persistent data, best first (PlxNative paths.rs session_candidates,
// moonlight-tv path_webos.c). On the host: $XDG_DATA_HOME/<id> and the runtime dir.
std::vector<PathCandidate> storage_candidates(const std::string &app_id, const std::string &app_dir,
                                              const std::string &runtime_dir);

// Create <dir>/<file_name> with O_CREAT|O_EXCL|O_NOFOLLOW, write, fsync, read back, then unlink it.
// Never creates directories.
PathProbeResult probe_writable(const PathCandidate &candidate);

}  // namespace wnk::platform
