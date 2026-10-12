#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Drive an installed app through the lifecycle paths of LG's App Self Checklist on a TV, then check its
# event log (tools/check-lifecycle-log.py).
#   tools/lifecycle-check.sh APP_ID [--other APP_ID] [--params JSON] [--dry-run]
#
#   1. close it if it runs, then a cold start          4. launch it again: back to the foreground
#   2. relaunch with {"wnkCheck":"relaunch"}           5. close it through the application manager
#   3. launch --other (default Example A): background  6. pull /tmp/<APP_ID>/events.log and check it
#
# Needs the TV on and reachable as the ares device (tools/doctor.sh). --dry-run prints the steps.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
app="" other="org.wnk.examples.hello" params="" dry=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --other) other="${2:?}"; shift 2 ;;
        --params) params="${2:?}"; shift 2 ;;
        --dry-run) dry=1; shift ;;
        -*) die "usage: lifecycle-check.sh APP_ID [--other APP_ID] [--params JSON] [--dry-run]" ;;
        *) app="$1"; shift ;;
    esac
done
[[ -n "$app" ]] || die "usage: lifecycle-check.sh APP_ID [--other APP_ID] [--params JSON] [--dry-run]"
[[ "$other" != "$app" ]] || die "--other must be a different app"
tools="$(dirname "${BASH_SOURCE[0]}")"

run() {  # run DESCRIPTION SECONDS_TO_WAIT_AFTER COMMAND...
    local what="$1" wait="$2"; shift 2
    note "== $what"
    if [[ $dry -eq 1 ]]; then printf '   %q' "$@"; printf '\n'; return 0; fi
    "$@" || warn "$what: command failed"
    sleep "$wait"
}

run "close $app if it runs" 2 "$tools/launch.sh" --close "$app"
if [[ -n "$params" ]]; then
    run "cold start" 6 "$tools/launch.sh" "$app" "$params"
else
    run "cold start" 6 "$tools/launch.sh" "$app"
fi
run "relaunch with parameters" 3 "$tools/launch.sh" "$app" '{"wnkCheck":"relaunch"}'
run "launch $other: $app goes to the background" 4 "$tools/launch.sh" "$other"
run "launch $app again: back to the foreground" 4 "$tools/launch.sh" "$app"
run "close $app" 3 "$tools/launch.sh" --close "$app"
run "close $other" 1 "$tools/launch.sh" --close "$other"
[[ $dry -eq 1 ]] && { note "== check the log: $tools/check-lifecycle-log.py <log> --expect-relaunch --expect-background"; exit 0; }

out="$KIT_ROOT/build/device-logs/lifecycle-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
"$tools/logs.sh" "$app" "$out"
log=$(ls -t "$out"/*.log | head -1)
python3 "$tools/check-lifecycle-log.py" "$log" --expect-relaunch --expect-background
