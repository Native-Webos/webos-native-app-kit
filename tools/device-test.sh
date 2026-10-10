#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Cross-build platform-independent unit tests for ARM, run them on a Developer Mode TV, then remove them.
#   tools/device-test.sh [test_name ...]        default: test_input test_focus test_async
# Runs in the Developer Mode shell (user prisoner), not inside an app's jail, so it verifies code on the
# TV's CPU, kernel and C library; it does not exercise app permissions (DB8, display). Uses
# ares-shell/ares-push from ares-cli-rs and $ARES_DEVICE (or the default device).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
resolve_ndk
need ares-shell "install ares-cli-rs (https://github.com/webosbrew/ares-cli-rs/releases)"
need ares-push "install ares-cli-rs (https://github.com/webosbrew/ares-cli-rs/releases)"
tests=("$@")
[[ ${#tests[@]} -gt 0 ]] || tests=(test_input test_focus test_async)

build="$KIT_ROOT/build/webos-tests"
cmake -S "$KIT_ROOT" -B "$build" -DCMAKE_TOOLCHAIN_FILE="$WEBOS_NDK/share/buildroot/toolchainfile.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DWNK_BUILD_TESTS=ON -DWNK_BUILD_EXAMPLES=OFF >/dev/null
cmake --build "$build" -j"$(nproc)" --target "${tests[@]}"

mapfile -t dev < <(ares_device_args)
remote="/tmp/wnk-device-tests-$$"
files=()
for t in "${tests[@]}"; do files+=("$build/tests/$t"); done
ares-shell "${dev[@]}" --no-pty -r "mkdir -p $remote"
ares-push "${dev[@]}" -i "${files[@]}" "$remote/"
script="cd $remote && chmod +x test_* && status=0;"
for t in "${tests[@]}"; do script+=" echo '== $t'; ./$t || status=1;"; done
script+=" cd /tmp && rm -rf $remote; exit \$status"
ares-shell "${dev[@]}" --no-pty -r "$script"
