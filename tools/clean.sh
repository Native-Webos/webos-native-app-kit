#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Remove build output for one preset (or all with --all). Never touches sources.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
cd "$KIT_ROOT"
case "${1:-}" in
    --all) rm -rf build ;;
    host-*|webos-*) rm -rf "build/$1" ;;
    *) die "usage: clean.sh PRESET | --all" ;;
esac
