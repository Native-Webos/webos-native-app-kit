#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetch an app's event log from a Developer Mode TV (unrooted: user prisoner, port 9922).
#   tools/logs.sh APP_ID [OUT_DIR]
# The app writes /tmp/<APP_ID>/events.log with mode 0640. Whether the Developer Mode shell user can
# read it is one of the first device experiments (docs/research/PACKAGING-AND-FIRMWARE.md E.3);
# if this fails, download the file with webOS Dev Manager instead.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
[[ $# -ge 1 ]] || die "usage: logs.sh APP_ID [OUT_DIR]"
app_id="$1"; out="${2:-$KIT_ROOT/build/device-logs}"
mkdir -p "$out"
need ares-pull "install ares-cli-rs (https://github.com/webosbrew/ares-cli-rs/releases)"
mapfile -t dev < <(ares_device_args)
dest="$out/${app_id}-events-$(date +%Y%m%d-%H%M%S).log"
ares-pull "${dev[@]}" "/tmp/$app_id/events.log" "$dest"
note "saved $dest"
