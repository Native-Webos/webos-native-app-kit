#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Report what this machine has for host builds, webOS builds, packaging checks and TV work.
# Read-only: it never installs or changes anything. Exit 1 if a required (host build) item is missing.
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

missing_required=0
row() { printf '  %-30s %-8s %s\n' "$1" "$2" "$3"; }
have() { command -v "$1" >/dev/null 2>&1; }
ver() { "$@" 2>&1 | head -1; }

note "Host build (required)"
for t in cmake c++ make python3 pkg-config; do
    if have "$t"; then row "$t" ok "$(ver "$t" --version)"; else row "$t" MISSING ""; missing_required=1; fi
done
if pkg-config --exists sdl2 2>/dev/null; then row "SDL2 (host)" ok "$(pkg-config --modversion sdl2)"
else row "SDL2 (host)" MISSING "sudo apt install libsdl2-dev"; missing_required=1; fi
if pkg-config --exists glesv2 2>/dev/null; then row "GLESv2 (host)" ok "$(pkg-config --modversion glesv2)"
else row "GLESv2 (host)" MISSING "sudo apt install libgles-dev libegl-dev"; missing_required=1; fi

note ""
note "webOS build"
if ( resolve_ndk ) >/dev/null 2>&1; then
    resolve_ndk
    row "webOS NDK" ok "$WEBOS_NDK"
    row "  compiler" ok "$(ver "$WEBOS_NDK/bin/arm-webos-linux-gnueabi-gcc" --version)"
else
    row "webOS NDK" MISSING "https://github.com/openlgtv/buildroot-nc4/releases (then relocate-sdk.sh; set WEBOS_NDK)"
fi
if have git; then row "git" ok "$(ver git --version)"; else row "git" MISSING "needed to fetch SDL-webOS"; fi

note ""
note "Packaging checks"
db="$(python3 -c 'import sys; sys.path.insert(0, sys.argv[1]); import wnk_fwdb; print(wnk_fwdb.find_db() or "")' "$KIT_ROOT/tools" 2>/dev/null)"
if [[ -n "$db" ]]; then row "firmware inventory" ok "$db ($(ls "$db" | wc -l) images)"
else row "firmware inventory" MISSING "webosbrew-toolbox-fw-symbols .deb from https://github.com/webosbrew/dev-toolbox-cli/releases"; fi
if have webosbrew-ipk-verify; then row "webosbrew-ipk-verify" ok "$(command -v webosbrew-ipk-verify)"
else row "webosbrew-ipk-verify" optional "webosbrew-toolbox-ipk-verify .deb (same release page)"; fi
if have webosbrew-gen-manifest; then row "webosbrew-gen-manifest" ok ""
else row "webosbrew-gen-manifest" optional "only needed to publish to Homebrew Channel"; fi

note ""
note "TV (Developer Mode, ares-cli-rs)"
for t in ares-setup-device ares-install ares-launch ares-pull ares-package; do
    if have "$t"; then row "$t" ok "$(command -v "$t")"
    else row "$t" missing "https://github.com/webosbrew/ares-cli-rs/releases (amd64 .deb or tarball)"; fi
done
if have ares-setup-device; then
    note "  configured devices:"
    ares-setup-device -l 2>&1 | sed 's/^/    /'
fi
[[ -n "${ARES_DEVICE:-}" ]] && row "ARES_DEVICE" set "$ARES_DEVICE"

note ""
if (( missing_required )); then note "doctor: host build requirements missing"; exit 1; fi
note "doctor: host build requirements present"
