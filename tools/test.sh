#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build and run the host test suite (C++ unit tests + Python packaging tests) for a host preset.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
preset="${1:-host-debug}"
"$KIT_ROOT/tools/build-host.sh" "$preset"
cd "$KIT_ROOT"
ctest --preset "$preset"
