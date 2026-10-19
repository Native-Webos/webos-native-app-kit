#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Install an .ipk on a Developer Mode TV with ares-cli-rs. Target: $ARES_DEVICE, else the default device.
#   tools/install.sh PACKAGE.ipk [--private-runtime DIR]...   (passed to verify.sh)
# Runs tools/verify.sh first and refuses to install a package that fails it, unless WNK_SKIP_VERIFY=1.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
[[ $# -ge 1 && -f "$1" ]] || die "usage: install.sh PACKAGE.ipk [--private-runtime DIR]..."
ipk="$1"; shift
verify_args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --private-runtime) [[ $# -ge 2 ]] || die "--private-runtime needs a directory"; verify_args+=("$1" "$2"); shift 2 ;;
        *) die "unknown option $1" ;;
    esac
done
need ares-install "install ares-cli-rs (https://github.com/webosbrew/ares-cli-rs/releases) and run ares-setup-device"
if [[ "${WNK_SKIP_VERIFY:-0}" != 1 ]]; then
    "$KIT_ROOT/tools/verify.sh" "$ipk" "${verify_args[@]}" || die "verify failed; fix it or set WNK_SKIP_VERIFY=1 to install anyway"
fi
mapfile -t dev < <(ares_device_args)
ares-install "${dev[@]}" "$ipk"
