// SPDX-License-Identifier: GPL-3.0-or-later
//
// AutoBackend: a preferred backend when it takes the media's codecs as they are, a fallback
// otherwise, chosen per media in prepare() (after the source is open, before tracks are selected).
//
// The rule: the preferred backend is used when it accepts the video codec (if there is video) and
// the audio codec directly (if there is audio). On webOS that is Starfish for H.264/HEVC with
// AAC/AC-3/E-AC-3, which plays 24p without judder and has a real pause, and NDL for the rest (AV1,
// VP9, and audio Starfish cannot take, which the source decodes to PCM for NDL). Every other call
// goes to the chosen backend; before prepare() the preferred one answers.
#pragma once

#include "wnk/media/media.hpp"

#include <atomic>
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

    // Whether `b` takes this media's codecs as they are (exposed for tests and logs).
    static bool takes(const MediaBackend &b, const TrackInfo *video, const TrackInfo *audio, std::string *why);
    bool using_fallback() const { return use_fallback_.load(); }

 private:
    MediaBackend &active() const { return use_fallback_.load() ? *fallback_ : *preferred_; }
    std::unique_ptr<MediaBackend> preferred_, fallback_;
    std::atomic<bool> use_fallback_{false};  // written at prepare (playback thread), read anywhere
};

}  // namespace wnk::media
