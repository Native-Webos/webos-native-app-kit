// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PTS mapping behind splice seeks (backends with seek_splices): after splice(), the next packet
// with a PTS is placed `gap_us` after the highest PTS mapped so far and everything after it keeps
// that offset, so the decoder's timeline only moves forward. unmap() turns a decoder timestamp back
// into content time, using the offset that was in force where that timestamp was fed. Pure.
#pragma once

#include "wnk/media/media.hpp"

#include <cstdint>
#include <vector>

namespace wnk::media {

class PtsSplice {
 public:
    explicit PtsSplice(int64_t gap_us) : gap_us_(gap_us) {}
    void reset() { *this = PtsSplice(gap_us_); }
    void splice() { pending_ = true; }
    // kNoTime stays kNoTime.
    int64_t map(int64_t pts_us);
    int64_t unmap(int64_t out_us) const;
    int64_t shift_us() const { return shift_us_; }

 private:
    struct Segment {
        int64_t out_start;  // first decoder timestamp fed with this shift
        int64_t shift;
    };
    int64_t gap_us_;
    int64_t shift_us_ = 0;
    bool pending_ = false;
    int64_t max_out_us_ = kNoTime;
    std::vector<Segment> segments_;  // the most recent few, oldest first
};

}  // namespace wnk::media
