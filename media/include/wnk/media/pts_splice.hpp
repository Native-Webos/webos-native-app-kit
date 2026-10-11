// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PTS mapping behind splice seeks (backends with seek_splices): after splice(), the next packet
// with a PTS is placed `gap_us` after the highest PTS mapped so far, or after the decoder's clock if
// that has run past it (a burst of seeks can leave nothing fed for a second while the clock goes on),
// and everything after it keeps that offset, so the decoder's timeline only moves forward and new
// content is never late. unmap() turns a decoder timestamp back
// into content time, using the offset that was in force where that timestamp was fed. Pure.
#pragma once

#include "wnk/media/media.hpp"

#include <cstdint>
#include <vector>

namespace wnk::media {

class PtsSplice {
 public:
    // `start_us`: where the first packet after a reset lands (kNoTime: where it is). Starfish's clock
    // starts at 0 and holds the first frame until it reaches its PTS, so a live stream joined 54 s
    // into its window must be moved to the start of the decoder's timeline.
    // `clock_lead_us`: how far ahead of the decoder's clock new content must land at least, so the
    // decoder has time to take the keyframe. Frames that arrive late stay late on Starfish: a splice
    // 0.27 s ahead of its clock dropped every frame for the rest of playback (OLED65C1PTB).
    explicit PtsSplice(int64_t gap_us, int64_t start_us = kNoTime, int64_t clock_lead_us = 0)
        : gap_us_(gap_us), start_us_(start_us), clock_lead_us_(clock_lead_us) {}
    void reset() { *this = PtsSplice(gap_us_, start_us_, clock_lead_us_); }
    void splice() { pending_ = true; }
    // kNoTime stays kNoTime. `clock_out_us`: where the decoder's clock is now, if known (only used
    // for the first packet after a splice).
    int64_t map(int64_t pts_us, int64_t clock_out_us = kNoTime);
    int64_t unmap(int64_t out_us) const;
    int64_t shift_us() const { return shift_us_; }

 private:
    struct Segment {
        int64_t out_start;  // first decoder timestamp fed with this shift
        int64_t shift;
    };
    int64_t gap_us_;
    int64_t start_us_;
    int64_t clock_lead_us_;
    int64_t shift_us_ = 0;
    bool pending_ = false;
    int64_t max_out_us_ = kNoTime;
    std::vector<Segment> segments_;  // the most recent few, oldest first
};

}  // namespace wnk::media
