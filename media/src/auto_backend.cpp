// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/auto_backend.hpp"

#include "wnk/log.hpp"

namespace wnk::media {

AutoBackend::AutoBackend(std::unique_ptr<MediaBackend> preferred, std::unique_ptr<MediaBackend> fallback)
    : preferred_(std::move(preferred)), fallback_(std::move(fallback)) {}

bool AutoBackend::takes(const MediaBackend &b, const TrackInfo *video, const TrackInfo *audio, std::string *why) {
    const BackendCaps c = b.caps();
    if (video != nullptr && !c.accepts(video->codec)) {
        if (why != nullptr) *why = "video " + video->codec_label;
        return false;
    }
    if (audio != nullptr && audio_path(c, *audio) == AudioPath::None) {
        if (why != nullptr) *why = "audio " + audio->codec_label;
        return false;
    }
    return true;
}

void AutoBackend::prepare(const MediaInfo &info, const TrackInfo *video, const TrackInfo *audio) {
    std::string why;
    use_fallback_ = !takes(*preferred_, video, audio, &why);  // neither is loaded between media
    if (use_fallback_) {
        WNK_LOGI("media", "%s: %s cannot take %s; using %s", info.uri.c_str(), preferred_->name(), why.c_str(),
                 fallback_->name());
    } else {
        WNK_LOGI("media", "%s: using %s", info.uri.c_str(), preferred_->name());
    }
    active().prepare(info, video, audio);
}

void AutoBackend::set_event_callback(EventFn fn) {
    preferred_->set_event_callback(fn);
    fallback_->set_event_callback(std::move(fn));
}

void AutoBackend::unload() {
    preferred_->unload();
    fallback_->unload();
}

void AutoBackend::set_area(const Rect &pixels) {
    preferred_->set_area(pixels);
    fallback_->set_area(pixels);
}

}  // namespace wnk::media
