// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaSource on the kit's bundled FFmpeg (tools/build-ffmpeg.sh): opens local files (file:// URIs
// or absolute paths) in Matroska/WebM, MP4/MOV, MPEG-TS, raw H.264/HEVC, AAC, MP3, Ogg or WAV and
// delivers the packet contract of wnk/media/media.hpp:
//
//   video  H.264/HEVC as Annex-B access units: MP4/MKV length-prefixed samples go through FFmpeg's
//          h264_mp4toannexb / hevc_mp4toannexb, which also insert the parameter sets before every
//          keyframe; streams that are already Annex-B (MPEG-TS, raw) pass through. VP9/AV1 as is.
//   audio  by audio_path(): passed through when the backend accepts the codec; otherwise, for every
//          codec this FFmpeg build decodes (AAC, AC-3, E-AC-3, MP3/MP2, Opus, Vorbis, FLAC, TrueHD,
//          DTS, PCM), decoded to interleaved S16 stereo for a backend that takes PCM (more channels
//          are downmixed: L = FL + 0.707·C + 0.707·SL, LFE dropped) at the stream's own sample rate,
//          or re-encoded to AC-3 for a backend that takes AC-3 but no PCM (Starfish): mono and
//          stereo stay so, more channels become 5.1, rates other than 32/44.1/48 kHz become 48 kHz,
//          one 1536-sample frame per packet.
//   time   microseconds from the start of the media (the container's start_time is subtracted, so
//          MPEG-TS clips start at 0 like the others).
//
// http:// and https:// URIs are read through the kit's HTTP client (wnk/net/http_stream.hpp: range
// requests, read-ahead, reconnects) behind a custom AVIOContext, so TLS and certificate rules are the
// kit's and the bundled FFmpeg needs no network protocols. That needs FfmpegSourceOptions::http; the
// stream is seekable when the server honours byte ranges.
#pragma once

#include "wnk/media/media.hpp"
#include "wnk/net/byte_reader.hpp"
#include "wnk/net/http_stream.hpp"

#include <memory>

namespace wnk::media {

struct FfmpegSourceOptions {
    std::shared_ptr<net::HttpTransport> http;  // for http(s) URIs; without it they fail to open
    net::HttpStreamOptions http_stream;
    bool ac3_encode = true;  // offer AudioPath::Ac3 (TrackInfo::ac3_encodable)
};

class FfmpegSource : public MediaSource {
 public:
    explicit FfmpegSource(FfmpegSourceOptions options = {});
    ~FfmpegSource() override;

    bool open(const std::string &uri, std::string *error) override;
    // Opens a stream read from `reader` (not seekable unless it says so); `name` hints the format
    // by its extension and becomes MediaInfo::uri.
    bool open_reader(std::unique_ptr<net::ByteReader> reader, const std::string &name, std::string *error);
    // The time subtracted from every timestamp, for the next open: the container's start time by
    // default (kNoTime). A stream opened partway through (adaptive streaming after a seek) passes the
    // origin of the first open so its times stay those of the presentation.
    void set_time_origin(int64_t origin_us);
    int64_t time_origin() const;  // in use since the last open
    const MediaInfo &info() const override;
    bool select(int video_track, int audio_track, const BackendCaps &caps, StreamFormat *out,
                std::string *error) override;
    Read read(Packet *out, std::string *error) override;
    bool seek(int64_t pts_us, std::string *error) override;
    void interrupt() override;
    void reset_interrupt() override;

    // "avformat 63.1.100 / avcodec 63.1.100" etc., for logs.
    static std::string version();

    struct Impl;

 private:
    bool open_input(const std::string &name, const std::string &uri, std::string *error);
    std::unique_ptr<Impl> impl_;
};

// Interleaves planar or packed float/S16/S32 samples into S16 stereo, downmixing more channels.
// Exposed for tests. `layout_channels` is the channel count; `order` gives the channel position of
// each input channel using FFmpeg's AV_CHAN_* numbering (FL=0, FR=1, FC=2, LFE=3, BL=4, BR=5,
// SL=9, SR=10), empty for "first two are left/right".
void mix_to_stereo_s16(const float *const *planes, int channels, int samples, const int *order, int16_t *out);

}  // namespace wnk::media
