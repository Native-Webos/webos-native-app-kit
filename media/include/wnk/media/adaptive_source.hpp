// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaSource for DASH (.mpd) and HLS (.m3u8) presentations over HTTP(S), on the bundled FFmpeg.
//
//   open()    fetches the manifest (wnk/media/manifest.hpp), refuses fully encrypted presentations,
//             chooses one video rendition (AdaptiveSourceOptions: codecs in order of
//             preference, size, frame-rate and bandwidth limits, Dolby Vision or not) and lists the
//             audio choices as tracks: track 0 is the video, then one track per audio rendition (DASH:
//             the best Representation of each AdaptationSet; HLS: every audio rendition, or the audio
//             muxed into the variant). HLS media playlists and DASH segment indexes (sidx) are
//             fetched when a rendition is first used.
//   select()  opens one FfmpegSource per rendition on a SegmentReader (initialisation segment, then
//             the media segments in order, prefetched) and merges their packets by timestamp; audio
//             goes through FfmpegSource's own rules (passthrough, PCM or AC-3 re-encoding).
//   bitrate   with adaptive_bitrate, the video rendition follows the measured throughput within a
//             ladder of renditions that can replace each other (same codec and Dolby Vision status;
//             DASH: same AdaptationSet; HLS: same audio arrangement; other sizes only when the
//             backend has adaptive_resolution). A switch ends the current rendition's reader after
//             the segment it is downloading, so its demuxer meets a clean end exactly at a segment
//             boundary, and the next rendition, already prefetching from there, takes over.
//             DASH audio does the same within its AdaptationSet's Representations of one codec,
//             channel count and sample rate, over what the video's bitrate leaves of the estimate
//             (and of the limit), so the backend's audio format never changes.
//   seek()    reopens each rendition at the segment holding the target (the audio one at or before the
//             video's), so the first video packet is the segment's keyframe; nothing before the
//             target is downloaded.
//
//   subtitles text tracks of the manifest are offered as Subtitle tracks after the audio ones: HLS
//             WebVTT playlists (cue times placed through X-TIMESTAMP-MAP), DASH text/vtt and
//             application/ttml+xml files, and WebVTT or TTML in MP4 (wvtt, stpp; parsed by the kit:
//             FFmpeg reads neither). After select() a background thread fetches every one of them
//             (the default first) and read() hands their cues out ahead of the media; the player's
//             subtitle store keeps them across seeks.
//
// Timestamps are presentation time: DASH segments map through presentationTimeOffset and the Period
// start; HLS renditions share the timeline of the first segment of the video rendition.
//
//   live      DASH type="dynamic" (read against the server's clock when the MPD names a UTCTiming
//             source) and HLS without EXT-X-ENDLIST. Playback joins a few segments behind the newest
//             (HLS: three target durations; DASH: suggestedPresentationDelay, else three segments) on
//             a timeline that starts at the window's first segment; duration is unknown and seeks are
//             clamped to what the server still lists. A refresher thread reloads the playlists in use
//             (HLS: every target duration) or the MPD and appends new segments, by sequence number, to
//             the open-ended readers of every rendition in use, including a bitrate switch in progress.
//             DASH: every segment duration it reads the last MPD again against the clock (a
//             SegmentTemplate@duration makes segments available by time, not with a new MPD) and
//             fetches the MPD itself every minimumUpdatePeriod; it follows every rendition the bitrate
//             may switch to, so a switch lands on the segment it was decided at. HLS ENDLIST or an MPD
//             turned static ends the stream.
//             Live subtitles are followed too: from shortly before the join point, each list's
//             new segments as it grows (HLS: the loader reloads the subtitle playlists; DASH: the
//             refresher merges the subtitle renditions with the MPD).
//
// Several Periods (static DASH): each lane plays its rendition's continuation (Rendition::next_period)
// when a Period ends, and a seek starts in the Period holding the target. Bitrate switching and
// subtitles follow the first Period's renditions only.
// WebM renditions (VP9, Opus) play like fMP4 ones; with SegmentBase their Cues are the index.
// HLS audio in groups per variant follows the video's variant into its group (the same language,
// codec and channels), and the copies in each group are one audio track.
// Not yet: TTML in HLS (IMSC in fMP4), encrypted renditions without ClearKey keys
// (refused), hierarchical sidx indexes.
#pragma once

#include "wnk/media/clearkey.hpp"
#include "wnk/media/manifest.hpp"
#include "wnk/media/media.hpp"
#include "wnk/net/http.hpp"
#include "wnk/net/segment_reader.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
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
    bool adaptive_bitrate = true;             // switch video (and DASH audio) renditions with the measured throughput
    int64_t initial_bandwidth = 6000000;      // assumed before the first measurement (bits per second)
    // Media downloaded ahead of the demuxer, per rendition (rounded up to whole segments, at least
    // two). A bitrate switch plays this much of the old rendition out first.
    int64_t max_ahead_us = 12000000;
    // ClearKey (wnk/media/clearkey.hpp): keys for encrypted renditions. Key IDs the manifest names
    // without a key here are asked from `license_url`, else from the manifest's ClearKey licence URL,
    // unless fetch_licenses is off. Encrypted renditions without their keys are left out.
    ContentKeys keys;
    std::string license_url;
    net::Headers license_headers;  // sent with licence requests only (an Authorization header)
    bool fetch_licenses = true;
};

// Network throughput from completed downloads: two exponentially weighted averages over transfer
// time (half-lives 3 s and 10 s, bias-corrected while young), the lower of the two, as Shaka Player
// does: quick to fall, slow to trust a burst. Downloads under 16 KiB say more about latency than
// throughput and are ignored. Thread-safe.
class ThroughputEstimator {
 public:
    void add(int64_t bytes, double seconds);
    int64_t estimate_bps() const;  // 0 until 128 KiB have been measured
    int samples() const;

 private:
    mutable std::mutex m_;
    double fast_ = 0, slow_ = 0, fast_weight_ = 0, slow_weight_ = 0;
    int64_t bytes_ = 0;
    int samples_ = 0;
};

// The rung of a ladder (bandwidths ascending) to play next from `current`: up only to a rung whose
// bandwidth is at most 70 % of the estimate, down when the current one exceeds 85 % of it (to the
// highest that does not), never above `limit_bps` (0: no limit). Without an estimate: `current`.
int choose_rung(const std::vector<int64_t> &bandwidths, int current, int64_t estimate_bps, int64_t limit_bps);

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
    // Any thread: caps the video bandwidth (bits per second, 0: none), e.g. a data-saver setting;
    // takes effect at the next segment boundary.
    void set_bandwidth_limit(int64_t bps);
    int64_t throughput_estimate() const;  // bits per second, 0 while unknown
    int switches() const;                 // bitrate switches since open(), video and audio
    int audio_switches() const;           // of those, audio ones
    // Seek previews (thumbnails.hpp) after open(): the track whose tiles are closest to `want_width`
    // pixels wide without going over. DASH tracks come with the MPD; an HLS image playlist is fetched
    // here, so call it from a worker. False (with a reason) when there are none, the presentation is
    // live, or the fetch fails. Any thread.
    bool thumbnails(int want_width, ThumbnailTrack *out, const async::CancellationToken &token, std::string *error) const;

    struct Impl;

 private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace wnk::media
