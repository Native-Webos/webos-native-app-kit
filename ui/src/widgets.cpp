// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/ui/widgets.hpp"

#include <algorithm>
#include <cmath>

namespace wnk::ui {
namespace {

constexpr float kHuge = 1e7f;

float main_of(Axis a, Size s) { return a == Axis::Horizontal ? s.w : s.h; }
float cross_of(Axis a, Size s) { return a == Axis::Horizontal ? s.h : s.w; }

float align_offset(Align a, float space) {
    switch (a) {
        case Align::Center: return space * 0.5f;
        case Align::End: return space;
        default: return 0;
    }
}

// Byte offsets of UTF-8 code point starts in s[from, to).
size_t prev_boundary(const std::string &s, size_t pos) {
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) --pos;
    return pos;
}

size_t next_boundary(const std::string &s, size_t pos) {
    if (pos >= s.size()) return s.size();
    ++pos;
    while (pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) ++pos;
    return pos;
}

std::string trim_right(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

const char *const kEllipsis = "\xE2\x80\xA6";  // U+2026

}  // namespace

// ---- Box ------------------------------------------------------------------------------------------

Box &Box::set_gap(float g) {
    gap_ = g;
    invalidate();
    return *this;
}

Box &Box::set_justify(Align a) {
    justify_ = a;
    invalidate();
    return *this;
}

Size Box::measure_content(Ui &ui, Size available) {
    float main = 0, cross = 0;
    int n = 0;
    for (const auto &c : children()) {
        if (!c->visible()) continue;
        const Size m = c->measure(ui, available);
        main += main_of(axis_, m);
        cross = std::max(cross, cross_of(axis_, m));
        ++n;
    }
    if (n > 1) main += gap_ * static_cast<float>(n - 1);
    return axis_ == Axis::Horizontal ? Size{main, cross} : Size{cross, main};
}

void Box::layout_content(Ui &ui, const Rect &content) {
    const bool horiz = axis_ == Axis::Horizontal;
    const float main_len = horiz ? content.w : content.h;
    const float cross_len = horiz ? content.h : content.w;
    struct Item {
        View *v;
        float main = 0;
        float weight = 0;
    };
    std::vector<Item> items;
    float used = 0, weights = 0;
    for (const auto &c : children()) {
        if (!c->visible()) {
            c->layout(ui, {content.x, content.y, 0, 0});
            continue;
        }
        const Dim d = horiz ? c->width() : c->height();
        Item it{c.get()};
        if (d.kind == Dim::Fill) {
            it.weight = d.value > 0 ? d.value : 1;
            weights += it.weight;
        } else {
            const Size m = c->measure(ui, {content.w, content.h});
            it.main = main_of(axis_, m);
            used += it.main;
        }
        items.push_back(it);
    }
    if (items.empty()) return;
    used += gap_ * static_cast<float>(items.size() - 1);
    const float free = std::max(0.0f, main_len - used);
    float pos = 0, gap = gap_;
    if (weights > 0) {
        for (auto &it : items) {
            if (it.weight > 0) it.main = free * it.weight / weights;
        }
    } else if (justify_ == Align::Stretch && items.size() > 1) {
        gap += free / static_cast<float>(items.size() - 1);  // space between
    } else {
        pos = align_offset(justify_, free);
    }
    for (auto &it : items) {
        View &v = *it.v;
        const Size avail = horiz ? Size{it.main, cross_len} : Size{cross_len, it.main};
        const Size m = v.measure(ui, avail);
        const Align ca = horiz ? v.align_y() : v.align_x();
        const float cs = extent(horiz ? v.height() : v.width(), ca, cross_of(axis_, m), cross_len);
        const float co = align_offset(ca, cross_len - cs);
        if (horiz) {
            v.layout(ui, {content.x + pos, content.y + co, it.main, cs});
        } else {
            v.layout(ui, {content.x + co, content.y + pos, cs, it.main});
        }
        pos += it.main + gap;
    }
}

// ---- Grid -----------------------------------------------------------------------------------------

Grid &Grid::set_gaps(float x, float y) {
    gap_x_ = x;
    gap_y_ = y;
    invalidate();
    return *this;
}

Size Grid::measure_content(Ui &ui, Size available) {
    const float cw = std::max(0.0f, (available.w - gap_x_ * static_cast<float>(columns_ - 1)) / columns_);
    float h = 0, row_h = 0;
    int col = 0, rows = 0;
    for (const auto &c : children()) {
        if (!c->visible()) continue;
        row_h = std::max(row_h, c->measure(ui, {cw, available.h}).h);
        if (++col == columns_) {
            h += row_h;
            row_h = 0;
            col = 0;
            ++rows;
        }
    }
    if (col > 0) {
        h += row_h;
        ++rows;
    }
    if (rows > 1) h += gap_y_ * static_cast<float>(rows - 1);
    return {available.w, h};
}

void Grid::layout_content(Ui &ui, const Rect &content) {
    const float cw = std::max(0.0f, (content.w - gap_x_ * static_cast<float>(columns_ - 1)) / columns_);
    std::vector<View *> visible;
    for (const auto &c : children()) {
        if (c->visible()) visible.push_back(c.get());
    }
    float y = content.y;
    for (size_t row = 0; row * columns_ < visible.size(); ++row) {
        const size_t first = row * columns_, last = std::min(visible.size(), first + columns_);
        float row_h = 0;
        for (size_t i = first; i < last; ++i) row_h = std::max(row_h, visible[i]->measure(ui, {cw, content.h}).h);
        for (size_t i = first; i < last; ++i) {
            const Rect cell{content.x + static_cast<float>(i - first) * (cw + gap_x_), y, cw, row_h};
            visible[i]->layout(ui, place(ui, *visible[i], cell));
        }
        y += row_h + gap_y_;
    }
}

// ---- ScrollView -----------------------------------------------------------------------------------

ScrollView::ScrollView() { scroll_.rate = 16; }

void ScrollView::scroll_to(float y, bool animate) {
    scroll_.target = std::clamp(y, 0.0f, max_scroll_);
    if (!animate) scroll_.snap(scroll_.target);
}

Size ScrollView::measure_content(Ui &ui, Size available) {
    Size out;
    for (const auto &c : children()) {
        const Size m = c->measure(ui, {available.w, kHuge});
        out.w = std::max(out.w, m.w);
        out.h = std::max(out.h, m.h);
    }
    out.h = std::min(out.h, available.h);
    return out;
}

void ScrollView::layout_content(Ui &ui, const Rect &content) {
    float content_h = 0;
    for (const auto &c : children()) {
        const Size m = c->measure(ui, {content.w, kHuge});
        const float h = std::max(m.h, content.h);
        c->layout(ui, {content.x, content.y, content.w, h});
        content_h = std::max(content_h, m.h);
    }
    max_scroll_ = std::max(0.0f, content_h - content.h);
    scroll_.target = std::clamp(scroll_.target, 0.0f, max_scroll_);
    scroll_.value = std::clamp(scroll_.value, 0.0f, max_scroll_);
}

void ScrollView::on_descendant_focused(Ui &ui, View *, View *focused) {
    if (ui.focus_source() == FocusSource::Pointer) return;  // hovering must not move the content
    const Rect c = content_rect();
    // Position of the focused view inside the scrolled content (nested scrolling is not combined).
    const Rect f = focused->frame();
    const float top = f.y - c.y, bottom = f.bottom() - c.y;
    float s = scroll_.target;
    if (bottom + margin > s + c.h) s = bottom + margin - c.h;
    if (top - margin < s) s = top - margin;
    scroll_to(s);
}

void ScrollView::update(Ui &, float dt) { scroll_.step(dt); }

bool ScrollView::on_wheel(Ui &, float notches) {
    if (max_scroll_ <= 0) return false;  // nothing to scroll: let an outer scroller have it
    scroll_to(scroll_.target - notches * 160);
    return true;
}

// ---- Label ----------------------------------------------------------------------------------------

Label::Label(std::string text, TextRole role) : text_(std::move(text)), role_(role) {
    if (role_ == TextRole::Title) bold_ = true;
}

Label &Label::set_text(std::string text) {
    if (text == text_) return *this;
    text_ = std::move(text);
    invalidate();
    return *this;
}

Label &Label::set_role(TextRole role) {
    role_ = role;
    invalidate();
    return *this;
}

Label &Label::set_bold(bool bold) {
    if (bold_ != bold) {
        bold_ = bold;
        invalidate();
    }
    return *this;
}

Label &Label::set_color(render::Color c) {
    color_ = c;  // colour does not change size: no relayout
    return *this;
}

Label &Label::set_muted(bool muted) {
    muted_ = muted;
    return *this;
}

Label &Label::set_max_lines(int n) {
    max_lines_ = std::max(1, n);
    invalidate();
    return *this;
}

Label &Label::set_text_align(Align a) {
    text_align_ = a;
    return *this;
}

Label &Label::set_font_size(float size) {
    size_override_ = size;
    invalidate();
    return *this;
}

render::TextStyle Label::style(const Ui &ui) const {
    const Theme &t = ui.theme();
    render::TextStyle s;
    s.font = bold_ ? t.bold : t.regular;
    s.size = size_override_ > 0 ? size_override_
             : role_ == TextRole::Title ? t.title_size
             : role_ == TextRole::Small ? t.small_size
                                        : t.text_size;
    s.color = color_ ? *color_ : muted_ ? t.text_muted : t.text;
    return s;
}

std::vector<std::string> Label::wrap(const render::Renderer &r, const render::TextStyle &style, const std::string &text,
                                     float width, int max_lines) {
    std::vector<std::string> out;
    if (text.empty() || width <= 0 || max_lines < 1) return out;
    auto fits = [&](const std::string &s) { return r.measure_text(style, s) <= width; };
    auto ellipsize = [&](std::string s) {
        s = trim_right(std::move(s));
        while (!s.empty() && !fits(s + kEllipsis)) s = trim_right(s.substr(0, prev_boundary(s, s.size())));
        return s + kEllipsis;
    };

    size_t pos = 0;
    const size_t n = text.size();
    while (pos < n && static_cast<int>(out.size()) < max_lines) {
        while (pos < n && text[pos] == ' ') ++pos;
        if (pos >= n) break;
        const bool last = static_cast<int>(out.size()) + 1 == max_lines;
        const size_t nl = text.find('\n', pos);
        const size_t para_end = nl == std::string::npos ? n : nl;

        // Longest run of whole words from pos that fits.
        size_t end = pos;  // exclusive end of the accepted text
        size_t scan = pos;
        while (scan < para_end) {
            size_t word_end = text.find(' ', scan);
            if (word_end == std::string::npos || word_end > para_end) word_end = para_end;
            if (!fits(text.substr(pos, word_end - pos))) break;
            end = word_end;
            scan = word_end;
            while (scan < para_end && text[scan] == ' ') ++scan;
        }
        const bool broken = end == pos;
        if (broken) {
            // A single word wider than the line: break it at a code point boundary.
            size_t cut = next_boundary(text, pos);
            while (cut < para_end && fits(text.substr(pos, next_boundary(text, cut) - pos))) cut = next_boundary(text, cut);
            end = cut;
        }

        const bool more = end < para_end || (para_end < n && text.find_first_not_of(" \n", para_end) != std::string::npos);
        if (last && more) {
            // Whatever remains of this paragraph goes on the last line, shortened with an ellipsis.
            // Nothing past the next word can fit (end is the longest run of whole words that does),
            // so shorten from there: the rest of a long paragraph would be measured once per code point.
            size_t cut = end;
            if (broken) {
                cut = std::min(next_boundary(text, end), para_end);
            } else {
                while (cut < para_end && text[cut] == ' ') ++cut;
                cut = std::min(text.find(' ', cut), para_end);
            }
            out.push_back(ellipsize(text.substr(pos, cut - pos)));
            break;
        }
        out.push_back(trim_right(text.substr(pos, end - pos)));
        pos = end;
        if (pos >= para_end && para_end < n) pos = para_end + 1;  // consume the newline
    }
    return out;
}

Size Label::measure_content(Ui &ui, Size available) {
    const render::Renderer &r = ui.renderer();
    const render::TextStyle s = style(ui);
    const float lh = r.line_height(s);
    if (max_lines_ == 1 && text_.find('\n') == std::string::npos) {
        const float w = r.measure_text(s, text_);
        return {std::min(w, available.w), lh};
    }
    const auto lines = wrap(r, s, text_, available.w, max_lines_);
    float w = 0;
    for (const auto &l : lines) w = std::max(w, r.measure_text(s, l));
    return {w, lh * static_cast<float>(std::max<size_t>(1, lines.size()))};
}

void Label::layout_content(Ui &ui, const Rect &content) {
    const render::Renderer &r = ui.renderer();
    const render::TextStyle s = style(ui);
    if (max_lines_ == 1 && text_.find('\n') == std::string::npos && r.measure_text(s, text_) <= content.w) {
        lines_.assign(1, text_);
    } else {
        lines_ = wrap(r, s, text_, content.w, max_lines_);
    }
}

void Label::draw(Ui &ui) {
    if (lines_.empty()) return;
    render::Renderer &r = ui.renderer();
    render::TextStyle s = style(ui);
    s.color = ui.fade(s.color);
    const Rect c = content_rect();
    const float lh = r.line_height(s);
    float baseline = c.y + r.ascent(s);
    for (const auto &line : lines_) {
        float x = c.x;
        if (text_align_ != Align::Start) x += align_offset(text_align_, c.w - r.measure_text(s, line));
        r.draw_text(s, {x, baseline}, line);
        baseline += lh;
    }
}

// ---- ImageView ------------------------------------------------------------------------------------

ImageView::ImageView(std::string url) : url_(std::move(url)) {}

ImageView::~ImageView() { cancel(); }

ImageView &ImageView::set_url(std::string url) {
    if (url == url_) return *this;
    cancel();
    url_ = std::move(url);
    state_ = State::Empty;
    key_.clear();
    reloads_ = 0;
    return *this;
}

ImageView &ImageView::set_region(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) x = y = w = h = 0;
    const bool reload = (w > 0) != (region_w_ > 0);  // the image is requested at another size
    region_x_ = x, region_y_ = y, region_w_ = w, region_h_ = h;
    if (reload && state_ != State::Empty) {
        cancel();
        state_ = State::Empty;
        key_.clear();
        reloads_ = 0;
    }
    return *this;
}

