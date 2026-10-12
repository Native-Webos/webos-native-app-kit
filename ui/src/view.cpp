// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/ui/view.hpp"

#include "wnk/render/renderer.hpp"
#include "wnk/ui/ui.hpp"

#include <algorithm>
#include <cmath>

namespace wnk::ui {
namespace {
ViewId g_next_id = 1;  // main thread only
}

View::View() : id_(g_next_id++) {}
View::~View() = default;

View &View::add_child(std::unique_ptr<View> child) {
    View &ref = add_child_silent(std::move(child));
    invalidate();
    return ref;
}

View &View::add_child_silent(std::unique_ptr<View> child) {
    child->parent_ = this;
    children_.push_back(std::move(child));
    return *children_.back();
}

void View::remove_child(View *child) {
    remove_child_silent(child);
    invalidate();
}

void View::remove_child_silent(View *child) {
    auto it = std::find_if(children_.begin(), children_.end(), [child](const auto &c) { return c.get() == child; });
    if (it == children_.end()) return;
    std::unique_ptr<View> owned = std::move(*it);
    children_.erase(it);
    owned->parent_ = nullptr;
    if (Ui *u = ui()) {
        u->retire(std::move(owned));
    }
}

void View::clear_children() {
    Ui *u = ui();
    std::vector<std::unique_ptr<View>> old;
    old.swap(children_);
    for (auto &c : old) {
        c->parent_ = nullptr;
        if (u != nullptr) u->retire(std::move(c));
    }
    invalidate();
}

std::unique_ptr<View> View::take_child(View *child) {
    auto it = std::find_if(children_.begin(), children_.end(), [child](const auto &c) { return c.get() == child; });
    if (it == children_.end()) return nullptr;
    std::unique_ptr<View> owned = std::move(*it);
    children_.erase(it);
    owned->parent_ = nullptr;
    invalidate();
    return owned;
}

Ui *View::ui() const {
    const View *v = this;
    while (v->parent_ != nullptr) v = v->parent_;
    return v->owner_ui_;
}

bool View::is_ancestor_of(const View *v) const {
    for (const View *p = v != nullptr ? v->parent_ : nullptr; p != nullptr; p = p->parent_) {
        if (p == this) return true;
    }
    return false;
}

View &View::set_width(Dim d) {
    width_ = d;
    invalidate();
    return *this;
}

View &View::set_height(Dim d) {
    height_ = d;
    invalidate();
    return *this;
}

View &View::set_padding(Insets p) {
    padding_ = p;
    invalidate();
    return *this;
}

View &View::set_align(Align x, Align y) {
    align_x_ = x;
    align_y_ = y;
    invalidate();
    return *this;
}

View &View::set_visible(bool v) {
    if (visible_ == v) return *this;
    visible_ = v;
    invalidate();
    return *this;
}

View &View::set_background(std::optional<render::Color> c, float radius) {
    background_ = c;
    radius_ = radius;
    return *this;
}

View &View::set_focusable(bool f) {
    focusable_ = f;
    return *this;
}

View &View::set_enabled(bool e) {
    enabled_ = e;
    return *this;
}

View &View::set_focus_scope(bool contain, bool restore) {
    scope_ = true;
    scope_contain_ = contain;
    scope_restore_ = restore;
    return *this;
}

View &View::set_focus_ring(bool on, float radius) {
    focus_ring_ = on;
    ring_radius_ = radius;
    return *this;
}

void View::invalidate() {
    if (Ui *u = ui()) u->invalidate_layout();
}

float View::extent(Dim d, Align a, float measured, float available) {
    switch (d.kind) {
        case Dim::Fixed: return d.value;
        case Dim::Fill: return available;
        case Dim::Auto: return a == Align::Stretch ? available : std::min(measured, available);
    }
    return measured;
}

Size View::measure(Ui &ui, Size available) {
    if (!visible_) return {0, 0};
    const float pw = padding_.horizontal(), ph = padding_.vertical();
    Size inner{std::max(0.0f, (width_.kind == Dim::Fixed ? width_.value : available.w) - pw),
               std::max(0.0f, (height_.kind == Dim::Fixed ? height_.value : available.h) - ph)};
    Size content;
    if (width_.kind != Dim::Fixed || height_.kind != Dim::Fixed) content = measure_content(ui, inner);
    Size out;
    out.w = width_.kind == Dim::Fixed ? width_.value : width_.kind == Dim::Fill ? available.w : content.w + pw;
    out.h = height_.kind == Dim::Fixed ? height_.value : height_.kind == Dim::Fill ? available.h : content.h + ph;
    return out;
}

void View::layout(Ui &ui, const Rect &frame) {
    frame_ = frame;
    layout_content(ui, content_rect());
}

Rect View::content_rect() const {
    return {frame_.x + padding_.left, frame_.y + padding_.top, std::max(0.0f, frame_.w - padding_.horizontal()),
            std::max(0.0f, frame_.h - padding_.vertical())};
}

Rect View::screen_rect() const { return to_screen(frame_); }

Rect View::to_screen(Rect r) const {
    r.x += translation_.x;
    r.y += translation_.y;
    for (const View *p = parent_; p != nullptr; p = p->parent_) {
        const Point s = p->scroll_offset();
        r.x += p->translation_.x - s.x;
        r.y += p->translation_.y - s.y;
    }
    return r;
}

Size View::measure_content(Ui &ui, Size available) {
    Size out;
    for (const auto &c : children_) {
        if (!c->visible_) continue;
        const Size m = c->measure(ui, available);
        out.w = std::max(out.w, m.w);
        out.h = std::max(out.h, m.h);
    }
    return out;
}

Rect View::place(Ui &ui, View &child, const Rect &area) {
    const Size m = child.measure(ui, {area.w, area.h});
    const float w = extent(child.width_, child.align_x_, m.w, area.w);
    const float h = extent(child.height_, child.align_y_, m.h, area.h);
    auto offset = [](Align a, float space) {
        switch (a) {
            case Align::Center: return space * 0.5f;
            case Align::End: return space;
            default: return 0.0f;
        }
    };
    return {area.x + offset(child.align_x_, area.w - w), area.y + offset(child.align_y_, area.h - h), w, h};
}

void View::layout_content(Ui &ui, const Rect &content) {
    for (const auto &c : children_) c->layout(ui, place(ui, *c, content));
}

bool View::on_action(Ui &, const input::ActionEvent &) { return false; }
void View::on_focus_changed(Ui &, bool) {}
void View::on_descendant_focused(Ui &, View *, View *) {}
void View::update(Ui &, float) {}
bool View::on_wheel(Ui &, float) { return false; }
bool View::on_text(Ui &, const std::string &) { return false; }
bool View::on_edit_key(Ui &, input::EditKey) { return false; }
void View::on_composition(Ui &, const std::string &) {}
void View::on_text_input_closed(Ui &) {}
void View::draw(Ui &) {}
void View::draw_over(Ui &) {}

void View::draw_tree(Ui &ui) {
    if (!visible_ || opacity_ <= 0.001f) return;
    render::Renderer &r = ui.renderer();
    const float saved_alpha = ui.alpha_;
    ui.alpha_ *= opacity_;
    const bool translated = translation_.x != 0 || translation_.y != 0;
    if (translated) r.push_translate(translation_.x, translation_.y);

    if (background_) r.fill_rect(frame_, ui.fade(*background_), radius_);
    draw(ui);
    if (!children_.empty()) {
        const bool clip = clips_children();
        if (clip) r.push_clip(frame_);
        // Scroll by whole device pixels: text is drawn on the pixel grid, images are not, so a
        // fractional offset makes text shimmer against the images around it while scrolling.
        Point s = scroll_offset();
        const float px = r.mapping().scale > 0 ? r.mapping().scale : 1;
        s = {std::round(s.x * px) / px, std::round(s.y * px) / px};
        const bool scrolled = s.x != 0 || s.y != 0;
        if (scrolled) r.push_translate(-s.x, -s.y);
        // Index loop: a child must not change the tree while drawing, but be defensive about size.
        for (size_t i = 0; i < children_.size(); ++i) children_[i]->draw_tree(ui);
        if (scrolled) r.pop_translate();
        if (clip) r.pop_clip();
    }
    draw_over(ui);

    if (translated) r.pop_translate();
    ui.alpha_ = saved_alpha;
}

}  // namespace wnk::ui
