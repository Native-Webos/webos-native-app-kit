// SPDX-License-Identifier: GPL-3.0-or-later
//
// Seek previews: small pictures of the media at a time, for a scrub bar. Streams publish them as
// sheets of tiles (a JPEG holding a grid of frames, each tile showing a stretch of time):
//   - DASH: an image AdaptationSet with EssentialProperty "http://dashif.org/thumbnail_tile" (DASH-IF
//     IOP 6.2.6), parsed into Presentation::thumbnails;
//   - HLS: EXT-X-IMAGE-STREAM-INF image playlists with EXT-X-TILES (Apple / Roku image media
//     playlists), listed in Presentation::image_playlists and read with parse_hls_image_playlist;
//   - a WebVTT file whose cues name an image and a region ("sheet.jpg#xywh=0,0,160,90", the form
//     JW Player and many sites use), for plain files: parse_thumbnail_vtt.
// at(t) gives the image URL and the region of its tile in the image's own pixels: load the image at
// its original size (ui::ImageView::set_region) and show that region.
#pragma once

#include "wnk/media/media.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wnk::media {

struct Thumbnail {
    std::string url;
    int x = 0, y = 0, width = 0, height = 0;  // in the image's pixels; width 0: the whole image
    int64_t start_us = 0, end_us = 0;         // the stretch of media time it stands for
};

struct ThumbnailTrack {
    // One image: `columns` x `rows` tiles of tile_width x tile_height, left to right then top to
    // bottom, each showing tile_duration_us of [start_us, start_us + duration_us). A WebVTT cue is a
    // sheet of one tile at (x, y).
    struct Sheet {
        std::string url;
        int64_t start_us = 0, duration_us = 0;
        int columns = 1, rows = 1;
        int tile_width = 0, tile_height = 0;  // 0: the whole image is the tile
        int64_t tile_duration_us = 0;         // 0: duration_us / (columns * rows)
        int x = 0, y = 0;                     // the first tile's corner
    };
    std::vector<Sheet> sheets;  // start ascending
    int tile_width = 0, tile_height = 0;  // as declared (for choosing a track)
    std::string id;                       // DASH Representation id, HLS playlist URL, or the file URL

    bool empty() const { return sheets.empty(); }
    // The tile for media time `t`, nullopt before the first sheet or after the last.
    std::optional<Thumbnail> at(int64_t t) const;
};

// "10x1" → columns 10, rows 1; false if malformed.
bool parse_tile_layout(const std::string &text, int *columns, int *rows);
// A WebVTT thumbnails file at `url` (relative image URLs resolve against it).
bool parse_thumbnail_vtt(const std::string &text, const std::string &url, ThumbnailTrack *out, std::string *error);
// An HLS image media playlist at `url`.
bool parse_hls_image_playlist(const std::string &text, const std::string &url, ThumbnailTrack *out, std::string *error);

// The track closest to `want_width` tiles without going over it (else the smallest), or -1.
int choose_thumbnail_track(const std::vector<ThumbnailTrack> &tracks, int want_width);

}  // namespace wnk::media
