// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/ui/collection.hpp"

#include <algorithm>
#include <cmath>

namespace wnk::ui {

CollectionView::CollectionView(Axis axis, int cross_count, Size cell, float spacing)
    : axis_(axis), cross_(cross_count < 1 ? 1 : cross_count), cell_(cell), spacing_(spacing) {
    set_focus_scope(false, true);
    scroll_.rate = 14;
}

void CollectionView::set_items(size_t count, Factory factory) {
    Ui *u = ui();
    const bool had_focus = u != nullptr && u->has_focus_within(this);
    drop_all();
    notified_index_ = static_cast<size_t>(-1);
    count_ = count;
    factory_ = std::move(factory);
    focused_index_ = count_ == 0 ? 0 : std::min(focused_index_, count_ - 1);
    scroll_.target = std::clamp(scroll_.target, 0.0f, max_scroll());
    scroll_.value = std::clamp(scroll_.value, 0.0f, max_scroll());
    invalidate();
    if (had_focus && count_ > 0) {
        // Move focus straight to the replacement cell so it never wanders out of the collection.
        if (View *c = realize_one(*u, focused_index_)) {
            ensure_visible(focused_index_, false);
            if (c->focusable() ? !u->focus(c) : !u->focus_within(c)) restore_focus_ = true;
        }
    }
}

size_t CollectionView::lines() const { return (count_ + static_cast<size_t>(cross_) - 1) / static_cast<size_t>(cross_); }

float CollectionView::line_extent() const { return (axis_ == Axis::Vertical ? cell_.h : cell_.w) + spacing_; }

float CollectionView::cross_extent() const { return (axis_ == Axis::Vertical ? cell_.w : cell_.h) + spacing_; }

float CollectionView::viewport_main() const {
    const Rect c = content_rect();
    return axis_ == Axis::Vertical ? c.h - content_padding_.vertical() : c.w - content_padding_.horizontal();
}

float CollectionView::max_scroll() const {
    if (count_ == 0) return 0;
    const float content = static_cast<float>(lines()) * line_extent() - spacing_;
    return std::max(0.0f, content - viewport_main());
}

Rect CollectionView::cell_rect(size_t index) const {
    const Rect c = content_rect();
    const float line = static_cast<float>(index / static_cast<size_t>(cross_));
    const float across = static_cast<float>(index % static_cast<size_t>(cross_));
    if (axis_ == Axis::Vertical) {
        return {c.x + content_padding_.left + across * cross_extent(), c.y + content_padding_.top + line * line_extent(),
                cell_.w, cell_.h};
    }
    return {c.x + content_padding_.left + line * line_extent(), c.y + content_padding_.top + across * cross_extent(),
            cell_.w, cell_.h};
}

Point CollectionView::scroll_offset() const {
    return axis_ == Axis::Vertical ? Point{0, scroll_.value} : Point{scroll_.value, 0};
}

View *CollectionView::cell(size_t index) const {
    auto it = live_.find(index);
    return it == live_.end() ? nullptr : it->second;
}

Size CollectionView::measure_content(Ui &, Size available) {
    const float main = count_ == 0 ? 0 : static_cast<float>(lines()) * line_extent() - spacing_;
    const float cross = static_cast<float>(cross_) * cross_extent() - spacing_;
    if (axis_ == Axis::Vertical) {
        return {cross + content_padding_.horizontal(), std::min(available.h, main + content_padding_.vertical())};
    }
    return {std::min(available.w, main + content_padding_.horizontal()), cross + content_padding_.vertical()};
}

void CollectionView::layout_content(Ui &ui, const Rect &) {
    scroll_.target = std::clamp(scroll_.target, 0.0f, max_scroll());
    scroll_.value = std::clamp(scroll_.value, 0.0f, max_scroll());
    for (auto &[i, v] : live_) v->layout(ui, cell_rect(i));
    realize(ui);
}

View *CollectionView::realize_one(Ui &ui, size_t index) {
    if (index >= count_ || !factory_) return nullptr;
    if (View *v = cell(index)) return v;
    std::unique_ptr<View> made = factory_(ui, index);
    if (!made) return nullptr;
    View &v = add_child_silent(std::move(made));
    v.layout(ui, cell_rect(index));
    live_[index] = &v;
    return &v;
}

void CollectionView::realize(Ui &ui) {
    if (frame().empty() || count_ == 0) {
        if (!live_.empty()) drop_all();
        return;
    }
    // Lines covering both where the view is and where it is scrolling to, plus one either side.
    const float le = line_extent();
    const float lo = std::min(scroll_.value, scroll_.target);
    const float hi = std::max(scroll_.value, scroll_.target) + viewport_main();
    const long last_line = static_cast<long>(lines()) - 1;
    const long first = std::clamp(static_cast<long>(std::floor(lo / le)) - 1, 0L, last_line);
    const long last = std::clamp(static_cast<long>(std::floor(hi / le)) + 1, 0L, last_line);
    const size_t first_index = static_cast<size_t>(first) * static_cast<size_t>(cross_);
    const size_t last_index = std::min(count_ - 1, static_cast<size_t>(last + 1) * static_cast<size_t>(cross_) - 1);

    View *focused = ui.focused();
    for (auto it = live_.begin(); it != live_.end();) {
        const bool keep = (it->first >= first_index && it->first <= last_index) ||
                          (focused != nullptr && (it->second == focused || it->second->is_ancestor_of(focused)));
        if (!keep) {
            remove_child_silent(it->second);
            it = live_.erase(it);
        } else {
            ++it;
        }
    }
    for (size_t i = first_index; i <= last_index; ++i) realize_one(ui, i);
}

void CollectionView::drop_all() {
    for (auto &[i, v] : live_) remove_child_silent(v);
    live_.clear();
}

void CollectionView::ensure_visible(size_t index, bool animate) {
    const float le = line_extent();
    const float cell_main = axis_ == Axis::Vertical ? cell_.h : cell_.w;
    const float start = static_cast<float>(index / static_cast<size_t>(cross_)) * le;
    const float end = start + cell_main;
    const float vp = viewport_main();
    const float peek = le * 0.5f;  // show part of the next line so the user sees there is more
    float s = scroll_.target;
    if (vp >= cell_main + 2 * peek) {
        if (end + peek > s + vp) s = end + peek - vp;
        if (start - peek < s) s = start - peek;
    } else {
        s = start;
    }
    scroll_.target = std::clamp(s, 0.0f, max_scroll());
    if (!animate) scroll_.snap(scroll_.target);
}

void CollectionView::scroll_to_index(size_t index, bool animate) {
    if (index < count_) ensure_visible(index, animate);
}

bool CollectionView::focus_index(Ui &ui, size_t index) {
    if (index >= count_) return false;
    View *c = realize_one(ui, index);
    if (c == nullptr) return false;
    ensure_visible(index, true);
    return c->focusable() ? ui.focus(c) : ui.focus_within(c);
}

bool CollectionView::on_action(Ui &ui, const input::ActionEvent &e) {
    if (e.phase == input::Phase::Release) return false;
    View *focused = ui.focused();
    long index = -1;
    for (auto &[i, v] : live_) {
        if (v == focused || v->is_ancestor_of(focused)) {
            index = static_cast<long>(i);
            break;
        }
    }
    if (index < 0) return false;

    if (e.action == input::Action::Confirm) {
        if (!on_activate) return false;
        if (e.phase == input::Phase::Press) on_activate(ui, static_cast<size_t>(index));
        return true;
    }

    const long n = static_cast<long>(count_), cross = cross_;
    const long along = index % cross;
    const bool vertical = axis_ == Axis::Vertical;
    // "Next/previous line" and "next/previous in line" in terms of arrows for this orientation.
    const input::Action prev_line = vertical ? input::Action::Up : input::Action::Left;
    const input::Action next_line = vertical ? input::Action::Down : input::Action::Right;
    const input::Action prev_in = vertical ? input::Action::Left : input::Action::Up;
    const input::Action next_in = vertical ? input::Action::Right : input::Action::Down;
    long target = -1;
    if (e.action == prev_line) {
        if (index - cross >= 0) target = index - cross;
    } else if (e.action == next_line) {
        if (index + cross < n) {
            target = index + cross;
        } else if (index / cross < (n - 1) / cross) {
            target = n - 1;  // the last line is shorter: go to its last cell
        }
    } else if (e.action == prev_in) {
        if (along > 0) target = index - 1;
    } else if (e.action == next_in) {
        if (along < cross - 1 && index + 1 < n) target = index + 1;
    } else {
        return false;
    }
    if (target < 0) return false;  // at an edge: let geometric focus leave the collection
    return focus_index(ui, static_cast<size_t>(target));
}

void CollectionView::on_descendant_focused(Ui &ui, View *child, View *) {
    for (auto &[i, v] : live_) {
        if (v != child) continue;
        const bool changed = i != notified_index_;
        focused_index_ = notified_index_ = i;
        if (ui.focus_source() != FocusSource::Pointer) ensure_visible(i, true);
        if (changed && on_focus) on_focus(ui, i);
        return;
    }
}

bool CollectionView::on_wheel(Ui &, float notches) {
    // The wheel scrolls vertically: a horizontal rail passes it on, so a page of rails scrolls.
    if (count_ == 0 || axis_ != Axis::Vertical) return false;
    // One line per notch; the focused cell may scroll out of view and stays alive until it does.
    scroll_.target = std::clamp(scroll_.target - notches * line_extent(), 0.0f, max_scroll());
    return true;
}

void CollectionView::update(Ui &ui, float dt) {
    if (restore_focus_ && !frame().empty()) {
        restore_focus_ = false;
        focus_index(ui, focused_index_);
    }
    scroll_.step(dt);
    realize(ui);
}

}  // namespace wnk::ui
