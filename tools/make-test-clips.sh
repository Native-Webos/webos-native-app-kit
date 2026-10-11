#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Generate the A/V test clips Example D ships, with the host's ffmpeg CLI:
#   tools/make-test-clips.sh OUT_DIR
# Every clip is 15 s of 1080p25 test pattern with a full-screen white flash and a 1 kHz beep during
# the first 80 ms of each second, so picture/sound sync and the decoder's PTS unit can be judged by
# eye and ear on the TV. Keyframe every second.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
need ffmpeg "to generate the player test clips (apt install ffmpeg)"
out="${1:?usage: make-test-clips.sh OUT_DIR}"
mkdir -p "$out"
V="testsrc2=size=1920x1080:rate=25:duration=15,drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable='lt(mod(t\,1)\,0.08)'"
A="sine=frequency=1000:sample_rate=48000:duration=15,volume=volume=0:enable='gte(mod(t\,1)\,0.08)'"
common=(-hide_banner -loglevel error -y -f lavfi -i "$V" -f lavfi -i "$A" -g 25 -keyint_min 25 -threads 1
        -map_metadata -1 -fflags +bitexact -flags:v +bitexact -flags:a +bitexact)
ffmpeg "${common[@]}" -c:v libx264 -preset veryfast -crf 33 -pix_fmt yuv420p -c:a aac -b:a 128k "$out/h264-aac.mp4"
ffmpeg "${common[@]}" -c:v libx265 -preset veryfast -crf 34 -x265-params log-level=error -pix_fmt yuv420p \
       -c:a ac3 -ac 6 -b:a 384k "$out/hevc-ac3-5.1.mkv"
ffmpeg "${common[@]}" -c:v libx264 -preset veryfast -crf 33 -pix_fmt yuv420p -c:a libmp3lame -b:a 128k \
       -f mpegts "$out/h264-mp3.ts"
note "test clips written to $out"
