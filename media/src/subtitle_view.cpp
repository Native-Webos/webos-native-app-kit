// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/subtitle_view.hpp"

#include "wnk/ui/ui.hpp"
#include "wnk/ui/widgets.hpp"

#include <algorithm>

namespace wnk::media {

SubtitleView::SubtitleView() = default;

SubtitleView::~SubtitleView() {
    for (auto &[key, t] : textures_) t.used = false;
    release_unused();
}

SubtitleView &SubtitleView::set_provider(std::function<Cues()> provider) {
    provider_ = std::move(provider);
    return *this;
}

SubtitleView &SubtitleView::set_video_rect(const Rect &rect) {
    video_rect_ = rect;
    has_video_rect_ = true;
    return *this;
}

SubtitleView &SubtitleView::set_text_style(TextStyle style) {
    style_ = style;
    return *this;
}

SubtitleView &SubtitleView::set_text_scale(float fraction) {
    text_scale_ = std::clamp(fraction, 0.02f, 0.15f);
    return *this;
}

SubtitleView &SubtitleView::set_bottom_margin(float fraction) {
    bottom_margin_ = std::clamp(fraction, 0.0f, 0.5f);
    return *this;
}

Rect SubtitleView::video_rect() const { return has_video_rect_ ? video_rect_ : frame(); }

Rect SubtitleView::place_bitmap(const SubtitleBitmap &b, int canvas_w, int canvas_h, const Rect &video) {
    if (canvas_w <= 0 || canvas_h <= 0) return {};
    const float sx = video.w / static_cast<float>(canvas_w);
    const float sy = video.h / static_cast<float>(canvas_h);
    return {video.x + b.x * sx, video.y + b.y * sy, b.width * sx, b.height * sy};
}

void SubtitleView::update(ui::Ui &, float) {
    cues_ = provider_ ? provider_() : Cues{};
}

void SubtitleView::release_unused() {
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (!it->second.used) {
            if (renderer_ != nullptr && it->second.id != render::kNoTexture) renderer_->destroy_texture(it->second.id);
            it = textures_.erase(it);
        } else {
            it->second.used = false;
            ++it;
        }
    }
}

void SubtitleView::draw(ui::Ui &ui) {
    render::Renderer &r = ui.renderer();
    renderer_ = &r;
    const Rect video = video_rect();
    lines_.clear();

    // Bitmaps first; text cues of the same track rarely coexist with them.
    std::string text;
    for (const auto &cue : cues_) {
        for (size_t i = 0; i < cue->bitmaps.size(); ++i) {
            const SubtitleBitmap &b = cue->bitmaps[i];
            if (b.width <= 0 || b.height <= 0) continue;
            Texture &t = textures_[{cue.get(), i}];
            if (t.id == render::kNoTexture) {
                t.id = r.create_texture(b.width, b.height, b.rgba.data());
                t.cue = cue;
            }
            t.used = true;
            if (t.id != render::kNoTexture) r.draw_image(t.id, place_bitmap(b, cue->canvas_w, cue->canvas_h, video));
        }
        if (!cue->text.empty()) text += (text.empty() ? "" : "\n") + cue->text;
    }
    release_unused();
    if (text.empty() || video.h <= 0) return;

    render::TextStyle ts;
    ts.font = ui.theme().bold != render::kNoFont ? ui.theme().bold : ui.theme().regular;
    ts.size = video.h * text_scale_;
    const float max_width = video.w * 0.84f;
    lines_ = ui::Label::wrap(r, ts, text, max_width, 6);
    if (lines_.empty()) return;

    const float line_h = r.line_height(ts);
    const float ascent = r.ascent(ts);
    const float outline = std::max(2.0f, ts.size * 0.06f);
    const float bottom = video.y + video.h * (1.0f - bottom_margin_);
    float top = bottom - line_h * static_cast<float>(lines_.size());
    for (const std::string &line : lines_) {
        const float w = r.measure_text(ts, line);
        const float x = video.x + (video.w - w) / 2;
        const float baseline = top + ascent;
        if (style_ == TextStyle::Box) {
            const float pad = ts.size * 0.25f;
            r.fill_rect({x - pad, top, w + 2 * pad, line_h}, render::kBlack.with_alpha(0.65f), pad / 2);
        } else {
            // An outline from eight offset copies: no outline support needed in the renderer.
            render::TextStyle dark = ts;
            dark.color = render::kBlack.with_alpha(0.85f);
            static constexpr float kDirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0.7f, 0.7f}, {-0.7f, 0.7f},
                                                  {0.7f, -0.7f}, {-0.7f, -0.7f}};
            for (const auto &d : kDirs) r.draw_text(dark, {x + d[0] * outline, baseline + d[1] * outline}, line);
        }
        r.draw_text(ts, {x, baseline}, line);
        top += line_h;
    }
}

}  // namespace wnk::media
