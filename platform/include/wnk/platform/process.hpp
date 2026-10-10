// SPDX-License-Identifier: GPL-3.0-or-later
//
// Process-level platform setup: environment, signals, launch parameters and the app's own paths.
// See docs/research/APPLICATION-LIFECYCLE.md and WEBOS-PLATFORM.md for the evidence behind each step.
#pragma once

#include <string>

namespace wnk::platform {

#if defined(WNK_PLATFORM_WEBOS)
inline constexpr bool kIsWebOS = true;
#else
inline constexpr bool kIsWebOS = false;
#endif

// Call first in main(), before SDL is initialised.
//  - webOS: set EGL_PLATFORM=wayland and XDG_RUNTIME_DIR=/tmp/xdg if unset. SAM launches native apps
//    without them, and SDL picks its EGL platform from the environment at video init
//    (moonlight-tv src/main.c, lgnome native_lifecycle.c, webosbrew samples sdl_shell.c).
//  - Ignore SIGPIPE so a dropped socket returns EPIPE instead of killing the process (lgnome).
// SIGINT/SIGTERM are left at SIG_DFL on purpose: SDL installs its quit handlers only over SIG_DFL
// and turns them into SDL_QUIT, which drives the orderly shutdown.
void prepare_process_environment();

struct LaunchInfo {
    std::string app_id;       // $APPID on webOS (set by the launcher), else the fallback passed in
    std::string params_json;  // argv[1] when it is a JSON object (SAM launch params), else empty
    std::string exe_path;     // resolved /proc/self/exe
    std::string app_dir;      // directory containing the executable
};

LaunchInfo read_launch_info(int argc, char **argv, const std::string &fallback_app_id);

// realpath of /proc/self/exe, or empty. Inside the webOS jail this resolves jail-relative, which is
// the path the app must use; $HOME is not reliable there (PlxNative rust-modules/base/src/paths.rs).
std::string executable_path();

std::string dirname_of(const std::string &path);

// Directory for logs and other volatile per-run files, created if missing (mode 0755) and checked
// to be a real directory owned by this uid:
//   webOS: /tmp/<app_id>     host: $XDG_RUNTIME_DIR/<app_id>, else /tmp/<app_id>-<uid>
// Returns empty if it cannot be created or fails the ownership check.
std::string runtime_dir(const std::string &app_id);

}  // namespace wnk::platform
