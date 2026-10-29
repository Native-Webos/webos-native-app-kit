#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Create a new app in its own folder, outside the kit, that uses the kit by reference.

    tools/new-app.py DIR APP_ID --title "My App" [--vendor NAME] [--color '#1f2937']

DIR becomes a standalone CMake project with its own build directory: main.cpp (one screen with a
button), CMakeLists.txt (the ipk and a host smoke test, through cmake/WnkApp.cmake), CMakePresets.json,
build.sh, placeholder icons, the reviewed DT_NEEDED list, a README and a .gitignore. It finds the
kit through WNK_KIT_DIR (CMake cache or environment), else the path from DIR to this kit recorded now.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

KIT = Path(__file__).resolve().parent.parent

# LG's rules for an app id, and the prefixes LG keeps for its own apps.
ID_RE = re.compile(r"^[a-z0-9]+(\.[a-z0-9][a-z0-9+-]*)+$")
RESERVED = ("com.lge.", "com.webos.", "com.palm.", "com.lg.")

CMAKE = """# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.22)
project({name} LANGUAGES C CXX)

# The kit: -DWNK_KIT_DIR=..., else $WNK_KIT_DIR, else where it was when tools/new-app.py made this.
# A relative path is taken from this folder (a git submodule at kit/ would be "kit").
if (DEFINED ENV{{WNK_KIT_DIR}})
    set(_kit_default "$ENV{{WNK_KIT_DIR}}")
else ()
    set(_kit_default "{kit_rel}")
endif ()
set(WNK_KIT_DIR "${{_kit_default}}" CACHE PATH "The webOS native app kit (absolute, or relative to this folder)")
get_filename_component(_kit "${{WNK_KIT_DIR}}" ABSOLUTE BASE_DIR "${{CMAKE_CURRENT_SOURCE_DIR}}")
if (NOT EXISTS "${{_kit}}/cmake/WnkApp.cmake")
    message(FATAL_ERROR "The webOS native app kit is not at ${{_kit}}: set WNK_KIT_DIR")
endif ()
# set(WNK_FFMPEG ON)  # media playback: see the kit's docs/AGENT-APP-GUIDE.md, "Media playback"
include("${{_kit}}/cmake/WnkApp.cmake")

add_executable({target} main.cpp)
target_link_libraries({target} PRIVATE wnk_app)
target_include_directories({target} SYSTEM PRIVATE "${{WNK_KIT_DIR}}/third_party")  # nlohmann/json.hpp

# Fonts (Atkinson Hyperlegible, OFL) live in fonts/ next to the executable, on the host and in the ipk.
add_custom_command(TARGET {target} POST_BUILD
        COMMAND "${{CMAKE_COMMAND}}" -E copy_directory "${{WNK_FONT_DIR}}" "$<TARGET_FILE_DIR:{target}>/fonts"
        VERBATIM)

wnk_add_webos_package(
        TARGET {target}
        APPID {app_id}
        TITLE "{title}"
        VERSION 0.1.0
        VENDOR "{vendor}"
        ICON "${{CMAKE_CURRENT_SOURCE_DIR}}/icon.png"
        LARGE_ICON "${{CMAKE_CURRENT_SOURCE_DIR}}/largeIcon.png"
        ICON_COLOR "{color}"
        EXPECTED_NEEDED "${{CMAKE_CURRENT_SOURCE_DIR}}/expected-needed.txt"
        EXTRA_DIRS "${{WNK_FONT_DIR}}")

# Host smoke test: OK on the focused button, then Back twice at the root quits.
wnk_add_app_test(NAME e2e.{name}.starts_and_quits TARGET {target} PREFIX {prefix}
        KEYS "Return,Escape,Escape" PASS "hello pressed 1")
"""

MAIN = """// SPDX-License-Identifier: GPL-3.0-or-later
//
// {title}: made with the kit's tools/new-app.py. The kit's docs/AGENT-APP-GUIDE.md maps features
// onto its API (lists and grids, HTTP, storage, playback); its applications/examples/ shows each of
// them in a working app.
#include "wnk/app/app.hpp"
#include "wnk/log.hpp"
#include "wnk/ui/navigation.hpp"
#include "wnk/ui/widgets.hpp"

#include <memory>
#include <string>

namespace {{

constexpr const char *kCat = "{name}";

class HomeScreen : public wnk::ui::Screen {{
 public:
    HomeScreen() : Screen("home") {{
        auto &col = add<wnk::ui::Column>(32);
        col.set_padding(wnk::ui::Insets::all(96));
        col.add<wnk::ui::Label>("{title}", wnk::ui::TextRole::Title);
        auto &status = col.add<wnk::ui::Label>("Press OK on a button. Back twice quits.");
        status.set_muted(true);
        auto &buttons = col.add<wnk::ui::Row>(24);
        buttons.add<wnk::ui::Button>("Say hello", [this, &status](wnk::ui::Ui &) {{
            ++presses_;
            WNK_LOGI(kCat, "hello pressed %d", presses_);
            status.set_text("Hello from the TV! Pressed " + std::to_string(presses_) + (presses_ == 1 ? " time." : " times."));
        }});
        buttons.add<wnk::ui::Button>("Show a toast", [](wnk::ui::Ui &ui) {{ ui.toast("A toast from {title}"); }});
    }}

 private:
    int presses_ = 0;
}};

class App : public wnk::app::Delegate {{
 public:
    void on_start(wnk::app::Context &ctx) override {{
        auto &ui = ctx.ui();
        nav_ = &ui.root().add<wnk::ui::Navigator>();
        nav_->push(ui, std::make_unique<HomeScreen>());
        WNK_LOGI(kCat, "started");
    }}

 private:
    wnk::ui::Navigator *nav_ = nullptr;
}};

}}  // namespace

int main(int argc, char **argv) {{
    wnk::app::AppConfig config;
    config.fallback_app_id = "{app_id}";  // used when $APPID is unset (host runs)
    config.title = "{title}";
    config.script_env_prefix = "{prefix}";  // {prefix}_KEYS=Return,Escape drives the host smoke test
    return wnk::app::run(argc, argv, config, [] {{ return std::make_unique<App>(); }});
}}
"""

