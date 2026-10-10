#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Install an .ipk on a Developer Mode TV with ares-cli-rs. Target: $ARES_DEVICE, else the default device.
#   tools/install.sh PACKAGE.ipk
# Runs tools/verify.sh first and refuses to install a package that fails it, unless WNK_SKIP_VERIFY=1.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
[[ $# -eq 1 && -f "$1" ]] || die "usage: install.sh PACKAGE.ipk"
need ares-install "install ares-cli-rs (https://github.com/webosbrew/ares-cli-rs/releases) and run ares-setup-device"
if [[ "${WNK_SKIP_VERIFY:-0}" != 1 ]]; then
    "$KIT_ROOT/tools/verify.sh" "$1" || die "verify failed; fix it or set WNK_SKIP_VERIFY=1 to install anyway"
fi
mapfile -t dev < <(ares_device_args)
ares-install "${dev[@]}" "$1"
