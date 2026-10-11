// SPDX-License-Identifier: GPL-3.0-or-later
//
// Provider-independent media model (docs/research/NATIVE-MEDIA.md, "Kit implications"):
//
//   MediaSource (demuxer: URI → tracks → elementary-stream packets)
//        │  read() on the playback thread
//        ▼
//   PlaybackController (state machine, feeding, seek, pause, position)   ── wnk/media/controller.hpp
//        │  feed() with backpressure
//        ▼
//   MediaBackend (hardware decoder + video plane: NDL on webOS 5+)
//
// Every native decoder on webOS takes elementary streams, so the source delivers H.264/HEVC as
// Annex-B access units (start codes, parameter sets before each keyframe) and audio either as codec
// frames the backend accepts or as decoded PCM (NDL v2 accepts PCM, MP3 and Opus only; AAC and AC-3
// are decoded in software). Timestamps are microseconds of content time throughout; a backend
// converts to whatever unit its decoder wants.
//
// Threading: a MediaSource and a MediaBackend are driven from the controller's playback thread.
// MediaSource::interrupt(), MediaBackend::set_area() and the backend's event callback are the only
// entry points used from other threads, and implementations must make those safe.
#pragma once

#include "wnk/geometry.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wnk::media {

inline constexpr int64_t kNoTime = INT64_MIN;

enum class CodecId : uint8_t { Unknown, H264, Hevc, Vp9, Av1, Aac, Ac3, Eac3, Mp3, Opus, PcmS16 };
const char *codec_name(CodecId c);
bool is_video(CodecId c);

enum class TrackType : uint8_t { Video, Audio, Subtitle, Other };
const char *track_type_name(TrackType t);

struct TrackInfo {
    int index = -1;  // the source's stream index
    TrackType type = TrackType::Other;
    CodecId codec = CodecId::Unknown;
    std::string codec_label;  // the demuxer's own name for it (also for unknown codecs)
    std::string language;
    std::string title;
    bool is_default = false;
    int width = 0, height = 0;  // video
    double frame_rate = 0;
    int sample_rate = 0, channels = 0;  // audio
};

struct MediaInfo {
    std::string uri;
    std::string container;
    int64_t duration_us = kNoTime;
    bool seekable = false;
    std::vector<TrackInfo> tracks;

    const TrackInfo *track(int index) const;
    // First default track of the type, else the first of the type; nullptr if none.
    const TrackInfo *best(TrackType type) const;
};

// What the backend is asked to load: the selected tracks in the form the source will deliver them.
struct VideoFormat {
    CodecId codec = CodecId::Unknown;
    int width = 0, height = 0;
    double frame_rate = 0;
};
struct AudioFormat {
    CodecId codec = CodecId::Unknown;  // PcmS16 when the source decodes in software
    int sample_rate = 0;
    int channels = 0;                  // PCM is delivered as interleaved stereo
    std::vector<uint8_t> codec_header;  // e.g. the OpusHead for Opus
};
struct StreamFormat {
    std::optional<VideoFormat> video;
    std::optional<AudioFormat> audio;
};

struct Packet {
    TrackType type = TrackType::Video;
    std::vector<uint8_t> data;
    int64_t pts_us = kNoTime;
    int64_t duration_us = 0;
    bool key = false;
};

// Audio codecs a backend can take directly; the source decodes everything else to PcmS16.
struct BackendCaps {
    std::vector<CodecId> video_codecs;
    std::vector<CodecId> audio_codecs;
    bool pause = false;    // can hold playback without being re-fed (NDL v2 cannot)
    // With pause: the backend cannot stop presenting what it already holds, so the picture stops
    // only after the queued packets have played out, at the last PTS fed.
    bool pause_drains = false;
    // flush() drops nothing: packets after a seek are queued behind what the decoder still holds,
    // separated by splice_gap_us, so the new content shows once that has played out.
    bool seek_splices = false;
    int64_t splice_gap_us = 0;
    bool position = false;  // reports the presented position
    bool accepts(CodecId c) const;
};

class MediaSource {
 public:
    enum class Read : uint8_t { Packet, EndOfStream, Error, Interrupted };

    virtual ~MediaSource() = default;
    // Blocking. `error` gets a short reason on failure.
    virtual bool open(const std::string &uri, std::string *error) = 0;
    virtual const MediaInfo &info() const = 0;
    // Chooses the tracks to deliver (-1: none of that type) and how: audio codecs the backend does
    // not accept are decoded to PCM S16 stereo. Returns false if a selected track cannot be delivered.
    virtual bool select(int video_track, int audio_track, const BackendCaps &caps, StreamFormat *out,
                        std::string *error) = 0;
    // Next packet of a selected track. Blocking.
    virtual Read read(Packet *out, std::string *error) = 0;
    // Repositions to the keyframe at or before `pts_us`. The next video packet read is a keyframe.
    virtual bool seek(int64_t pts_us, std::string *error) = 0;
    // Any thread: makes a blocked open/read return Interrupted (and later calls, until reset).
    virtual void interrupt() = 0;
    virtual void reset_interrupt() = 0;
};

enum class BackendEvent : uint8_t {
    Loaded,       // ready for packets
    Playing,      // first frame presented (if the backend can tell)
    EndOfStream,  // everything fed has been presented (if the backend can tell)
    ResourceLost,  // the decoder was taken away (another app, background)
    Error,
};
const char *backend_event_name(BackendEvent e);

class MediaBackend {
 public:
    enum class Feed : uint8_t { Accepted, Full, Error };
    using EventFn = std::function<void(BackendEvent, const std::string &detail)>;

    virtual ~MediaBackend() = default;
    virtual const char *name() const = 0;
    virtual BackendCaps caps() const = 0;
    // May be called from any thread; events may arrive on any thread (the controller handles that).
    virtual void set_event_callback(EventFn fn) = 0;
    virtual bool load(const StreamFormat &format, std::string *error) = 0;
    // Full: try the same packet again later (decoder buffer full). Never blocks for long.
    virtual Feed feed(const Packet &p) = 0;
    // Drops everything queued in the decoder (for seeking). May be implemented as unload + load.
    virtual bool flush(std::string *error) = 0;
    virtual void unload() = 0;
    // Any thread: where the video goes, in drawable pixels (full screen by default).
    virtual void set_area(const Rect &pixels) = 0;
    // Presented position, if caps().position.
    virtual int64_t position_us() const { return kNoTime; }
    // Hold/continue presentation, if caps().pause.
    virtual bool pause(bool) { return false; }
};

}  // namespace wnk::media