BUILD_SH = """#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# {title}: build, test, package and install with the webOS native app kit.
#   ./build.sh            host build + smoke test (build/host-debug)
#   ./build.sh asan       the same with AddressSanitizer (build/host-asan)
#   ./build.sh run        run on the desktop (arrow keys, Enter, Escape)
#   ./build.sh webos      cross-build and package: build/webos-release/dist/*.ipk, package-checked
#   ./build.sh install    install the newest ipk on the TV (ARES_DEVICE, else ares' default device)
#   ./build.sh launch     start it on the TV
# The kit: $WNK_KIT_DIR, else {kit_rel} (relative to this folder).
set -euo pipefail
cd "$(dirname "${{BASH_SOURCE[0]}}")"
kit="${{WNK_KIT_DIR:-{kit_rel}}}"
[[ -f "$kit/tools/common.sh" ]] || {{ echo "error: the kit is not at $kit (set WNK_KIT_DIR)" >&2; exit 1; }}
source "$kit/tools/common.sh"
export WNK_KIT_DIR="$KIT_ROOT"
host() {{
    cmake --preset "$1"
    cmake --build --preset "$1" -j"$(nproc)"
    ctest --preset "$1"
}}
case "${{1:-host}}" in
    host) host host-debug ;;
    asan) host host-asan ;;
    run) cmake --build --preset host-debug -j"$(nproc)" && exec build/host-debug/{target} ;;
    webos)
        resolve_ndk
        cmake --preset webos-release
        cmake --build --preset webos-release -j"$(nproc)"
        cmake --build --preset webos-release --target wnk-ipks
        ls -l build/webos-release/dist/ ;;
    install)
        ipk="$(ls -t build/webos-release/dist/{app_id}_*_arm.ipk 2>/dev/null | head -n 1)"
        [[ -n "$ipk" ]] || die "no package yet: ./build.sh webos"
        exec "$KIT_ROOT/tools/install.sh" "$ipk" ;;
    launch) exec "$KIT_ROOT/tools/launch.sh" {app_id} ;;
    *) die "usage: ./build.sh [host|asan|run|webos|install|launch]" ;;
esac
"""

README = """# {title}

`{app_id}`, a native webOS TV app on the [webOS native app kit](https://github.com/Native-Webos/webos-native-app-kit),
made with its `tools/new-app.py`. The kit is not copied in: this folder points at it through
`WNK_KIT_DIR` (environment or `-DWNK_KIT_DIR=`), else `{kit_rel}`.

```sh
./build.sh            # host build and smoke test (e2e.{name}.starts_and_quits)
./build.sh run        # on the desktop: arrow keys, Enter, Escape
./build.sh webos      # build/webos-release/dist/{app_id}_0.1.0_arm.ipk, package-checked
ARES_DEVICE=tv ./build.sh install && ./build.sh launch
```

The first build of each configuration also builds the kit's libraries and dependencies (SDL on webOS,
libwebp, libass), into this folder's `build/`; later builds reuse them.

Next steps:

- Read the kit's `docs/AGENT-APP-GUIDE.md`: the rules, then the map from a feature to the kit's API.
- `icon.png` (80x80) and `largeIcon.png` (130x130) are placeholders; replace them with real artwork.
- Network access: `config.http = true`. Settings that survive a relaunch: `config.storage = true`
  and `REQUIRED_PERMISSIONS database.operation` in `wnk_add_webos_package`.
- Media playback: uncomment `set(WNK_FFMPEG ON)` in CMakeLists.txt and follow the guide's
  "Media playback" section (more libraries to link, `BUNDLED_LIBS`, the player's `expected-needed.txt`).
- If the executable's libraries change, the package check fails until `expected-needed.txt` is
  reviewed and updated.
- To keep the kit with the app, add it as a git submodule (`git submodule add <kit url> kit`) and
  set the default in CMakeLists.txt and build.sh to `kit`.
"""

