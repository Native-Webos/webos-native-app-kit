#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Launch an installed app through the application manager (needed for the video plane to composite).
#   tools/launch.sh APP_ID [PARAMS_JSON]       e.g. tools/launch.sh org.wnk.examples.hello
#   tools/launch.sh --close APP_ID
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
need ares-launch "install ares-cli-rs (https://github.com/webosbrew/ares-cli-rs/releases)"
mapfile -t dev < <(ares_device_args)
if [[ "${1:-}" == --close ]]; then
    [[ $# -eq 2 ]] || die "usage: launch.sh --close APP_ID"
    exec ares-launch "${dev[@]}" --close "$2"
fi
[[ $# -ge 1 ]] || die "usage: launch.sh APP_ID [PARAMS_JSON]"
if [[ $# -ge 2 ]]; then exec ares-launch "${dev[@]}" "$1" --params "$2"; fi
exec ares-launch "${dev[@]}" "$1"
