// SPDX-License-Identifier: GPL-3.0-or-later
//
// Crash record for fatal signals (docs/research/DEBUGGING-AND-TESTING.md, "Crash diagnostics").
//
// On SIGSEGV, SIGBUS, SIGILL, SIGFPE or SIGABRT the handler runs on its own stack, writes a plain-text
// record to a file and re-raises the signal with the default action, so the process still dies with
// the real signal. The record holds: signal, si_code and fault address; app id, version and build id;
// the lifecycle state; uptime; pc/lr/sp from the signal context; a backtrace; and the executable
// mappings from /proc/self/maps, which with the build id is enough to symbolise offline
// (addr2line -e <unstripped binary> <pc - mapping start>).
//
// The handler uses only async-signal-safe calls (open, write, read, close, clock_gettime, raise), plus
// backtrace()/backtrace_symbols_fd(), which glibc makes safe enough once backtrace() has been called
// outside the handler (it loads libgcc_s on first use); install_crash_handler() does that. All
// strings it prints are copied into static buffers at install time. libgcc_s.so.1 and both backtrace
// symbols exist on every inventoried firmware 1.2-11.2 (tools/fw-lib-matrix.py).
#pragma once

#include <string>

namespace wnk::diagnostics {

struct CrashConfig {
    std::string path;      // record file, created only when a crash happens (0640, O_NOFOLLOW)
    std::string app_id;
    std::string version;
    std::string build_id;  // own_build_id() if empty
};

// Installs the handlers (once per process; later calls update the strings). Returns false with
// `error` set if the alternate stack or a handler could not be installed.
bool install_crash_handler(const CrashConfig &config, std::string *error = nullptr);

// Text written as "state=" in the record. Must point to storage that lives forever (a string
// literal, e.g. wnk::app::state_name()). Safe to call from the main thread at any time.
void set_crash_state(const char *state);

struct PreviousCrash {
    bool found = false;
    std::string text;     // the whole record
    std::string summary;  // its "signal=" line
};
// Reads a record left by an earlier run and renames it to <path>.prev, so it is reported once.
PreviousCrash take_previous_crash(const std::string &path);

// Hex GNU build id of the main executable (NT_GNU_BUILD_ID), or empty if it has none.
std::string own_build_id();

}  // namespace wnk::diagnostics
