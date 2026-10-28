// SPDX-License-Identifier: GPL-3.0-or-later
//
// AutoBackend: a preferred backend when it takes the media's codecs as they are, a fallback
// otherwise, chosen per media in prepare() (after the source is open, before tracks are selected).
//
// The rule: the preferred backend is used when it accepts the video codec (if there is video) and
// can be given the audio (if there is audio) in some way of audio_path(): as it is, or re-encoded to
// AC-3 by the source. On webOS that is Starfish for H.264/HEVC with any audio the source can decode,
// which plays 24p without judder and has a real pause, and NDL for the rest (AV1, VP9, and audio the
// source cannot decode). Every other call
// goes to the chosen backend; before prepare() the preferred one answers.
#pragma once

#include "wnk/media/media.hpp"

#include <atomic>
#include <deque>
#include <optional>
#include <memory>
#include <string>

namespace wnk::media {

class AutoBackend : public MediaBackend {
 public:
    AutoBackend(std::unique_ptr<MediaBackend> preferred, std::unique_ptr<MediaBackend> fallback);

    const char *name() const override { return active().name(); }
    BackendCaps caps() const override { return active().caps(); }
    void prepare(const MediaInfo &info, const TrackInfo *video, const TrackInfo *audio) override;
    void set_event_callback(EventFn fn) override;
    bool load(const StreamFormat &format, std::string *error) override { return active().load(format, error); }
    Feed feed(const Packet &p) override { return active().feed(p); }
    bool flush(std::string *error) override { return active().flush(error); }
    void unload() override;
    void set_area(const Rect &pixels) override;
    int64_t position_us() const override { return active().position_us(); }
    bool pause(bool paused) override { return active().pause(paused); }
    void end_of_stream() override { active().end_of_stream(); }
    bool splice_settled() const override { return active().splice_settled(); }
    // The preferred backend refused this media before its first picture: the fallback takes it, now
    // and whenever the same URI is prepared again (PlxNative learns refusals the same way).
    bool fall_back(const std::string &why) override;

    // Whether `b` can be given this media's tracks (exposed for tests and logs).
    static bool takes(const MediaBackend &b, const TrackInfo *video, const TrackInfo *audio, std::string *why);
    bool using_fallback() const { return use_fallback_.load(); }

 private:
    MediaBackend &active() const { return use_fallback_.load() ? *fallback_ : *preferred_; }
    std::unique_ptr<MediaBackend> preferred_, fallback_;
    std::atomic<bool> use_fallback_{false};  // written at prepare (playback thread), read anywhere
    // Playback thread: the media prepared last and the URIs the preferred backend refused.
    std::string uri_;
    std::optional<TrackInfo> video_, audio_;
    std::deque<std::string> refused_;
};

}  // namespace wnk::media
