// SPDX-License-Identifier: GPL-3.0-or-later
//
// WebVTT (W3C) cue text for subtitles delivered as plain files: HLS subtitle segments and DASH
// text/vtt. Cue settings and styling are dropped (the kit draws plain text): tags such as <i>, <b>,
// <c.class>, <v Speaker> and timestamps inside cue text are removed, the character references
// &amp; &lt; &gt; &lrm; &rlm; &nbsp; decoded. NOTE, STYLE and REGION blocks are skipped.
//
// HLS segments carry X-TIMESTAMP-MAP=MPEGTS:<90 kHz>,LOCAL:<cue time>, which says which media
// timestamp a cue time stands for (RFC 8216 3.5); it is reported so the caller can place the cues
// on its own timeline.
#pragma once

#include "wnk/media/media.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wnk::media {

struct WebVttFile {
    std::vector<SubtitleCue> cues;  // track -1; start/end in the file's own cue time
    bool has_timestamp_map = false;
    int64_t map_mpegts = 0;   // 90 kHz ticks
    int64_t map_local_us = 0;
};

// False (with a reason) if `text` is not WebVTT. Malformed cues are skipped.
bool parse_webvtt(const std::string &text, WebVttFile *out, std::string *error);

// "01:02:03.456" or "02:03.456" → microseconds; false if malformed.
bool parse_vtt_timestamp(const std::string &s, int64_t *us);

// Cue text without tags, with character references decoded (lines kept).
std::string webvtt_plain_text(const std::string &cue_text);

// WebVTT in ISO BMFF (ISO/IEC 14496-30, DASH "wvtt"), which FFmpeg does not read. The init segment
// gives the track's timescale (mdhd); each fragment (moof + mdat) gives samples whose boxes are cues
// (vttc, text in payl) or gaps (vtte). Times are media time in microseconds (tfdt/timescale).
bool parse_wvtt_init(const uint8_t *data, size_t size, uint32_t *timescale, std::string *error);
bool parse_wvtt_fragment(const uint8_t *data, size_t size, uint32_t timescale, std::vector<SubtitleCue> *out,
                         std::string *error);

}  // namespace wnk::media
