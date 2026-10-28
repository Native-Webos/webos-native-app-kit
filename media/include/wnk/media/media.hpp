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
// frames the backend accepts, as decoded PCM (NDL v2 accepts PCM, MP3 and Opus only; AAC and AC-3
// are decoded in software), or re-encoded to AC-3 for a backend that takes no PCM (Starfish). Timestamps are microseconds of content time throughout; a backend
// converts to whatever unit its decoder wants.
//
// Threading: a MediaSource and a MediaBackend are driven from the controller's playback thread.
// MediaSource::interrupt(), MediaBackend::set_area() and the backend's event callback are the only
// entry points used from other threads, and implementations must make those safe.
#pragma once

#include "wnk/geometry.hpp"

#include <cstdint>
#include <functional>
#include <memory>
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

// How a subtitle track is drawn: text the app lays out, or bitmaps (PGS, DVD, DVB) it scales.
enum class SubtitleKind : uint8_t { None, Text, Bitmap };

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
    bool decodable = false;      // audio the source can decode to PCM
    bool ac3_encodable = false;  // audio the source can decode and re-encode to AC-3
    SubtitleKind subtitle = SubtitleKind::None;  // subtitle tracks the source can decode
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

// The colour description and static HDR metadata the container states (H.273 code points, 2 meaning
// unspecified). Values carried only in the bitstream (HEVC SEI) are not here: they reach the decoder
// with the stream itself.
struct VideoColour {
    int transfer = 2, primaries = 2, matrix = 2;  // e.g. 16 PQ, 18 HLG; 9 BT.2020; 9 BT.2020 NCL
    bool full_range = false;
    bool has_primaries = false;
    double display_primaries[3][2] = {};  // mastering display R, G, B as CIE 1931 (x, y)
    double white_point[2] = {};
    bool has_luminance = false;
    double min_luminance = 0, max_luminance = 0;  // cd/m2
    bool has_light_level = false;
    unsigned max_cll = 0, max_fall = 0;  // cd/m2
};

// What the backend is asked to load: the selected tracks in the form the source will deliver them.
struct VideoFormat {
    CodecId codec = CodecId::Unknown;
    int width = 0, height = 0;
    double frame_rate = 0;
    // Single-layer Dolby Vision carried in the stream (RPUs in-band): its profile (5, 8, ...), else -1.
    // Dual-layer streams (an enhancement layer, profile 7) report -1: only the base layer is fed.
    int dolby_vision_profile = -1;
    VideoColour colour{};
};
struct AudioFormat {
    CodecId codec = CodecId::Unknown;  // PcmS16 when the source decodes in software, Ac3 when it re-encodes
    int sample_rate = 0;
    int channels = 0;                  // PCM is delivered as interleaved stereo; re-encoded AC-3 has up to 5.1
    std::vector<uint8_t> codec_header;  // e.g. the OpusHead for Opus
    bool atmos = false;  // E-AC-3 with Atmos objects (JOC), delivered as it is
};
struct StreamFormat {
    std::optional<VideoFormat> video;
    std::optional<AudioFormat> audio;
};

// One bitmap of a subtitle cue, positioned on the cue's canvas.
struct SubtitleBitmap {
    int x = 0, y = 0, width = 0, height = 0;
    std::vector<uint8_t> rgba;  // width*height*4, straight (not premultiplied) alpha
};

// What one subtitle track shows from start_us until end_us. Text cues carry plain UTF-8 with '\n'
// between lines (styling tags removed); bitmap cues carry images on a canvas (usually the video's
// size). A cue with no text and no bitmaps clears the track (PGS "clear" events).
struct SubtitleCue {
    int track = -1;                 // the source's stream index
    int64_t start_us = kNoTime;
    int64_t end_us = kNoTime;       // kNoTime: until the next cue of the track
    std::string text;
    std::vector<SubtitleBitmap> bitmaps;
    int canvas_w = 0, canvas_h = 0;
    bool empty() const { return text.empty() && bitmaps.empty(); }
};

struct Packet {
    TrackType type = TrackType::Video;
    std::vector<uint8_t> data;
    int64_t pts_us = kNoTime;
    int64_t dts_us = kNoTime;  // decode time, where it differs from pts (video with B-frames)
    int64_t duration_us = 0;
    bool key = false;
    std::shared_ptr<const SubtitleCue> subtitle;  // type == Subtitle: the decoded cue (data unused)
};

// A 7-byte ADTS header (no CRC) for one AAC frame of `payload_size` bytes, from the stream's
// AudioSpecificConfig (MP4/MKV extradata). HE-AAC configs give the core profile and rate, as ADTS
// signals SBR implicitly. False if the config cannot be expressed in ADTS.
bool adts_header(const std::vector<uint8_t> &audio_specific_config, size_t payload_size, uint8_t out[7]);

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
    bool aac_adts = false;  // AAC passed through must carry ADTS headers (Starfish)
    // The decoder follows a change of picture size mid-stream (new parameter sets at a keyframe), so
    // an adaptive source may switch between renditions of different resolutions.
    bool adaptive_resolution = false;
    bool accepts(CodecId c) const;
};

// How an audio track reaches a backend: as it is, decoded to PCM, or decoded and re-encoded to AC-3,
// in that order of preference; None if the backend can take it in no way. The source applies this in
// select() and AutoBackend uses it to choose a backend.
enum class AudioPath : uint8_t { None, Passthrough, Pcm, Ac3 };
AudioPath audio_path(const BackendCaps &caps, const TrackInfo &track);
const char *audio_path_name(AudioPath p);

class MediaSource {
 public:
    enum class Read : uint8_t { Packet, EndOfStream, Error, Interrupted };

    virtual ~MediaSource() = default;
    // Blocking. `error` gets a short reason on failure.
    virtual bool open(const std::string &uri, std::string *error) = 0;
    virtual const MediaInfo &info() const = 0;
    // Chooses the tracks to deliver (-1: none of that type) and how: audio goes by audio_path(), so
    // codecs the backend does not accept are decoded to PCM S16 stereo or re-encoded to AC-3. Returns false if a selected track cannot be delivered.
    // Every subtitle track the source can decode (TrackInfo::subtitle) is delivered as well, as
    // Subtitle packets carrying cues, so the viewer can switch between them without a seek.
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
    // Called after the source is open and before tracks are selected (and caps() consulted for
    // them), so a backend that wraps others can choose one for this media.
    virtual void prepare(const MediaInfo &, const TrackInfo * /*video*/, const TrackInfo * /*audio*/) {}
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
    // Everything has been fed (until the next flush or load): let the decoder drain.
    virtual void end_of_stream() {}
    // Splicing backends: whether the decoder has reached the content fed after the last splice.
    // Another splice before that stacks one seek on another, which Starfish does not survive (every
    // frame dropped for the rest of playback, OLED65C1PTB), so the controller holds the next seek.
    virtual bool splice_settled() const { return true; }
    // This backend refused the media prepared last before showing a picture (a failed load, or an
    // Error event): switch to another decoder for it if there is one that takes its tracks. True
    // means the controller should open the media again; prepare() then keeps the other decoder.
    virtual bool fall_back(const std::string & /*why*/) { return false; }
};

}  // namespace wnk::media
