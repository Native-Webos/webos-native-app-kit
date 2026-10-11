// SPDX-License-Identifier: GPL-3.0-or-later
//
// CollectionView: a virtualised list or grid of equally sized cells.
//
// Only the cells inside the viewport (plus one line either side) exist as views; a factory creates
// them on demand and the collection retires them when they scroll away, so a 10 000-item grid costs
// the same as a 20-item one. Arrows inside the collection are handled here by index, so moving
// never depends on whether the next cell has been created yet; at an edge the arrow is left
// unhandled and geometric focus takes over (e.g. Left from the first column to a sidebar). The view
// scrolls smoothly to keep the focused cell visible with a little of the next one showing.
//
//   vertical, columns = 1   → a list           vertical, columns = 6 → a poster grid
//   horizontal, rows = 1    → a rail/row        (cells flow along the axis, wrapping across)
#pragma once

#include "wnk/ui/anim.hpp"
#include "wnk/ui/widgets.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>

namespace wnk::ui {

class CollectionView : public View {
 public:
    using Factory = std::function<std::unique_ptr<View>(Ui &, size_t index)>;

    CollectionView(Axis axis, int cross_count, Size cell, float spacing = 24);

    // Replaces the data. Existing cells are dropped; focus (if inside) stays on the same index, clamped.
    void set_items(size_t count, Factory factory);
    size_t count() const { return count_; }

    std::function<void(Ui &, size_t)> on_activate;  // OK on a cell
    std::function<void(Ui &, size_t)> on_focus;     // focused cell changed

    size_t focused_index() const { return focused_index_; }
    // Scrolls to and focuses cell `index` (creating it if needed).
    bool focus_index(Ui &ui, size_t index);
    void scroll_to_index(size_t index, bool animate = true);
    View *cell(size_t index) const;  // live cell or nullptr
    size_t live_count() const { return live_.size(); }
    float scroll_position() const { return scroll_.value; }
    float scroll_target() const { return scroll_.target; }

    // Space between the viewport edge and the cells, so focus rings are not clipped.
    void set_content_padding(Insets p) { content_padding_ = p; invalidate(); }

    Point scroll_offset() const override;
    bool clips_children() const override { return true; }

 protected:
    Size measure_content(Ui &ui, Size available) override;
    void layout_content(Ui &ui, const Rect &content) override;
    bool on_action(Ui &ui, const input::ActionEvent &e) override;
    void on_descendant_focused(Ui &ui, View *child, View *focused) override;
    void update(Ui &ui, float dt) override;

 private:
    size_t lines() const;
    float line_extent() const;   // cell size + spacing along the axis
    float cross_extent() const;  // cell size + spacing across
    float viewport_main() const;
    float max_scroll() const;
    Rect cell_rect(size_t index) const;
    void ensure_visible(size_t index, bool animate);
    void realize(Ui &ui);           // create/retire cells for the current window
    View *realize_one(Ui &ui, size_t index);
    void drop_all();

    Axis axis_;
    int cross_;
    Size cell_;
    float spacing_;
    Insets content_padding_ = Insets::all(16);
    size_t count_ = 0;
    Factory factory_;
    std::map<size_t, View *> live_;
    size_t focused_index_ = 0;
    size_t notified_index_ = static_cast<size_t>(-1);  // last index reported through on_focus
    Smoothed scroll_;
    bool restore_focus_ = false;
};

}  // namespace wnk::ui
