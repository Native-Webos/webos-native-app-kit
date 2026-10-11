// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/media.hpp"

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

}  // namespace wnk::media
