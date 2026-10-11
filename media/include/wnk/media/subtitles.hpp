// SPDX-License-Identifier: GPL-3.0-or-later
//
// Subtitle cues between the demuxer and the screen.
//
//   SubtitleStore   every cue read so far, per track, sorted by start time. It survives seeks on
//                   purpose: after a seek Matroska does not resend a cue that started before the
//                   target, so the store is what still knows it (PlxNative found the same). Cues read
//                   again after a seek are recognised and dropped. Bitmap cues are evicted farthest
//                   from the playhead first once they exceed a byte budget. Thread-safe: the playback
//                   thread adds, the main thread asks what is showing.
//   ass_event_text  FFmpeg's text subtitle decoders (SubRip, WebVTT, MP4 timed text, ASS/SSA) all
//                   produce ASS dialogue events; this turns one into plain lines for drawing.
#pragma once

#include "wnk/media/media.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wnk::media {

// "ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text" (FFmpeg's AVSubtitleRect::ass) →
// plain text: override blocks {...} and drawings (\p1 ... \p0) removed, \N and \n become line
// breaks, \h a space, blank lines and surrounding spaces dropped.
std::string ass_event_text(const std::string &event);

class SubtitleStore {
 public:
    struct Limits {
        size_t max_cues_per_track = 5000;
        size_t max_bitmap_bytes = 48u << 20;
        int64_t open_end_us = 10000000;  // a cue with no end and no successor shows at most this long
    };
    SubtitleStore() : SubtitleStore(Limits{}) {}
    explicit SubtitleStore(Limits limits) : limits_(limits) {}

    // False if the same cue (track, start, content) is already stored.
    bool add(std::shared_ptr<const SubtitleCue> cue);
    // Cues of `track` showing at `pos_us`, earliest first. Clear cues are never returned.
    std::vector<std::shared_ptr<const SubtitleCue>> active(int track, int64_t pos_us) const;
    void clear();
    size_t count(int track) const;
    size_t bitmap_bytes() const;

 private:
    struct Entry {
        std::shared_ptr<const SubtitleCue> cue;
        uint64_t hash;
        size_t bytes;
    };
    void evict_locked();

    Limits limits_;
    mutable std::mutex mutex_;
    std::map<int, std::vector<Entry>> tracks_;  // sorted by start
    size_t bitmap_bytes_ = 0;
    mutable int64_t last_pos_us_ = 0;  // eviction keeps what is near the last query
};

}  // namespace wnk::media