ImageView &ImageView::set_fit(ImageFit fit) {
    fit_ = fit;
    return *this;
}

ImageView &ImageView::set_animate(bool on) {
    animate_ = on;
    if (!on) frame_ = 0, frame_ms_ = 0;
    return *this;
}

void ImageView::cancel() {
    if (ticket_ != 0 && source_ != nullptr) source_->cancel(ticket_);
    ticket_ = 0;
}

void ImageView::start(Ui &ui) {
    source_ = ui.images();
    if (source_ == nullptr || url_.empty()) return;
    const Rect f = frame();
    // Cover crops one dimension, so ask for the larger side in both directions.
    const int big = static_cast<int>(std::ceil(std::max(f.w, f.h)));
    // A region is in the original's pixels: load it whole.
    const int w = region_w_ > 0 ? 0 : fit_ == ImageFit::Cover ? big : static_cast<int>(std::ceil(f.w));
    const int h = region_w_ > 0 ? 0 : fit_ == ImageFit::Cover ? big : static_cast<int>(std::ceil(f.h));
    state_ = State::Loading;
    // `this` is safe: the destructor cancels the ticket and the source never calls back after that.
    ticket_ = source_->load(url_, w, h, [this](ImageSource::Loaded l) {
        ticket_ = 0;
        if (l.ok) {
            state_ = State::Ready;
            key_ = std::move(l.key);
            img_w_ = l.width;
            img_h_ = l.height;
            delays_ = std::move(l.delays_ms);
            loops_ = l.loops;
            played_ = 0;
            frame_ = 0;
            frame_ms_ = 0;
        } else {
            state_ = State::Failed;
        }
    });
}

