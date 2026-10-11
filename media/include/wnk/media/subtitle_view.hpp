// SPDX-License-Identifier: GPL-3.0-or-later
//
// SubtitleView: draws the cues a PlaybackController reports (or any cue provider) over the video.
//
//   text     wrapped and centred at the bottom of the video rectangle, in the theme's bold font at a
//            size relative to the video height, white with a dark outline (or on a dark box).
//   bitmaps  PGS/DVD/DVB images mapped from their canvas onto the video rectangle (the canvas is the
//            coded frame, so an anamorphic DVD canvas stretches with its picture). Each bitmap is
//            uploaded once while its cue shows and released when it no longer does.
//
// Lives in wnk_media_ui (ui + media) so that ui/ stays free of media types. Main thread only.
#pragma once

#include "wnk/media/media.hpp"
#include "wnk/render/renderer.hpp"
#include "wnk/ui/view.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wnk::media {

class SubtitleView : public ui::View {
 public:
    using Cues = std::vector<std::shared_ptr<const SubtitleCue>>;
    enum class TextStyle : uint8_t { Outline, Box };

    SubtitleView();
    ~SubtitleView() override;

    // Asked once per frame for what to show (e.g. [&] { return controller.subtitles_now(); }).
    SubtitleView &set_provider(std::function<Cues()> provider);
    // Where the picture is, in logical units; the view's own frame when unset.
    SubtitleView &set_video_rect(const Rect &rect);
    SubtitleView &set_text_style(TextStyle style);
    SubtitleView &set_text_scale(float fraction_of_video_height);  // default 0.055
    // Space kept free at the bottom of the video, as a fraction of its height (e.g. for controls).
    SubtitleView &set_bottom_margin(float fraction_of_video_height);  // default 0.07

    // Text lines laid out for the last frame (for tests and logs).
    const std::vector<std::string> &lines() const { return lines_; }
    size_t textures() const { return textures_.size(); }

    // Where a bitmap of a `canvas_w` x `canvas_h` canvas lands on `video`. Exposed for tests.
    static Rect place_bitmap(const SubtitleBitmap &b, int canvas_w, int canvas_h, const Rect &video);

 protected:
    void update(ui::Ui &ui, float dt) override;
    void draw(ui::Ui &ui) override;

 private:
    struct Texture {
        render::TextureId id = render::kNoTexture;
        std::shared_ptr<const SubtitleCue> cue;  // keeps the key alive
        bool used = false;
    };
    Rect video_rect() const;
    void release_unused();

    std::function<Cues()> provider_;
    Cues cues_;
    bool has_video_rect_ = false;
    Rect video_rect_;
    TextStyle style_ = TextStyle::Outline;
    float text_scale_ = 0.055f;
    float bottom_margin_ = 0.07f;
    std::vector<std::string> lines_;
    std::map<std::pair<const SubtitleCue *, size_t>, Texture> textures_;
    render::Renderer *renderer_ = nullptr;  // for releasing textures outside draw()
};

}  // namespace wnk::media
