// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/media.hpp"
#include "wnk/media/pts_splice.hpp"

#include <algorithm>

namespace wnk::media {

const char *codec_name(CodecId c) {
    switch (c) {
        case CodecId::Unknown: return "unknown";
        case CodecId::H264: return "h264";
        case CodecId::Hevc: return "hevc";
        case CodecId::Vp9: return "vp9";
        case CodecId::Av1: return "av1";
        case CodecId::Aac: return "aac";
        case CodecId::Ac3: return "ac3";
        case CodecId::Eac3: return "eac3";
        case CodecId::Mp3: return "mp3";
        case CodecId::Opus: return "opus";
        case CodecId::PcmS16: return "pcm_s16le";
    }
    return "?";
}

bool is_video(CodecId c) { return c == CodecId::H264 || c == CodecId::Hevc || c == CodecId::Vp9 || c == CodecId::Av1; }

const char *track_type_name(TrackType t) {
    switch (t) {
        case TrackType::Video: return "video";
        case TrackType::Audio: return "audio";
        case TrackType::Subtitle: return "subtitle";
        case TrackType::Other: return "other";
    }
    return "?";
}

const char *backend_event_name(BackendEvent e) {
    switch (e) {
        case BackendEvent::Loaded: return "loaded";
        case BackendEvent::Playing: return "playing";
        case BackendEvent::EndOfStream: return "end-of-stream";
        case BackendEvent::ResourceLost: return "resource-lost";
        case BackendEvent::Error: return "error";
    }
    return "?";
}

const TrackInfo *MediaInfo::track(int index) const {
    auto it = std::find_if(tracks.begin(), tracks.end(), [index](const TrackInfo &t) { return t.index == index; });
    return it == tracks.end() ? nullptr : &*it;
}

const TrackInfo *MediaInfo::best(TrackType type) const {
    const TrackInfo *first = nullptr;
    for (const auto &t : tracks) {
        if (t.type != type) continue;
        if (t.is_default) return &t;
        if (first == nullptr) first = &t;
    }
    return first;
}

bool BackendCaps::accepts(CodecId c) const {
    const auto &list = is_video(c) ? video_codecs : audio_codecs;
    return std::find(list.begin(), list.end(), c) != list.end();
}

AudioPath audio_path(const BackendCaps &caps, const TrackInfo &t) {
    if (t.codec != CodecId::Unknown && caps.accepts(t.codec)) return AudioPath::Passthrough;
    if (t.decodable && caps.accepts(CodecId::PcmS16)) return AudioPath::Pcm;
    if (t.ac3_encodable && caps.accepts(CodecId::Ac3)) return AudioPath::Ac3;
    return AudioPath::None;
}

const char *audio_path_name(AudioPath p) {
    switch (p) {
        case AudioPath::None: return "none";
        case AudioPath::Passthrough: return "passthrough";
        case AudioPath::Pcm: return "pcm";
        case AudioPath::Ac3: return "ac3";
    }
    return "?";
}

bool adts_header(const std::vector<uint8_t> &asc, size_t payload_size, uint8_t out[7]) {
    if (asc.size() < 2) return false;
    uint32_t bits = 0;
    int have = 0;
    size_t pos = 0;
    auto take = [&](int n) -> int {
        while (have < n) {
            bits = (bits << 8) | (pos < asc.size() ? asc[pos] : 0);
            ++pos;
            have += 8;
        }
        have -= n;
        return static_cast<int>((bits >> have) & ((1u << n) - 1));
    };
    int object = take(5);
    int freq = take(4);
    if (freq == 15) return false;  // explicit rate: no ADTS index
    int channels = take(4);
    if (object == 5 || object == 29) {  // SBR/PS: the core follows
        const int ext = take(4);
        if (ext == 15) take(24);
        object = take(5);
    }
    if (object < 1 || object > 4 || channels > 7) return false;
    const size_t len = payload_size + 7;
    if (len >= (1u << 13)) return false;
    out[0] = 0xFF;
    out[1] = 0xF1;  // MPEG-4, layer 0, no CRC
    out[2] = static_cast<uint8_t>(((object - 1) << 6) | (freq << 2) | ((channels >> 2) & 1));
    out[3] = static_cast<uint8_t>(((channels & 3) << 6) | ((len >> 11) & 3));
    out[4] = static_cast<uint8_t>((len >> 3) & 0xFF);
    out[5] = static_cast<uint8_t>(((len & 7) << 5) | 0x1F);
    out[6] = 0xFC;
    return true;
}

int64_t PtsSplice::map(int64_t pts_us) {
    if (pts_us == kNoTime) return kNoTime;
    if (pending_ || segments_.empty()) {
        if (pending_) shift_us_ = (max_out_us_ == kNoTime ? pts_us : max_out_us_ + gap_us_) - pts_us;
        pending_ = false;
        segments_.push_back({pts_us + shift_us_, shift_us_});
        if (segments_.size() > 8) segments_.erase(segments_.begin());
    }
    const int64_t out = pts_us + shift_us_;
    max_out_us_ = max_out_us_ == kNoTime ? out : std::max(max_out_us_, out);
    return out;
}

int64_t PtsSplice::unmap(int64_t out_us) const {
    if (out_us == kNoTime) return kNoTime;
    for (auto it = segments_.rbegin(); it != segments_.rend(); ++it) {
        if (out_us >= it->out_start) return out_us - it->shift;
    }
    return segments_.empty() ? out_us : out_us - segments_.front().shift;
}

}  // namespace wnk::media
