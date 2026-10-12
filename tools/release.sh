#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Make a release of the kit's example packages from a clean, committed tree.
#   tools/release.sh [--source-url URL] [--check-reproducible] [--skip-tests]
#
# Output: build/release/<version>/ with
#   *.ipk                            every example, built in a fresh directory, checked by check-package
#   webos-native-app-kit-<v>-src.tar.gz   the corresponding source (git archive of the release commit)
#   ffmpeg-<v>.tar.xz                the upstream FFmpeg source of the bundled libraries (LGPL-2.1 §6)
#   RELEASE-NOTES.md                 packages, firmware loadability, TV results (tools/release-notes.py)
#   SHA256SUMS
#
# <version> is the kit's project() VERSION in CMakeLists.txt. Steps, each of which stops the release:
#   1. the working tree is clean (the source archive must be exactly what was built)
#   2. host tests (tools/test.sh host-debug) and the packaging tool tests, unless --skip-tests
#   3. docs/compatibility/records.json validates (tools/check-compat.py)
#   4. an ARM build in build/release/<version>/build with -DWNK_SOURCE_URL (each package's
#      licenses/SOURCE.txt names it); check-package runs for every package as it is made
#   5. tools/fw-check.py for every package (webOS 5.3.1 and later)
#   6. with --check-reproducible: a second build in another directory must give identical packages
# Nothing is tagged or pushed: the script prints the tag command to run once the release is accepted.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

source_url=""
reproducible=unchecked
check_repro=0
skip_tests=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --source-url) source_url="${2:?--source-url needs a URL}"; shift 2 ;;
    --check-reproducible) check_repro=1; shift ;;
    --skip-tests) skip_tests=1; shift ;;
    *) die "usage: release.sh [--source-url URL] [--check-reproducible] [--skip-tests]" ;;
  esac
done

cd "$KIT_ROOT"
need git "the source archive is made with git archive"
resolve_ndk
version=$(sed -n 's/^project(webos-native-app-kit VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
[[ -n "$version" ]] || die "no VERSION in CMakeLists.txt project()"
[[ -z "$(git status --porcelain)" ]] || die "the working tree has uncommitted changes; commit or stash them first"
commit=$(git rev-parse HEAD)
src_name="webos-native-app-kit-$version-src.tar.gz"
[[ -n "$source_url" ]] || source_url="$src_name, published with this package"
out="build/release/$version"
note "release $version from $commit -> $out"
rm -rf "$out"
mkdir -p "$out"

if [[ $skip_tests -eq 0 ]]; then
  note "host tests"
  tools/test.sh host-debug
  python3 -m unittest discover -s tests/tools -p "test_*.py"
fi
python3 tools/check-compat.py

build_once() {  # build_once DIR
  local dir="$1"
  cmake -S . -B "$dir" -G "Unix Makefiles" --toolchain "$WEBOS_NDK/share/buildroot/toolchainfile.cmake" \
      -DCMAKE_BUILD_TYPE=Release -DWNK_BUILD_TESTS=OFF -DWNK_SOURCE_URL="$source_url" >/dev/null
  cmake --build "$dir" -j"$(nproc)"
  cmake --build "$dir" --target wnk-ipks
}

note "ARM build"
build_once "$out/build"
cp "$out"/build/dist/*.ipk "$out/"

note "firmware loadability"
fw_logs=()
for ipk in "$out"/*.ipk; do
  log="$out/build/$(basename "$ipk" .ipk).fw.log"
  python3 tools/fw-check.py "$ipk" --min-release 5.3.1 --runtime-libs cmake/sdl-webos-runtime-libs.txt | tee "$log"
  fw_logs+=(--fw-log "$log")
done

if [[ $check_repro -eq 1 ]]; then
  note "second build for the reproducibility check"
  build_once "$out/build-again"
  reproducible=yes
  for ipk in "$out"/*.ipk; do
    again="$out/build-again/dist/$(basename "$ipk")"
    if cmp -s "$ipk" "$again"; then
      note "identical: $(basename "$ipk")"
    else
      reproducible=no
      warn "DIFFERENT: $(basename "$ipk")"
    fi
  done
fi

note "source archives"
git archive --format=tar --prefix="webos-native-app-kit-$version/" "$commit" | gzip -n -9 > "$out/$src_name"
if compgen -G "$out/*.ipk" >/dev/null && python3 - "$out" <<'EOF'
import sys
from pathlib import Path
sys.path.insert(0, "tools")
from wnk_ipk import read_payload
sys.exit(0 if any("-wnk.so" in n for ipk in Path(sys.argv[1]).glob("*.ipk") for n in read_payload(ipk).files) else 1)
EOF
then
  ff_version=$(sed -n 's/^VERSION=//p' tools/build-ffmpeg.sh)
  ff_sha=$(sed -n 's/^SHA256=//p' tools/build-ffmpeg.sh)
  ff_tar=$(ls "$HOME"/.cache/wnk/ffmpeg/ffmpeg-"$ff_version".tar.xz "$HOME"/.cache/wnk/ffmpeg/*/ffmpeg-"$ff_version".tar.xz \
           "$HOME"/.cache/plxnative/ffmpeg/ffmpeg-"$ff_version"-*.tar.xz 2>/dev/null | head -1 || true)
  if [[ -z "$ff_tar" ]]; then
    curl -fsSL -o "$out/ffmpeg-$ff_version.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$ff_version.tar.xz"
  else
    cp "$ff_tar" "$out/ffmpeg-$ff_version.tar.xz"
  fi
  [[ "$(sha256sum "$out/ffmpeg-$ff_version.tar.xz" | cut -d' ' -f1)" == "$ff_sha" ]] \
    || die "the FFmpeg tarball does not match tools/build-ffmpeg.sh's SHA-256"
fi

python3 tools/release-notes.py "$out" --version "$version" --commit "$commit" --reproducible "$reproducible" "${fw_logs[@]}"
(cd "$out" && sha256sum -- *.ipk *.tar.* RELEASE-NOTES.md > SHA256SUMS)
rm -rf "$out/build-again"
[[ "$reproducible" != no ]] || die "packages are not reproducible; see above (the release directory is kept for inspection)"
note "release $version ready in $out"
note "after checking it: git tag -a v$version -m \"webOS Native App Kit $version\" $commit"
