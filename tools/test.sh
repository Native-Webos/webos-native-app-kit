#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build and run the host test suite (C++ unit tests + Python packaging tests) for a host preset:
#   tools/test.sh [host-debug|host-asan|host-tsan]   (host-tsan runs the C++ unit tests only)
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
preset="${1:-host-debug}"
"$KIT_ROOT/tools/build-host.sh" "$preset"
cd "$KIT_ROOT"
if [[ "$preset" == host-tsan ]]; then
    # ThreadSanitizer aborts with "unexpected memory mapping" on kernels with high mmap ASLR entropy
    # (seen on Linux 7.0). Running without address randomisation avoids it and needs no root.
    exec setarch "$(uname -m)" -R ctest --preset "$preset"
fi
ctest --preset "$preset"