void ImageView::update(Ui &ui, float dt) {
    if (state_ == State::Empty && !url_.empty() && !frame().empty() && ui.images() != nullptr && reloads_ < 3) start(ui);
    if (state_ != State::Ready || !animate_ || delays_.size() < 2 || (loops_ > 0 && played_ >= loops_)) return;
    frame_ms_ += dt * 1000.0f;
    // A slow frame (a stall, the idle probe) skips ahead rather than playing the backlog quickly.
    for (int guard = 0; guard < static_cast<int>(delays_.size()) && frame_ms_ >= static_cast<float>(delays_[frame_]); ++guard) {
        frame_ms_ -= static_cast<float>(delays_[frame_]);
        if (frame_ + 1 < delays_.size()) {
            ++frame_;
        } else if (loops_ > 0 && ++played_ >= loops_) {
            frame_ms_ = 0;  // stays on the last frame, as browsers do
            break;
        } else {
            frame_ = 0;
        }
    }
    if (frame_ms_ > static_cast<float>(delays_[frame_])) frame_ms_ = 0;
}

void ImageView::draw(Ui &ui) {
    render::Renderer &r = ui.renderer();
    const Rect f = frame();
    if (state_ == State::Ready && source_ != nullptr) {
        const render::TextureId tex = frame_ == 0 ? source_->texture(key_) : source_->texture(key_, frame_);
        if (tex != render::kNoTexture && img_w_ > 0 && img_h_ > 0) {
            // The part of the texture shown as "the image": all of it, or the region.
            Rect src{0, 0, 1, 1};
            if (region_w_ > 0) {
                src = {static_cast<float>(region_x_) / static_cast<float>(img_w_), static_cast<float>(region_y_) / static_cast<float>(img_h_),
                       static_cast<float>(region_w_) / static_cast<float>(img_w_), static_cast<float>(region_h_) / static_cast<float>(img_h_)};
            }
            const float ia = (src.w * static_cast<float>(img_w_)) / (src.h * static_cast<float>(img_h_));
            const float fa = f.w / f.h;
            Rect dst = f, uv{0, 0, 1, 1};
            if (fit_ == ImageFit::Cover) {
                if (ia > fa) {
                    uv.w = fa / ia;
                    uv.x = (1 - uv.w) * 0.5f;
                } else {
                    uv.h = ia / fa;
                    uv.y = (1 - uv.h) * 0.5f;
                }
            } else if (ia > fa) {
                dst.h = f.w / ia;
                dst.y += (f.h - dst.h) * 0.5f;
            } else {
                dst.w = f.h * ia;
                dst.x += (f.w - dst.w) * 0.5f;
            }
            const Rect in_src{src.x + uv.x * src.w, src.y + uv.y * src.h, uv.w * src.w, uv.h * src.h};
            r.draw_image(tex, dst, ui.fade(render::kWhite), in_src);
            return;
        }
        // Pixels evicted from memory before upload: load again (bounded).
        state_ = State::Empty;
        ++reloads_;
    }
    const render::Color base = ui.theme().placeholder;
    float a = 0.55f;
    if (state_ == State::Loading || state_ == State::Empty) {
        a = 0.45f + 0.35f * (0.5f + 0.5f * std::sin(static_cast<float>(ui.now_ms()) * 0.005f + f.x * 0.01f));
    }
    r.fill_rect(f, ui.fade(base.with_alpha(base.a * a)), radius_);
}

