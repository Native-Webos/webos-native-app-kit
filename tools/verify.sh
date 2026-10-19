#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Check that an .ipk would load on the targeted firmware releases. Offline; no TV.
#   tools/verify.sh PACKAGE.ipk [--min-release 5.3.1] [--max-release X] [--runtime-libs FILE]
#                   [--private-runtime DIR]...   (passed to check-package.py; see its --help)
# Runs tools/fw-check.py (always) and webosbrew-ipk-verify (when installed). Both answer
# "would the dynamic loader start it", not "does it work". Libraries the app or SDL dlopen()s
# at runtime are invisible to both.
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
[[ $# -ge 1 && -f "$1" ]] || die "usage: verify.sh PACKAGE.ipk [--min-release X.Y.Z] [--max-release X.Y.Z]"
ipk="$1"; shift
min="5.3.1"; max=""; runtime_libs=""; check_args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --min-release) min="$2"; shift 2 ;;
        --max-release) max="$2"; shift 2 ;;
        --runtime-libs) runtime_libs="$2"; shift 2 ;;
        --private-runtime) check_args+=(--private-runtime "$2"); shift 2 ;;
        *) die "unknown option $1" ;;
    esac
done

status=0
python3 "$KIT_ROOT/tools/check-package.py" "$ipk" "${check_args[@]}" >/dev/null || { note "check-package failed (run it directly for details)"; status=1; }
args=(--min-release "$min"); [[ -n "$max" ]] && args+=(--max-release "$max")
[[ -n "$runtime_libs" ]] && args+=(--runtime-libs "$runtime_libs")
python3 "$KIT_ROOT/tools/fw-check.py" "$ipk" "${args[@]}"; rc=$?
(( rc != 0 )) && status=1

if command -v webosbrew-ipk-verify >/dev/null 2>&1; then
    range=">=$min"; [[ -n "$max" ]] && range="$range, <=$max"
    note ""
    note "webosbrew-ipk-verify --fw-releases '$range'"
    webosbrew-ipk-verify -d -r "$range" "$ipk" || status=1
else
    note "(webosbrew-ipk-verify not installed: second opinion skipped)"
fi
exit "$status"
