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
# Subtitles: H.264 + AAC with two SubRip tracks, English (default) naming each second from 0.2 s to
# 0.9 s of it, French with two-line cues every three seconds.
srt_en="$out/.en.srt"; srt_fr="$out/.fr.srt"; : > "$srt_en"; : > "$srt_fr"
for i in $(seq 0 14); do
  printf '%d\n00:00:%02d,200 --> 00:00:%02d,900\nSecond %d\n\n' $((i + 1)) "$i" "$i" "$i" >> "$srt_en"
done
for i in 0 3 6 9 12; do
  printf '%d\n00:00:%02d,000 --> 00:00:%02d,500\nSous-titre français numéro %d\n<i>deuxième ligne</i>\n\n' \
    $((i / 3 + 1)) "$i" $((i + 2)) $((i / 3 + 1)) >> "$srt_fr"
done
ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$V" -f lavfi -i "$A" -i "$srt_en" -i "$srt_fr" \
       -map 0:v -map 1:a -map 2 -map 3 -g 25 -keyint_min 25 -threads 1 \
       -map_metadata -1 -fflags +bitexact -flags:v +bitexact -flags:a +bitexact \
       -c:v libx264 -preset veryfast -crf 33 -pix_fmt yuv420p -c:a aac -b:a 128k -c:s srt \
       -metadata:s:s:0 language=eng -metadata:s:s:0 title=English -disposition:s:0 default \
       -metadata:s:s:1 language=fra -metadata:s:s:1 title=Français -disposition:s:1 0 "$out/h264-aac-subs.mkv"
rm -f "$srt_en" "$srt_fr"
note "test clips written to $out"