// ---- Button ---------------------------------------------------------------------------------------

Button::Button(std::string text, ClickHandler on_click) : text_(std::move(text)), on_click_(std::move(on_click)) {
    set_focusable(true);
    set_padding(Insets::xy(40, 18));
    set_align(Align::Start, Align::Start);
    radius_ = 14;
}

Button &Button::set_text(std::string text) {
    text_ = std::move(text);
    invalidate();
    return *this;
}

Button &Button::set_on_click(ClickHandler h) {
    on_click_ = std::move(h);
    return *this;
}

std::string Button::accessible_text() const {
    if (!accessible_label.empty()) return accessible_label;
    return enabled() ? text_ + ", button" : text_ + ", button, unavailable";
}

bool Button::click(Ui &ui) {
    if (!enabled() || !on_click_) return false;
    press_.start(1, 0, ui.now_ms(), 200);
    // Copy: the handler may destroy or rebind this button.
    ClickHandler h = on_click_;
    h(ui);
    return true;
}

Size Button::measure_content(Ui &ui, Size) {
    const render::TextStyle s{ui.theme().bold, ui.theme().text_size, ui.theme().text};
    return {std::max(120.0f, ui.renderer().measure_text(s, text_)), ui.renderer().line_height(s)};
}

bool Button::on_action(Ui &ui, const input::ActionEvent &e) {
    // Without a handler OK bubbles up (e.g. to a CollectionView's on_activate).
    if (e.action != input::Action::Confirm || !enabled() || !on_click_) return false;
    if (e.phase == input::Phase::Press) click(ui);
    return true;
}

