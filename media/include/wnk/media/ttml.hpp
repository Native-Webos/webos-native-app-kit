// SPDX-License-Identifier: GPL-3.0-or-later
//
// TTML (W3C Timed Text, and its IMSC / EBU-TT-D / SMPTE-TT profiles) as plain cue text, for DASH
// subtitles (application/ttml+xml files, and TTML in MP4: "stpp") and subtitle files beside the
// media (.ttml, .dfxp). FFmpeg reads none of these.
//
// Timing: begin, end and dur on body, div, p and span, each relative to its parent (par) or to the
// previous sibling's end (seq); clock times (01:02:03.5, 01:02:03:12 frames), offsets (1.5s, 500ms,
// 2m, 1h, 25f, 900t) and the ttp:frameRate, frameRateMultiplier, subFrameRate and tickRate they
// need. A paragraph whose spans are timed on their own (roll-up and paint-on captions) becomes a cue
// for each stretch in which its text stays the same. <br/> breaks lines; xml:space="preserve" keeps
// the spaces as written, otherwise runs of white space become one. Styling, regions, animation
// (<set>) and images are dropped: the kit draws plain text.
#pragma once

#include "wnk/media/media.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wnk::media {

struct TtmlFile {
    std::vector<SubtitleCue> cues;  // track -1, sorted by start; end kNoTime when nothing ends it
};

// False (with a reason) if `doc` is not a TTML document. A document without text (a gap) is fine.
bool parse_ttml(const std::string &doc, TtmlFile *out, std::string *error);

// The rates time expressions are counted in (TTML 1, 10.2): frames per second after the multiplier,
// sub-frames per frame, ticks per second.
struct TtmlRates {
    double frame_rate = 30;
    double sub_frame_rate = 1;
    double tick_rate = 1;
};
// A time expression → microseconds; false if malformed.
bool parse_ttml_time(const std::string &s, const TtmlRates &rates, int64_t *us);

// TTML in MP4 (ISO/IEC 14496-30, DASH "stpp"): every sample of a fragment is a whole document. Its
// times are media time, as DASH-IF and EBU-TT-D in ISO BMFF say, except for documents whose cues all
// end before the sample starts, which some packagers time from the sample instead. The timescale
// comes from the init segment (parse_wvtt_init).
bool parse_stpp_fragment(const uint8_t *data, size_t size, uint32_t timescale, std::vector<SubtitleCue> *out,
                         std::string *error);

}  // namespace wnk::media
