#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the FFmpeg the kit ships: demuxers, parsers, bitstream filters and the few audio decoders
# webOS's NDL lacks, as shared LGPL libraries with a "-wnk" suffix.
#
#   tools/build-ffmpeg.sh arm  PREFIX     cross-compile with the webOS NDK
#   tools/build-ffmpeg.sh host PREFIX     the same component list for host tests
#
# Design and most reasons follow PlxNative's ci/build-ffmpeg.sh (Copyright the PlxNative authors,
# GPL-3.0-or-later; see THIRD-PARTY-NOTICES.md):
#   * Bundle, never use the TV's FFmpeg: its major moves with the firmware (57 on 4.x, 58 on 5-9,
#     59 on 10, 60 on 11 in the firmware inventory) and which demuxers and filters it contains is
#     registry data no symbol table can show. webosbrew's guidance agrees ("Don't use system FFmpeg
#     libraries!", webosbrew.org/develop/caniuse/?q=ffmpeg).
#   * Shared, LGPL-2.1 (no --enable-gpl/--enable-nonfree): the libraries stay replaceable (§6(b)).
#   * --build-suffix=-wnk: webOS 11.2 ships FFmpeg 6 itself; our SONAMEs must never collide with it.
#   * --prefix=/wnk with DESTDIR: FFmpeg records its configure line inside libavutil, so a real
#     prefix would leak this machine's paths into the shipped libraries.
#   * -tno-glibc-polyfill: the NDK's gcc links libglibc_polyfills.a (a getauxval shim of unknown
#     licence) unless told not to, and FFmpeg's ARM CPU detection would pull it in.
#   * --disable-autodetect, --enable-zlib explicitly: nothing from the build machine leaks in; zlib
#     (libz.so.1 on every inventoried firmware) lets matroska inflate compressed tracks.
#
# Components (each maps to kit code in media/src/ffmpeg_source.cpp):
#   demuxers  containers and raw streams the kit opens
#   parsers   fill codec parameters during stream probing
#   bsf       length-prefixed (MP4/MKV) H.264/HEVC → Annex-B, which NDL requires
#   decoders  AAC, AC-3, E-AC-3 and MP3, decoded to PCM: PCM is the NDL v2 audio path measured to work
#             (MP3 passed through to NDL was silent on an OLED65C1PTB, webOS 6.5.3). Video is never
#             decoded here.
#   protocol  file only; network reads go through the kit's own HTTP client (custom AVIO), later.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

VERSION=9.0
SHA256=7f607a00dd0d28a729d5a4811205812eef01cf6ef6155025febb6f36a9062d52
TARGET="${1:?usage: build-ffmpeg.sh arm|host PREFIX}"
PREFIX="${2:?usage: build-ffmpeg.sh arm|host PREFIX}"
CACHE="${WNK_BUILD_CACHE:-$HOME/.cache/wnk}/ffmpeg"
mkdir -p "$CACHE" "$PREFIX"
PREFIX=$(cd "$PREFIX" && pwd)

COMPONENTS=(
  --disable-everything --disable-autodetect --enable-zlib
  --enable-demuxer=matroska,mov,mpegts,h264,hevc,aac,mp3,ogg,wav
  --enable-parser=h264,hevc,aac,ac3,mpegaudio,opus,vp9,av1
  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,extract_extradata
  --enable-decoder=aac,ac3,eac3,mp3float
  --enable-protocol=file
)
COMMON=(
  --prefix=/wnk --build-suffix=-wnk
  --enable-shared --disable-static --enable-pic
  --disable-programs --disable-doc --disable-avdevice --disable-avfilter --disable-swscale
  --disable-swresample --disable-network --disable-debug --enable-small
)