void Button::draw(Ui &ui) {
    render::Renderer &r = ui.renderer();
    const Theme &t = ui.theme();
    const bool focused = ui.is_focused(this);
    const Rect f = frame();
    r.fill_rect(f, ui.fade(!enabled() ? t.surface : focused ? t.raised_focused : t.raised), radius_);
    const float flash = press_.duration_ms > 0 ? press_.value(ui.now_ms()) : 0;
    if (flash > 0.01f) r.fill_rect(f, ui.fade(render::kWhite.with_alpha(0.3f * flash)), radius_);
    const render::TextStyle s{t.bold, t.text_size, ui.fade(enabled() ? t.text : t.text_disabled)};
    const float w = r.measure_text(s, text_);
    const float lh = r.line_height(s);
    r.draw_text(s, {f.cx() - w * 0.5f, f.y + (f.h - lh) * 0.5f + r.ascent(s)}, text_);
}

// ---- Card -----------------------------------------------------------------------------------------

Card::Card(std::string title, std::string image_url, std::string subtitle) {
    set_focusable(true);
    radius_ = 12;
    image_ = &add<ImageView>(std::move(image_url));
    image_->set_background(std::nullopt, 12);
    title_ = &add<Label>(std::move(title), TextRole::Small);
    title_->set_muted(true);
    subtitle_ = &add<Label>(std::move(subtitle), TextRole::Small);
    subtitle_->set_muted(true).set_font_size(20);
    subtitle_->set_visible(!subtitle_->text().empty());
}