PRESETS = {
    "version": 3,
    "cmakeMinimumRequired": {"major": 3, "minor": 22, "patch": 0},
    "configurePresets": [
        {"name": "host-debug", "displayName": "Host (debug, smoke test)", "generator": "Unix Makefiles",
         "binaryDir": "${sourceDir}/build/host-debug", "cacheVariables": {"CMAKE_BUILD_TYPE": "Debug"}},
        {"name": "host-asan", "displayName": "Host (ASan + UBSan)", "inherits": "host-debug",
         "binaryDir": "${sourceDir}/build/host-asan",
         "cacheVariables": {
             "CMAKE_CXX_FLAGS": "-fsanitize=address,undefined -fno-omit-frame-pointer",
             "CMAKE_C_FLAGS": "-fsanitize=address,undefined -fno-omit-frame-pointer",
             "CMAKE_EXE_LINKER_FLAGS": "-fsanitize=address,undefined"}},
        {"name": "webos-release", "displayName": "webOS TV (ARM, release)", "generator": "Unix Makefiles",
         "binaryDir": "${sourceDir}/build/webos-release",
         "toolchainFile": "$env{WEBOS_NDK}/share/buildroot/toolchainfile.cmake",
         "cacheVariables": {"CMAKE_BUILD_TYPE": "Release"}},
    ],
    "buildPresets": [{"name": n, "configurePreset": n} for n in ("host-debug", "host-asan", "webos-release")],
    "testPresets": [{"name": n, "configurePreset": n, "output": {"outputOnFailure": True}} for n in ("host-debug", "host-asan")],
}


def die(msg: str) -> None:
    print(f"new-app: {msg}", file=sys.stderr)
    sys.exit(2)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir", type=Path, help="the app's folder, outside the kit (created; must be empty)")
    ap.add_argument("app_id", help="lowercase reverse-DNS id, e.g. org.example.weather")
    ap.add_argument("--title", required=True, help="the name shown in the launcher")
    ap.add_argument("--vendor", default="Unknown vendor")
    ap.add_argument("--color", default="#1f2937", help="launcher tile colour behind the icon")
    a = ap.parse_args()

    if not ID_RE.match(a.app_id):
        die(f"'{a.app_id}' is not a lowercase reverse-DNS id (letters, digits, '.', '+', '-'; at least two parts)")
    if a.app_id.startswith(RESERVED):
        die(f"'{a.app_id}' uses a prefix LG keeps for its own apps")
    if not re.fullmatch(r"#[0-9a-fA-F]{6}", a.color):
        die(f"--color '{a.color}' is not #rrggbb")
    for field, value in (("title", a.title), ("vendor", a.vendor)):
        if not value.strip() or any(c in value for c in '"\\$;{}\n'):
            die(f"--{field} must be non-empty without quotes, backslashes, braces, '$', ';' or newlines")
    out = a.dir.expanduser().resolve()
    if out == KIT or KIT in out.parents:
        die(f"{out} is inside the kit; an app lives in its own folder")
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        die(f"{out} exists and is not an empty folder")

    name = re.sub(r"[^a-z0-9]+", "_", a.app_id.rsplit(".", 1)[-1]).strip("_") or "app"
    target = name.replace("_", "-")
    if target.startswith("wnk"):
        target = "app-" + target  # the kit's own targets are wnk_* / wnk-*
    prefix = "APP_" + name.upper()
    out.mkdir(parents=True, exist_ok=True)
    # Relative when the two sit near each other (they can move together), else absolute.
    kit_rel = os.path.relpath(KIT, out)
    if kit_rel.split(os.sep).count("..") > 2:
        kit_rel = str(KIT)
    fill = dict(app_id=a.app_id, title=a.title, vendor=a.vendor, color=a.color, name=name, target=target,
                prefix=prefix, kit_rel=kit_rel)
    (out / "CMakeLists.txt").write_text(CMAKE.format(**fill))
    (out / "main.cpp").write_text(MAIN.format(**fill))
    (out / "CMakePresets.json").write_text(json.dumps(PRESETS, indent=2) + "\n")
    (out / "build.sh").write_text(BUILD_SH.format(**fill))
    (out / "build.sh").chmod(0o755)
    (out / "README.md").write_text(README.format(**fill))
    (out / ".gitignore").write_text("/build/\n")
    needed = (KIT / "applications/examples/tvui/expected-needed.txt").read_text()
    needed = needed.replace("for wnk-tvui on webOS (same as Example A)", f"for {target} on webOS (as the kit's Example B)", 1)
    (out / "expected-needed.txt").write_text(needed)
    for icon, size in (("icon.png", 80), ("largeIcon.png", 130)):
        subprocess.run([sys.executable, str(KIT / "tools/make-icon.py"), str(out / icon), str(size)], check=True)

    print(f"created {out}  (kit: {kit_rel})")
    print(f"next: cd {out} && ./build.sh")
    return 0


if __name__ == "__main__":
    sys.exit(main())
