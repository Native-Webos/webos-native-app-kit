# SPDX-License-Identifier: GPL-3.0-or-later
# Shared helpers for tools/*.sh. Source it; do not execute.

KIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
note() { printf '%s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }

# The openlgtv buildroot NDK. Override with WEBOS_NDK=/path/to/arm-webos-linux-gnueabi_sdk-buildroot.
resolve_ndk() {
    if [[ -z "${WEBOS_NDK:-}" ]]; then
        local c
        for c in "$HOME/webos-ndk/arm-webos-linux-gnueabi_sdk-buildroot" \
                 "/opt/arm-webos-linux-gnueabi_sdk-buildroot"; do
            if [[ -f "$c/share/buildroot/toolchainfile.cmake" ]]; then WEBOS_NDK="$c"; break; fi
        done
    fi
    [[ -n "${WEBOS_NDK:-}" && -f "$WEBOS_NDK/share/buildroot/toolchainfile.cmake" ]] ||
        die "webOS NDK not found. Download it from https://github.com/openlgtv/buildroot-nc4/releases, run its relocate-sdk.sh, and set WEBOS_NDK to its directory."
    export WEBOS_NDK
}

need() { command -v "$1" >/dev/null 2>&1 || die "$1 is required: $2"; }

ares_device_args() {
    if [[ -n "${ARES_DEVICE:-}" ]]; then printf -- '-d\n%s\n' "$ARES_DEVICE"; fi
}