std::string Card::accessible_text() const {
    if (!accessible_label.empty()) return accessible_label;
    std::string t = title_->text();
    if (subtitle_ != nullptr && !subtitle_->text().empty()) t += ", " + subtitle_->text();
    return t;
}

Card &Card::set_on_click(ClickHandler h) {
    on_click_ = std::move(h);
    return *this;
}

Size Card::measure_content(Ui &ui, Size available) {
    const float w = available.w;
    float h = w * image_aspect + 12 + title_->measure(ui, {w, kHuge}).h;
    if (subtitle_->visible()) h += subtitle_->measure(ui, {w, kHuge}).h;
    return {w, h};
}

void Card::layout_content(Ui &ui, const Rect &c) {
    const float ih = std::min(c.h, c.w * image_aspect);
    image_->layout(ui, {c.x, c.y, c.w, ih});
    float y = c.y + ih + 12;
    const float th = title_->measure(ui, {c.w, kHuge}).h;
    title_->layout(ui, {c.x, y, c.w, th});
    y += th;
    if (subtitle_->visible()) subtitle_->layout(ui, {c.x, y, c.w, subtitle_->measure(ui, {c.w, kHuge}).h});
}

Rect Card::focus_bounds() const { return image_->frame(); }

bool Card::on_action(Ui &ui, const input::ActionEvent &e) {
    if (e.action != input::Action::Confirm || !on_click_) return false;
    if (e.phase == input::Phase::Press) {
        ClickHandler h = on_click_;
        h(ui);
    }
    return true;
}

