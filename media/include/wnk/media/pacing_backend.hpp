// SPDX-License-Identifier: GPL-3.0-or-later
//
// A MediaBackend with no output: it accepts any codec and takes packets only as fast as they would
// be presented (up to `lead` ahead of a clock started at the first packet), then discards them.
// It decodes nothing and shows nothing. Use it where no hardware decoder exists (host runs of an
// app, demos of the controller) so the rest of the pipeline runs at real speed; never present it as
// playback.
#pragma once

#include "wnk/media/media.hpp"

#include <chrono>
#include <mutex>

namespace wnk::media {

class PacingBackend : public MediaBackend {
 public:
    explicit PacingBackend(int64_t lead_us = 500000) : lead_(lead_us) {}
    const char *name() const override { return "pacing (no output)"; }
    BackendCaps caps() const override {
        BackendCaps c;
        c.video_codecs = {CodecId::H264, CodecId::Hevc, CodecId::Vp9, CodecId::Av1};
        c.audio_codecs = {CodecId::PcmS16, CodecId::Mp3, CodecId::Opus};
        return c;
    }
    void set_event_callback(EventFn) override {}
    bool load(const StreamFormat &, std::string *) override {
        std::lock_guard<std::mutex> lock(mutex_);
        base_ = kNoTime;
        return true;
    }
    Feed feed(const Packet &p) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (p.pts_us == kNoTime) return Feed::Accepted;
        const auto now = std::chrono::steady_clock::now();
        if (base_ == kNoTime) {
            base_ = p.pts_us;
            start_ = now;
        }
        const int64_t presented = base_ + std::chrono::duration_cast<std::chrono::microseconds>(now - start_).count();
        return p.pts_us > presented + lead_ ? Feed::Full : Feed::Accepted;
    }
    bool flush(std::string *) override {
        std::lock_guard<std::mutex> lock(mutex_);
        base_ = kNoTime;
        return true;
    }
    void unload() override {}
    void set_area(const Rect &) override {}

 private:
    std::mutex mutex_;
    int64_t lead_;
    int64_t base_ = kNoTime;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace wnk::media
