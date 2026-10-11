// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaSource for DASH (.mpd) and HLS (.m3u8) presentations over HTTP(S), on the bundled FFmpeg.
//
//   open()    fetches the manifest (wnk/media/manifest.hpp), refuses live and fully encrypted
//             presentations, chooses one video rendition (AdaptiveSourceOptions: codecs in order of
//             preference, size, frame-rate and bandwidth limits, Dolby Vision or not) and lists the
//             audio choices as tracks: track 0 is the video, then one track per audio rendition (DASH:
//             the best Representation of each AdaptationSet; HLS: every audio rendition, or the audio
//             muxed into the variant). HLS media playlists and DASH segment indexes (sidx) are
//             fetched when a rendition is first used.
//   select()  opens one FfmpegSource per rendition on a SegmentReader (initialisation segment, then
//             the media segments in order, prefetched) and merges their packets by timestamp; audio
//             goes through FfmpegSource's own rules (passthrough, PCM or AC-3 re-encoding).
//   seek()    reopens each rendition at the segment holding the target (the audio one at or before the
//             video's), so the first video packet is the segment's keyframe; nothing before the
//             target is downloaded.
//
// Timestamps are presentation time: DASH segments map through presentationTimeOffset and the Period
// start; HLS renditions share the timeline of the first segment of the video rendition.
//
// Not yet: live presentations, bitrate switching during playback, subtitles from the manifest,
// encrypted renditions (refused), WebM renditions (left out), more than the first DASH Period.
#pragma once

#include "wnk/media/manifest.hpp"
#include "wnk/media/media.hpp"
#include "wnk/net/http.hpp"
#include "wnk/net/segment_reader.hpp"

#include <memory>
#include <string>
#include <vector>

namespace wnk::media {

struct AdaptiveSourceOptions {
    std::shared_ptr<net::HttpTransport> http;
    net::Headers headers;                     // sent with the manifest and segment requests
    net::SegmentReaderOptions segments;       // headers are taken from `headers`
    bool ac3_encode = true;                   // FfmpegSourceOptions::ac3_encode
    std::vector<CodecId> video_codecs = {CodecId::Hevc, CodecId::H264};  // usable, in order of preference
    int max_height = 2160;
    double max_frame_rate = 120;
    int64_t max_bandwidth = 0;                // bits per second; 0: no limit
    bool dolby_vision = false;                // prefer Dolby Vision renditions (else avoid them)
    std::string language;                     // preferred audio language ("de", "en-US"), else the default
};

// The renditions open() would choose, exposed for tests and logs. Indices into p.renditions; -1 none.
int choose_video(const Presentation &p, const AdaptiveSourceOptions &o);

class AdaptiveSource : public MediaSource {
 public:
    explicit AdaptiveSource(AdaptiveSourceOptions options);
    ~AdaptiveSource() override;

    // True for URIs that look like a manifest (.mpd/.m3u8 path, http(s) only).
    static bool handles(const std::string &uri);

    bool open(const std::string &uri, std::string *error) override;
    const MediaInfo &info() const override;
    bool select(int video_track, int audio_track, const BackendCaps &caps, StreamFormat *out,
                std::string *error) override;
    Read read(Packet *out, std::string *error) override;
    bool seek(int64_t pts_us, std::string *error) override;
    void interrupt() override;
    void reset_interrupt() override;

    const Presentation &presentation() const;
    // Rendition indices in use (after select), -1 if none.
    int video_rendition() const;
    int audio_rendition() const;  // -1 also when the audio is muxed into the video rendition

    struct Impl;

 private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace wnk::media
