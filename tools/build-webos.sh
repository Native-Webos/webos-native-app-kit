#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Cross-build for the TV with the webOS NDK and package every example (default preset webos-release).
#   tools/build-webos.sh [webos-release|webos-debug]
# Output: build/<preset>/dist/*.ipk, each already checked by tools/check-package.py.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
preset="${1:-webos-release}"
[[ "$preset" == webos-* ]] || die "build-webos.sh builds webOS presets only (got $preset)"
resolve_ndk
need git "SDL-webOS is fetched from GitHub at a pinned tag"
cd "$KIT_ROOT"
cmake --preset "$preset"
cmake --build --preset "$preset" -j"$(nproc)"
cmake --build --preset "$preset" --target wnk-hello-ipk
ls -l "build/$preset/dist/"
