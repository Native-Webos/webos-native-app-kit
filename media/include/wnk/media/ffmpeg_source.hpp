// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaSource on the kit's bundled FFmpeg (tools/build-ffmpeg.sh): opens local files (file:// URIs
// or absolute paths) in Matroska/WebM, MP4/MOV, MPEG-TS, raw H.264/HEVC, AAC, MP3, Ogg or WAV and
// delivers the packet contract of wnk/media/media.hpp:
//
//   video  H.264/HEVC as Annex-B access units: MP4/MKV length-prefixed samples go through FFmpeg's
//          h264_mp4toannexb / hevc_mp4toannexb, which also insert the parameter sets before every
//          keyframe; streams that are already Annex-B (MPEG-TS, raw) pass through. VP9/AV1 as is.
//   audio  passed through when the backend accepts the codec; AAC, AC-3 and E-AC-3 otherwise
//          decoded to interleaved S16 stereo (more channels are downmixed: L = FL + 0.707·C +
//          0.707·SL, LFE dropped), at the stream's own sample rate.
//   time   microseconds from the start of the media (the container's start_time is subtracted, so
//          MPEG-TS clips start at 0 like the others).
//
// Network URIs are not opened here yet: the bundled FFmpeg has only the file protocol, and HTTP will
// come through the kit's own HTTP client.
#pragma once

#include "wnk/media/media.hpp"

#include <memory>

namespace wnk::media {

class FfmpegSource : public MediaSource {
 public:
    FfmpegSource();
    ~FfmpegSource() override;

    bool open(const std::string &uri, std::string *error) override;
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
    std::unique_ptr<Impl> impl_;
};

// Interleaves planar or packed float/S16/S32 samples into S16 stereo, downmixing more channels.
// Exposed for tests. `layout_channels` is the channel count; `order` gives the channel position of
// each input channel using FFmpeg's AV_CHAN_* numbering (FL=0, FR=1, FC=2, LFE=3, BL=4, BR=5,
// SL=9, SR=10), empty for "first two are left/right".
void mix_to_stereo_s16(const float *const *planes, int channels, int samples, const int *order, int16_t *out);

}  // namespace wnk::media