void Card::on_focus_changed(Ui &, bool focused) { title_->set_muted(!focused); }

// ---- Spinner --------------------------------------------------------------------------------------

Spinner::Spinner(float size) {
    set_size(size, size);
    set_align(Align::Center, Align::Center);
}

void Spinner::draw(Ui &ui) {
    render::Renderer &r = ui.renderer();
    const Rect f = frame();
    const float size = std::min(f.w, f.h);
    const float ring = size * 0.38f, dot = size * 0.09f;
    const int kDots = 8;
    const float head = std::fmod(static_cast<float>(ui.now_ms()) / 90.0f, static_cast<float>(kDots));
    for (int i = 0; i < kDots; ++i) {
        const float angle = static_cast<float>(i) * 6.2831853f / kDots - 1.5707963f;
        float behind = head - static_cast<float>(i);
        if (behind < 0) behind += kDots;
        const float a = std::max(0.15f, 1.0f - behind / kDots);
        const float x = f.cx() + std::cos(angle) * ring, y = f.cy() + std::sin(angle) * ring;
        r.fill_rect({x - dot, y - dot, dot * 2, dot * 2}, ui.fade(ui.theme().accent.with_alpha(a)), dot);
    }
}

// ---- ProgressBar ----------------------------------------------------------------------------------

ProgressBar::ProgressBar() {
    value_.rate = 10;
    set_height(Dim::fixed(12));
}

ProgressBar &ProgressBar::set_value(float v) {
    value_.target = std::clamp(v, 0.0f, 1.0f);
    indeterminate_ = false;
    return *this;
}

ProgressBar &ProgressBar::set_indeterminate(bool on) {
    indeterminate_ = on;
    return *this;
}

Size ProgressBar::measure_content(Ui &, Size available) { return {available.w, 12}; }

void ProgressBar::update(Ui &, float dt) { value_.step(dt); }

void ProgressBar::draw(Ui &ui) {
    render::Renderer &r = ui.renderer();
    const Theme &t = ui.theme();
    const Rect f = frame();
    const float rad = f.h * 0.5f;
    r.fill_rect(f, ui.fade(t.raised), rad);
    if (indeterminate_) {
        const float phase = std::fmod(static_cast<float>(ui.now_ms()) / 1400.0f, 1.0f);
        const float seg = f.w * 0.3f;
        const float x = f.x - seg + (f.w + seg) * phase;
        r.push_clip(f);
        r.fill_rect({x, f.y, seg, f.h}, ui.fade(t.accent), rad);
        r.pop_clip();
    } else if (value_.value > 0) {
        r.fill_rect({f.x, f.y, std::max(f.h, f.w * value_.value), f.h}, ui.fade(t.accent), rad);
    }
}

// ---- GlView ---------------------------------------------------------------------------------------

GlView::GlView(DrawFn on_draw) : on_draw_(std::move(on_draw)) {}

GlView &GlView::set_on_draw(DrawFn fn) {
    on_draw_ = std::move(fn);
    ++version_;
    return *this;
}

GlView &GlView::set_animated(bool animated) {
    animated_ = animated;
    ++version_;
    return *this;
}

void GlView::render_gl(const render::CustomTarget &target, double now_ms) {
    if (on_draw_) on_draw_(target, now_ms);
}

void GlView::draw(Ui &ui) {
    // An animated view differs on every frame; a static one only when its version moves.
    const uint64_t version = animated_ ? (version_ << 32) + ++frames_ : version_;
    const double now = ui.now_ms();
    ui.renderer().draw_custom(frame(), version, [this, now](const render::CustomTarget &t) { render_gl(t, now); });
}

}  // namespace wnk::ui
