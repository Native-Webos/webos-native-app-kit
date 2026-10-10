#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Configure and build the host preset (default host-debug; pass host-asan for sanitizers).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
preset="${1:-host-debug}"
[[ "$preset" == host-* ]] || die "build-host.sh builds host presets only (got $preset)"
cd "$KIT_ROOT"
cmake --preset "$preset"
cmake --build --preset "$preset" -j"$(nproc)"
