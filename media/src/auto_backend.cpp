// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/auto_backend.hpp"

#include "wnk/log.hpp"

#include <algorithm>

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
    uri_ = info.uri;
    video_ = video != nullptr ? std::optional<TrackInfo>(*video) : std::nullopt;
    audio_ = audio != nullptr ? std::optional<TrackInfo>(*audio) : std::nullopt;
    std::string why;
    const bool refused = std::find(refused_.begin(), refused_.end(), info.uri) != refused_.end();
    if (refused && takes(*fallback_, video, audio, nullptr)) {
        use_fallback_ = true;
        WNK_LOGI("media", "%s: %s refused it before; using %s", info.uri.c_str(), preferred_->name(), fallback_->name());
        active().prepare(info, video, audio);
        return;
    }
    use_fallback_ = !takes(*preferred_, video, audio, &why);  // neither is loaded between media
    if (use_fallback_) {
        WNK_LOGI("media", "%s: %s cannot take %s; using %s", info.uri.c_str(), preferred_->name(), why.c_str(),
                 fallback_->name());
    } else {
        WNK_LOGI("media", "%s: using %s", info.uri.c_str(), preferred_->name());
    }
    active().prepare(info, video, audio);
}

bool AutoBackend::fall_back(const std::string &why) {
    if (use_fallback_ || uri_.empty()) return false;
    // Only a refusal before the first picture: after one, the stream is the preferred backend's.
    if (preferred_->position_us() != kNoTime) return false;
    std::string cannot;
    if (!takes(*fallback_, video_ ? &*video_ : nullptr, audio_ ? &*audio_ : nullptr, &cannot)) {
        WNK_LOGI("media", "%s refused %s (%s); %s cannot take %s either", preferred_->name(), uri_.c_str(), why.c_str(),
                 fallback_->name(), cannot.c_str());
        return false;
    }
    WNK_LOGW("media", "%s refused %s before its first picture (%s); trying %s", preferred_->name(), uri_.c_str(),
             why.c_str(), fallback_->name());
    refused_.push_back(uri_);
    if (refused_.size() > 32) refused_.pop_front();
    return true;
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