case "$TARGET" in
  arm)
    resolve_ndk
    CROSS="$WEBOS_NDK/bin/arm-webos-linux-gnueabi-"
    SYSROOT="$WEBOS_NDK/arm-webos-linux-gnueabi/sysroot"
    # Absolute --cross-prefix: the NDK gcc is a wrapper that fails when found through PATH (plx note).
    PLATFORM=(--enable-cross-compile --cross-prefix="$CROSS" --arch=arm --cpu=cortex-a9 --target-os=linux
              --sysroot="$SYSROOT" --extra-cflags=-Dstatic_assert=_Static_assert
              --extra-ldflags="-tno-glibc-polyfill -Wl,--build-id=sha1")
    TOOLCHAIN="$("${CROSS}gcc" -dumpmachine)|$("${CROSS}gcc" --version | head -1)"
    ;;
  host)
    # Host builds only serve tests: no hand-written x86 assembly, so nasm is not needed.
    PLATFORM=(--disable-x86asm)
    TOOLCHAIN="$(cc -dumpmachine)|$(cc --version | head -1)"
    ;;
  *) die "target must be arm or host (got $TARGET)" ;;
esac

ARGS=("${COMMON[@]}" "${COMPONENTS[@]}" "${PLATFORM[@]}")
KEY=$(printf '%s|%s|%s|%s' "$VERSION" "$TARGET" "$TOOLCHAIN" "${ARGS[*]}" | sha256sum | cut -c1-16)
STAMP="$PREFIX/.wnk-ffmpeg"
if [[ -f "$STAMP" && "$(cat "$STAMP")" == "$KEY" && -f "$PREFIX/include/libavformat/avformat.h" ]]; then
  note "ffmpeg $VERSION ($TARGET) up to date in $PREFIX"
  exit 0
fi

# Source tarball: our cache, PlxNative's cache (same pinned release), or ffmpeg.org. Always verified.
TAR="$CACHE/ffmpeg-$VERSION.tar.xz"
verified() { [[ -f "$1" ]] && [[ "$(sha256sum "$1" | cut -d' ' -f1)" == "$SHA256" ]]; }
if ! verified "$TAR"; then
  plx_tar=$(ls "$HOME"/.cache/plxnative/ffmpeg/ffmpeg-"$VERSION"-*.tar.xz 2>/dev/null | head -1 || true)
  if [[ -n "$plx_tar" ]] && verified "$plx_tar"; then
    cp "$plx_tar" "$TAR.part"
  else
    need curl "to download the FFmpeg source"
    note "downloading ffmpeg $VERSION"
    curl -fsSL "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz" -o "$TAR.part"
  fi
  verified "$TAR.part" || die "ffmpeg tarball SHA-256 mismatch (expected $SHA256)"
  mv "$TAR.part" "$TAR"
fi

WORK="$CACHE/$TARGET-$VERSION-$KEY"
SRC="$WORK/ffmpeg-$VERSION"
if [[ ! -f "$SRC/configure" ]]; then
  rm -rf "$WORK"
  mkdir -p "$WORK"
  tar xf "$TAR" -C "$WORK"
fi
cd "$SRC"
if [[ ! -f "$WORK/.configured" ]]; then
  note "configuring ffmpeg $VERSION for $TARGET"
  ./configure "${ARGS[@]}" > "$WORK/configure.log" 2>&1 || { tail -20 "$WORK/configure.log" >&2; die "configure failed: $WORK/configure.log"; }
  touch "$WORK/.configured"
fi
note "building ffmpeg $VERSION for $TARGET (a few minutes the first time)"
make -j"$(nproc)" > "$WORK/build.log" 2>&1 || { tail -20 "$WORK/build.log" >&2; die "build failed: $WORK/build.log"; }
rm -rf "$WORK/destdir"
make install DESTDIR="$WORK/destdir" > "$WORK/install.log" 2>&1
rm -rf "$PREFIX"/{include,lib}
cp -R "$WORK/destdir/wnk/." "$PREFIX/"
printf '%s' "$KEY" > "$STAMP"
note "ffmpeg $VERSION ($TARGET) installed in $PREFIX"
