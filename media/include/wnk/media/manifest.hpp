// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adaptive-streaming manifests reduced to one model: a Presentation is a set of Renditions (one
// encoding of one track: a DASH Representation, an HLS variant or alternative rendition), each with
// the segments a reader fetches in order. Everything here is pure (no I/O): the caller downloads the
// manifest, an HLS media playlist or a DASH segment index and hands the bytes in.
//
//   DASH (ISO/IEC 23009-1)  static MPDs. SegmentTemplate with $RepresentationID$, $Number$, $Time$,
//                           $Bandwidth$ (and %0Nd widths), by @duration or SegmentTimeline (with
//                           r = -1); SegmentList; SegmentBase with an indexRange, whose segments come
//                           from parse_sidx(); a lone BaseURL as one segment. BaseURLs nest
//                           MPD → Period → AdaptationSet → Representation; SegmentTemplate and
//                           SegmentBase attributes are inherited the same way. Only the first Period
//                           is read. ContentProtection marks a rendition encrypted.
//   HLS (RFC 8216)          master playlists (EXT-X-STREAM-INF variants, EXT-X-MEDIA audio and
//                           subtitle renditions) and media playlists (EXTINF, EXT-X-BYTERANGE,
//                           EXT-X-MAP, EXT-X-DISCONTINUITY); EXT-X-KEY other than NONE marks the
//                           rendition encrypted. A media playlist given directly becomes one muxed
//                           rendition.
//
// Live presentations (DASH type="dynamic", HLS without EXT-X-ENDLIST) are recognised and reported
// through Presentation::live; their segment lists are a snapshot.
#pragma once

#include "wnk/media/media.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wnk::media {

// Bytes [offset, offset + length) of a resource; length < 0 means to its end.
struct ByteRange {
    int64_t offset = 0;
    int64_t length = -1;
    bool whole() const { return offset == 0 && length < 0; }
};

struct MediaSegment {
    std::string url;
    ByteRange range;
    int64_t start_us = 0;     // presentation time from the start of the presentation
    int64_t duration_us = 0;
    bool discontinuity = false;  // HLS: timestamps may restart here
};

struct Rendition {
    std::string id;
    TrackType type = TrackType::Other;
    bool muxed_audio = false;  // video segments also carry the audio (HLS variant without audio renditions)
    std::string codecs;        // RFC 6381 list as declared ("avc1.640028,mp4a.40.2")
    std::string mime_type;
    int64_t bandwidth = 0;     // bits per second
    int width = 0, height = 0;
    double frame_rate = 0;
    int sample_rate = 0;
    int channels = 0;
    std::string language;
    std::string label;
    std::string group;         // DASH AdaptationSet id, HLS GROUP-ID of an alternative rendition
    std::string audio_group;   // HLS variant: the GROUP-ID of its audio renditions
    bool is_default = false;
    bool encrypted = false;
    // Media time minus presentation time (DASH: presentationTimeOffset less the Period start);
    // kNoTime when only the media can tell (HLS, where all renditions share one timeline).
    int64_t time_origin_us = kNoTime;
    std::string dolby_vision;  // the DV codec string ("dvhe.08.06") when the rendition is Dolby Vision
    bool atmos = false;        // E-AC-3 with Joint Object Coding

    std::optional<MediaSegment> init;  // initialisation segment (fMP4), if any
    std::vector<MediaSegment> segments;
    // DASH SegmentBase: the segments are described by the sidx box at `index` of `index_url`.
    std::string index_url;
    ByteRange index;
    // HLS: the media playlist that lists the segments (not loaded yet while `segments` is empty).
    std::string playlist_url;

    bool needs_index() const { return segments.empty() && !index_url.empty(); }
    bool needs_playlist() const { return segments.empty() && !playlist_url.empty(); }
};

struct Presentation {
    enum class Format : uint8_t { Dash, Hls };
    Format format = Format::Dash;
    std::string url;
    bool live = false;
    int64_t duration_us = kNoTime;
    std::vector<Rendition> renditions;
};

// RFC 3986 reference resolution ("../a.m4s" against "https://h/p/q/x.mpd" → "https://h/p/a.m4s").
std::string resolve_url(const std::string &base, const std::string &ref);

// ISO 8601 durations as used by DASH ("PT1H2M3.5S", "P1DT2H"); false if malformed.
bool parse_iso_duration(const std::string &s, int64_t *us);

// The kit's codec for the first entry of an RFC 6381 list of the given type (Video: avc1/avc3,
// hvc1/hev1/dvh1/dvhe, vp09, av01; Audio: mp4a, ac-3, ec-3, opus, mp3), Unknown if none.
CodecId codec_from_rfc6381(const std::string &codecs, TrackType type);

// The profile of a Dolby Vision codec string ("dvhe.08.06" → 8, "dvh1.05.06" → 5), else -1.
int dolby_vision_profile(const std::string &codec);

bool parse_mpd(const std::string &xml, const std::string &url, Presentation *out, std::string *error);

// A master playlist (renditions with playlist_url set) or a media playlist (one muxed rendition
// with its segments).
bool parse_hls(const std::string &text, const std::string &url, Presentation *out, std::string *error);

// Fills `r`'s segments (and init, encrypted) from its media playlist; `live` is set when the
// playlist has no EXT-X-ENDLIST. `url` is where the playlist was fetched from (after redirects).
bool parse_hls_media(const std::string &text, const std::string &url, Rendition *r, bool *live, std::string *error);

// Segments of a 'sidx' box (ISO/IEC 14496-12 8.16.3) read from `url` at byte `box_offset`.
// Hierarchical indexes (references to other sidx boxes) are refused.
bool parse_sidx(const uint8_t *data, size_t size, int64_t box_offset, const std::string &url,
                std::vector<MediaSegment> *out, std::string *error);

}  // namespace wnk::media
