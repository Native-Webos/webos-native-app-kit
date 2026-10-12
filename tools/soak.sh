#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run an installed app on a TV for hours and report memory growth, late frames and crashes
# (tools/soak-report.py on the app's heartbeat and perf lines). LG's checklist asks for a long run.
#   tools/soak.sh APP_ID [--hours 4] [--interval-min 15] [--params JSON] [--dry-run]
#
# The app is started once and left alone (drive it with the remote, or pass params that make it busy,
# e.g. Example D {"autoplay":7}); every interval the event log is pulled, so a crash or kill part-way
# still leaves the log up to then. The TV must not sleep: hold a wake lock (video does) or keep the
# TV's screensaver/energy settings in mind. --dry-run prints the plan.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
app="" hours=4 interval=15 params="" dry=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --hours) hours="${2:?}"; shift 2 ;;
        --interval-min) interval="${2:?}"; shift 2 ;;
        --params) params="${2:?}"; shift 2 ;;
        --dry-run) dry=1; shift ;;
        -*) die "usage: soak.sh APP_ID [--hours H] [--interval-min M] [--params JSON] [--dry-run]" ;;
        *) app="$1"; shift ;;
    esac
done
[[ -n "$app" ]] || die "usage: soak.sh APP_ID [--hours H] [--interval-min M] [--params JSON] [--dry-run]"
[[ "$hours" =~ ^[0-9]+([.][0-9]+)?$ && "$interval" =~ ^[0-9]+$ && "$interval" -ge 1 ]] || die "bad --hours or --interval-min"
tools="$(dirname "${BASH_SOURCE[0]}")"
total_s=$(python3 -c "print(int(float('$hours') * 3600))")
rounds=$(( (total_s + interval * 60 - 1) / (interval * 60) ))
note "soak $app for $hours h: $rounds log pulls every $interval min"
if [[ $dry -eq 1 ]]; then
    note "   $tools/launch.sh --close $app; $tools/launch.sh $app ${params:+'$params'}"
    note "   every $interval min: $tools/logs.sh $app <dir>; at the end: $tools/soak-report.py <latest log>"
    exit 0
fi

out="$KIT_ROOT/build/device-logs/soak-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
"$tools/launch.sh" --close "$app" >/dev/null 2>&1 || true
sleep 2
if [[ -n "$params" ]]; then "$tools/launch.sh" "$app" "$params"; else "$tools/launch.sh" "$app"; fi
for ((i = 1; i <= rounds; i++)); do
    sleep $((interval * 60))
    "$tools/logs.sh" "$app" "$out" || warn "log pull $i failed (TV asleep or unreachable?)"
    log=$(ls -t "$out"/*.log 2>/dev/null | head -1 || true)
    [[ -n "$log" ]] && python3 "$tools/soak-report.py" "$log" | head -3
done
log=$(ls -t "$out"/*.log | head -1)
note "== final report ($log)"
python3 "$tools/soak-report.py" "$log"
