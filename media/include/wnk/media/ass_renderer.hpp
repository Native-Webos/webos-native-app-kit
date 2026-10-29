// SPDX-License-Identifier: GPL-3.0-or-later
//
// AssRenderer: styled ASS/SSA subtitles through the kit's static libass (cmake/LibAss.cmake): fonts,
// colours, outlines, shadows, positioning, rotation, karaoke and drawings as the script describes them.
//
//   set_track(header)   a new script: its [Script Info] and [V4+ Styles] (TrackInfo::ass_header)
//   add_font            fonts the media carries (MediaInfo::fonts), before set_track
//   add_events(cue)     the cue's events (SubtitleCue::ass_events); an event already added (same
//                       ReadOrder) is ignored by libass, so cues can be offered again after a seek
//   render(t, w, h)     the frame at time t for a w x h video, as a few RGBA regions (straight alpha)
//                       covering what is drawn; `changed` is false when it equals the last one
//
// Without a font provider (no fontconfig on the TV) a family the media does not carry is drawn in the
// default font. Built without libass (WNK_LIBASS=OFF), create() returns null and callers draw the plain
// text instead. Not thread-safe: one thread (the UI's) uses it.
#pragma once

#include "wnk/media/media.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wnk::media {

class AssRenderer {
 public:
    struct Region {
        int x = 0, y = 0, width = 0, height = 0;  // in the w x h frame given to render()
        std::vector<uint8_t> rgba;               // width*height*4, straight alpha
    };
    struct Stats {
        uint64_t frames = 0;           // render() calls that produced a new frame
        double last_ms = 0, max_ms = 0;  // libass plus compositing, per new frame
        size_t events = 0;             // in the current track
    };

    // Null without libass, or if libass fails to start (`error` says why). `default_font` is a font
    // file used for families the media does not carry; empty: libass's own choice (often nothing).
    static std::unique_ptr<AssRenderer> create(const std::string &default_font, std::string *error);
    static bool available();  // built with libass
    virtual ~AssRenderer() = default;

    virtual void add_font(const std::string &name, const std::vector<uint8_t> &data) = 0;
    virtual bool set_track(const std::string &header) = 0;
    virtual void add_events(const SubtitleCue &cue) = 0;
    // Drops events that ended before `before_us` (memory for long films).
    virtual void prune(int64_t before_us) = 0;
    // `storage_w` x `storage_h`: the video's coded size (for aspect, blur and scaling as authored).
    virtual bool render(int64_t time_us, int width, int height, int storage_w, int storage_h,
                        std::vector<Region> *regions, bool *changed) = 0;
    virtual Stats stats() const = 0;
};

}  // namespace wnk::media
